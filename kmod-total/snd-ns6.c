// SPDX-License-Identifier: GPL-2.0
/*
 * snd-ns6.c — Numark NS6 USB audio/MIDI driver
 *
 * Based on reverse-engineering of the macOS proprietary driver via USB
 * capture analysis. Key findings applied:
 *   - Feedback EP 0x81 returns 0xAAAAAA always (dummy) — ignored
 *   - Fixed fractional accumulator at 44100 Hz (not feedback-driven)
 *   - Exactly 3 audio URBs in flight (not 6)
 *   - Complete init sequence matching macOS (SET_CUR on EP 0x02 + EP 0x86)
 *   - URBs run continuously from probe to disconnect (never stopped mid-stream)
 */

#include <linux/module.h>
#include <linux/usb.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/rawmidi.h>
#include <sound/initval.h>

#define DRV_NAME        "snd-ns6"
#define NS6_VID         0x15e4
#define NS6_PID         0x0079

/* Endpoints */
#define NS6_EP_PLAY     0x02    /* ISO OUT  - Audio Playback (Interface 2) */
#define NS6_EP_SYNC     0x81    /* ISO IN   - Feedback dummy (Interface 1) */
#define NS6_EP_MIDI_IN  0x83    /* Bulk IN  - MIDI from controller */
#define NS6_EP_MIDI_OUT 0x04    /* Bulk OUT - MIDI/LEDs to controller */
#define NS6_EP_WAVEFORM 0x86    /* Bulk IN  - Waveform display data */

/* PCM format — confirmed by macOS capture */
#define NS6_RATE            44100
#define NS6_CHANNELS        4
#define NS6_SAMPLE_BYTES    3       /* S24_3LE */
#define NS6_FRAME_BYTES     (NS6_CHANNELS * NS6_SAMPLE_BYTES)  /* 12 */

/* Audio URB config — matching macOS exactly */
#define NS6_PKTS_PER_URB    32      /* 32 ISO packets per URB = 4ms */
#define NS6_PKT_SIZE        156     /* wMaxPacketSize from descriptor */
#define NS6_URB_BYTES       (NS6_PKTS_PER_URB * NS6_PKT_SIZE)
#define NS6_NURBS           3       /* macOS uses exactly 3 in-flight */

/* Sync EP config (EP 0x81 — feedback is dummy, just re-submit) */
#define NS6_SYNC_PKTS_PER_URB 16
#define NS6_SYNC_PKT_SIZE     64
#define NS6_SYNC_URB_BYTES    (NS6_SYNC_PKTS_PER_URB * NS6_SYNC_PKT_SIZE)
#define NS6_SYNC_NURBS        2     /* macOS uses 2 feedback URBs */

/* MIDI */
#define NS6_MIDI_PKT_SIZE   42      /* Fixed 42-byte packets, FD-padded */
#define NS6_MIDI_NURBS      4
#define NS6_IDLE_BYTE       0xFD
#define NS6_PKT_TERM        0x00

/* Warmup: silence URBs before PCM starts (macOS does ~13) */
#define NS6_WARMUP_URBS     12

/* Waveform EP 0x86 — macOS reads 10240-byte packets at 275.6/s.
 * We submit read URBs to keep the endpoint drained so the NS6
 * doesn't stall internally. Data is discarded. */
#define NS6_WF_PKT_SIZE     10240
#define NS6_WF_NURBS        3

struct ns6_stream {
    struct urb               *urbs[NS6_NURBS];
    unsigned char            *bufs[NS6_NURBS];
    dma_addr_t                dma[NS6_NURBS];
    struct snd_pcm_substream *substream;
    spinlock_t                lock;
    unsigned int              hwptr;
    unsigned int              period_pos;
    bool                      active_usb;   /* URBs are running */
    bool                      active_pcm;   /* ALSA stream is playing */
    int                       warmup_cnt;

    /* Fixed fractional accumulator — 44100 Hz, no feedback */
    unsigned int              frac_acc;
};

struct ns6 {
    struct usb_device    *udev;
    struct snd_card      *card;
    struct snd_pcm       *pcm;
    struct ns6_stream     play;

    /* Sync EP 0x81 URBs (dummy feedback — just keep alive) */
    struct urb           *sync_urbs[NS6_SYNC_NURBS];
    unsigned char        *sync_bufs[NS6_SYNC_NURBS];
    dma_addr_t            sync_dma[NS6_SYNC_NURBS];
    bool                  sync_active;

    /* MIDI */
    struct snd_rawmidi             *rmidi;
    struct snd_rawmidi_substream   *midi_in_sub;
    struct snd_rawmidi_substream   *midi_out_sub;

    struct urb           *midi_in_urbs[NS6_MIDI_NURBS];
    uint8_t               midi_in_buf[NS6_MIDI_NURBS][NS6_MIDI_PKT_SIZE];

    #define NS6_MIDI_OUT_NURBS  4
    struct urb           *midi_out_urbs[NS6_MIDI_OUT_NURBS];
    uint8_t               midi_out_bufs[NS6_MIDI_OUT_NURBS][NS6_MIDI_PKT_SIZE];
    bool                  midi_out_busy[NS6_MIDI_OUT_NURBS];
    spinlock_t            midi_out_lock;

    uint8_t               midi_out_cache[4];
    int                   midi_out_cache_len;

    struct usb_interface  *intf1;
    struct work_struct    init_work;
    bool                  disconnecting;

    /* Waveform EP 0x86 — bulk IN, data discarded */
    struct urb           *wf_urbs[NS6_WF_NURBS];
    unsigned char        *wf_bufs[NS6_WF_NURBS];
    dma_addr_t            wf_dma[NS6_WF_NURBS];   /* <--- ADICIONE ESTA LINHA */
    bool                  wf_active;

    struct usb_endpoint_descriptor *ep_play;
    struct usb_endpoint_descriptor *ep_sync;
    struct usb_endpoint_descriptor *ep_midi_in;
    struct usb_endpoint_descriptor *ep_midi_out;
};

static struct usb_driver ns6_driver;

/* ADICIONE ESTA LINHA AQUI (Protótipo para o compilador não reclamar) */
static int ns6_start_streaming(struct ns6 *ns6);

/* ========================================================================= *
 * ENDPOINT FINDER
 * ========================================================================= */
static struct usb_endpoint_descriptor *ns6_get_ep_desc(
        struct usb_device *udev, u8 ep_addr)
{
    int i, a, e;
    for (i = 0; i < udev->actconfig->desc.bNumInterfaces; i++) {
        struct usb_interface *intf = udev->actconfig->interface[i];
        for (a = 0; a < intf->num_altsetting; a++) {
            struct usb_host_interface *alt = &intf->altsetting[a];
            for (e = 0; e < alt->desc.bNumEndpoints; e++) {
                if (alt->endpoint[e].desc.bEndpointAddress == ep_addr) {
                    if (intf->cur_altsetting != alt)
                        usb_set_interface(udev,
                            alt->desc.bInterfaceNumber,
                            alt->desc.bAlternateSetting);
                    return &alt->endpoint[e].desc;
                }
            }
        }
    }
    return NULL;
}

/* ========================================================================= *
 * INIT SEQUENCE — Clone of macOS driver (from USB capture analysis)
 *
 * Order confirmed from capture:
 *   1. Vendor bReq=86 (capability check) ×2
 *   2. SET_CONFIG (handled by USB core)
 *   3. SET_INTERFACE ×2
 *   4. CLEAR_FEATURE (halt) on EP 0x02, 0x83, 0x86
 *   5. Vendor bReq=73 GET (mode query)
 *   6. GET_CUR sample rate
 *   7. SET_CUR sample rate on EP 0x02 AND EP 0x86
 *   8. Vendor bReq=73 GET (re-check)
 *   9. Vendor bReq=73 SET wVal=0x0032 (activate DJ mode)
 *  10. SysEx init via EP 0x04
 * ========================================================================= */
static void ns6_deferred_init(struct work_struct *work)
{
    struct ns6 *ns6 = container_of(work, struct ns6, init_work);
    struct usb_device *udev = ns6->udev;
    u8 buf[4];
    u8 rate_le[3] = { 0x44, 0xAC, 0x00 };  /* 44100 Hz LE */
    int actual_len;
    int err;

    /* Dar tempo para a placa acordar após ser ligada na porta USB */
    msleep(200);

    /* 1. Vendor capability check (bReq=86, bmReqType=0xC0) */
    usb_control_msg(udev, usb_rcvctrlpipe(udev, 0),
                    86, 0xC0, 0x0000, 0, buf, 4, 1000);
    usb_control_msg(udev, usb_rcvctrlpipe(udev, 0),
                    86, 0xC0, 0x0000, 0, buf, 4, 1000);

    /* 2-4. Interfaces and halt clear — done in probe() */

    /* 5. Vendor mode query (bReq=73 GET) */
    usb_control_msg(udev, usb_rcvctrlpipe(udev, 0),
                    73, 0xC0, 0x0000, 0, buf, 4, 1000);

    /* 6. GET_CUR sample rate (bmReq=0xA2, bReq=0x81) */
    usb_control_msg(udev, usb_rcvctrlpipe(udev, 0),
                    0x81, 0xA2, 0x0100, 0, buf, 3, 1000);

    /* 7. SET_CUR sample rate on BOTH endpoints (the macOS secret!) */
    /* EP 0x86 (waveform, wIndex=134) — macOS does this 3 times */
    usb_control_msg(udev, usb_sndctrlpipe(udev, 0),
                    0x01, 0x22, 0x0100, 134, rate_le, 3, 1000);
    /* EP 0x02 (audio OUT, wIndex=2) */
    usb_control_msg(udev, usb_sndctrlpipe(udev, 0),
                    0x01, 0x22, 0x0100, 2, rate_le, 3, 1000);

    /* 8. GET_CUR sample rate on EP 0x86 (verify) */
    usb_control_msg(udev, usb_rcvctrlpipe(udev, 0),
                    0x81, 0xA2, 0x0100, 134, buf, 3, 1000);

    /* 9. Vendor mode re-check then activate */
    usb_control_msg(udev, usb_rcvctrlpipe(udev, 0),
                    73, 0xC0, 0x0000, 0, buf, 4, 1000);
    usb_control_msg(udev, usb_sndctrlpipe(udev, 0),
                    73, 0x40, 0x0032, 0, NULL, 0, 1000);

    msleep(1);  /* macOS waits ~0.5ms here */

    /* 10. SysEx init (lights/identify) via EP 0x04 */
    {
        u8 *pkt = kzalloc(NS6_MIDI_PKT_SIZE, GFP_KERNEL);
        if (pkt) {
            static const u8 sysex[] = {
                0xF0, 0x00, 0x01, 0x3F, 0x00, 0x79, 0x51, 0x00,
                0x10, 0x49, 0x01, 0x08, 0x01, 0x01, 0x08, 0x04,
                0x0C, 0x0D, 0x01, 0x0A, 0x0A, 0x05, 0x06, 0x05,
                0x0D, 0x07, 0x0E, 0x08, 0x07, 0x0D, 0xF7
            };
            memset(pkt, NS6_IDLE_BYTE, NS6_MIDI_PKT_SIZE);
            memcpy(pkt, sysex, sizeof(sysex));
            pkt[NS6_MIDI_PKT_SIZE - 1] = NS6_PKT_TERM;

            if (ns6->ep_midi_out) {
                if (usb_endpoint_xfer_int(ns6->ep_midi_out))
                    usb_interrupt_msg(udev,
                        usb_sndintpipe(udev, NS6_EP_MIDI_OUT),
                        pkt, NS6_MIDI_PKT_SIZE, &actual_len, 2000);
                else
                    usb_bulk_msg(udev,
                        usb_sndbulkpipe(udev, NS6_EP_MIDI_OUT),
                        pkt, NS6_MIDI_PKT_SIZE, &actual_len, 2000);
            }
            kfree(pkt);
        }
    }

    dev_info(&udev->dev, "NS6: Init sequence complete (macOS clone).\n");
    /* AGORA SIM: A placa já está em modo DJ. Podemos ligar os motores de áudio! */
    err = ns6_start_streaming(ns6);
    if (err)
        dev_err(&udev->dev, "NS6: streaming start err %d\n", err);
}

/* ========================================================================= *
 * AUDIO URB CALLBACK — Fixed fractional accumulator at 44100 Hz
 *
 * The NS6 feedback EP 0x81 returns 0xAAAAAA always (dummy).
 * The macOS driver uses a fixed accumulator: frac_acc += 44100 / 8000
 * producing pattern of 176/177 samples per URB (5/6 per packet).
 * ========================================================================= */
static void ns6_play_urb_cb(struct urb *urb)
{
    struct ns6 *ns6 = urb->context;
    struct ns6_stream *s = &ns6->play;
    struct snd_pcm_substream *sub;
    unsigned long flags;
    bool elapsed = false;
    unsigned int total_frames = 0, total_bytes = 0;
    int i;
    bool fill_silence;

    if (urb->status != 0 && urb->status != -EXDEV)
        return;
    if (!s->active_usb || ns6->disconnecting)
        return;

    spin_lock_irqsave(&s->lock, flags);
    sub = s->substream;

    fill_silence = (s->warmup_cnt > 0 || !s->active_pcm ||
                    !sub || !snd_pcm_running(sub));
    if (s->warmup_cnt > 0)
        s->warmup_cnt--;

    /* Fixed fractional accumulator: 44100 Hz / 8000 microframes/s
     * Produces 5 or 6 samples per packet, 176 or 177 per URB */
    for (i = 0; i < urb->number_of_packets; i++) {
        s->frac_acc += NS6_RATE;
        unsigned int pk_frames = s->frac_acc / 8000;
        s->frac_acc %= 8000;

        unsigned int pk_bytes = pk_frames * NS6_FRAME_BYTES;
        if (pk_bytes > NS6_PKT_SIZE) {
            pk_bytes = NS6_PKT_SIZE;
            pk_frames = pk_bytes / NS6_FRAME_BYTES;
        }

        urb->iso_frame_desc[i].offset = total_bytes;
        urb->iso_frame_desc[i].length = pk_bytes;
        total_frames += pk_frames;
        total_bytes  += pk_bytes;
    }

    /* Tell the USB stack the actual data size (not the max allocation) */
    urb->transfer_buffer_length = total_bytes;

    /* Fill with audio or silence */
    if (fill_silence) {
        memset(urb->transfer_buffer, 0, total_bytes);
    } else {
        unsigned int buf_frames = sub->runtime->buffer_size;
        unsigned char *dma = sub->runtime->dma_area;
        unsigned int hwptr_old = s->hwptr;

        if (hwptr_old + total_frames > buf_frames) {
            unsigned int chunk1 = buf_frames - hwptr_old;
            memcpy(urb->transfer_buffer,
                   dma + hwptr_old * NS6_FRAME_BYTES,
                   chunk1 * NS6_FRAME_BYTES);
            memcpy(urb->transfer_buffer + chunk1 * NS6_FRAME_BYTES,
                   dma,
                   (total_frames - chunk1) * NS6_FRAME_BYTES);
        } else {
            memcpy(urb->transfer_buffer,
                   dma + hwptr_old * NS6_FRAME_BYTES,
                   total_frames * NS6_FRAME_BYTES);
        }

        s->hwptr += total_frames;
        if (s->hwptr >= buf_frames)
            s->hwptr -= buf_frames;

        s->period_pos += total_frames;
        if (s->period_pos >= sub->runtime->period_size) {
            s->period_pos -= sub->runtime->period_size;
            elapsed = true;
        }
    }

    /* Debug: log a cada ~10 segundos */
    {
        static unsigned long dbg_cnt = 0;
        static unsigned long dbg_under = 0;
        if (fill_silence && s->active_pcm && sub && snd_pcm_running(sub))
            dbg_under++;
        if (++dbg_cnt % 2500 == 0)
            dev_info(&ns6->udev->dev,
                     "NS6: hwptr=%u period_pos=%u warmup=%d silence=%d under=%lu total_frames=%u\n",
                     s->hwptr, s->period_pos, s->warmup_cnt, fill_silence, dbg_under, total_frames);
    }

    spin_unlock_irqrestore(&s->lock, flags);

    if (elapsed && sub)
        snd_pcm_period_elapsed(sub);

    if (s->active_usb && !ns6->disconnecting)
        usb_submit_urb(urb, GFP_ATOMIC);
}

/* ========================================================================= *
 * SYNC EP 0x81 — Dummy feedback, just re-submit
 *
 * The NS6 returns 0xAAAAAA (uninitialized) regardless of audio state.
 * We keep the endpoint alive but ignore the data completely.
 * ========================================================================= */
static void ns6_sync_urb_cb(struct urb *urb)
{
    struct ns6 *ns6 = urb->context;

    /* Ignore feedback data — NS6 returns 0xAAAAAA always */

    if (ns6->sync_active && !ns6->disconnecting)
        usb_submit_urb(urb, GFP_ATOMIC);
}

/* ========================================================================= *
 * WAVEFORM EP 0x86 — Drain to keep NS6 internal state healthy
 *
 * The macOS driver reads 10240-byte waveform packets at 275.6/s.
 * If we don't drain this endpoint, the NS6 may stall internally,
 * causing periodic audio glitches. We read and discard the data.
 * ========================================================================= */
static void ns6_wf_urb_cb(struct urb *urb)
{
    struct ns6 *ns6 = urb->context;

    /* Discard waveform data — we don't use the displays */

    if (ns6->wf_active && !ns6->disconnecting)
        usb_submit_urb(urb, GFP_ATOMIC);
}

/* ========================================================================= *
 * MIDI
 * ========================================================================= */
static void ns6_process_midi_out(struct ns6 *ns6);

static void ns6_parse_midi(struct ns6 *ns6, const uint8_t *pkt, int len)
{
    struct snd_rawmidi_substream *sub = ns6->midi_in_sub;
    int i = 0;

    if (!sub)
        return;

    while (i < len) {
        uint8_t b = pkt[i];
        if (b == NS6_IDLE_BYTE || b == NS6_PKT_TERM)
            break;
        if (i + 2 < len) {
            uint8_t msg[3] = { pkt[i], pkt[i+1], pkt[i+2] };
            snd_rawmidi_receive(sub, msg, 3);
            i += 3;
        } else {
            break;
        }
    }
}

static void ns6_midi_in_urb_cb(struct urb *urb)
{
    struct ns6 *ns6 = urb->context;

    if (urb->status == 0 && urb->actual_length == NS6_MIDI_PKT_SIZE)
        ns6_parse_midi(ns6, urb->transfer_buffer, urb->actual_length);

    if (!ns6->disconnecting)
        usb_submit_urb(urb, GFP_ATOMIC);
}

static void ns6_midi_out_urb_cb(struct urb *urb)
{
    struct ns6 *ns6 = urb->context;
    unsigned long flags;
    int i;

    spin_lock_irqsave(&ns6->midi_out_lock, flags);
    for (i = 0; i < NS6_MIDI_OUT_NURBS; i++) {
        if (ns6->midi_out_urbs[i] == urb) {
            ns6->midi_out_busy[i] = false;
            break;
        }
    }
    spin_unlock_irqrestore(&ns6->midi_out_lock, flags);

    ns6_process_midi_out(ns6);
}

static inline int get_midi_msg_len(uint8_t status)
{
    if (status >= 0xF8) return 1;
    uint8_t cmd = status & 0xF0;
    if (cmd == 0xC0 || cmd == 0xD0) return 2;
    return 3;
}

static void ns6_process_midi_out(struct ns6 *ns6)
{
    uint8_t byte;
    unsigned long flags;

    spin_lock_irqsave(&ns6->midi_out_lock, flags);

    if (!ns6->ep_midi_out || !ns6->midi_out_sub) {
        spin_unlock_irqrestore(&ns6->midi_out_lock, flags);
        return;
    }

    while (1) {
        int idx = -1, i;

        for (i = 0; i < NS6_MIDI_OUT_NURBS; i++) {
            if (!ns6->midi_out_busy[i]) { idx = i; break; }
        }
        if (idx == -1) break;

        struct urb *u = ns6->midi_out_urbs[idx];
        uint8_t    *buf = ns6->midi_out_bufs[idx];
        int buf_pos = 0;
        bool has_data = false;

        while (buf_pos + 3 <= NS6_MIDI_PKT_SIZE - 1) {
            while (ns6->midi_out_cache[3] == 0 ||
                   ns6->midi_out_cache_len < ns6->midi_out_cache[3]) {
                if (snd_rawmidi_transmit(ns6->midi_out_sub, &byte, 1) != 1)
                    break;

                if (byte >= 0x80) {
                    ns6->midi_out_cache[0] = byte;
                    ns6->midi_out_cache_len = 1;
                    ns6->midi_out_cache[3] = get_midi_msg_len(byte);
                } else {
                    if (ns6->midi_out_cache_len == 0 &&
                        ns6->midi_out_cache[3] > 0)
                        ns6->midi_out_cache_len = 1;
                    if (ns6->midi_out_cache[3] > 0)
                        ns6->midi_out_cache[ns6->midi_out_cache_len++] = byte;
                }
            }

            if (ns6->midi_out_cache[3] > 0 &&
                ns6->midi_out_cache_len == ns6->midi_out_cache[3]) {
                buf[buf_pos++] = ns6->midi_out_cache[0];
                buf[buf_pos++] = (ns6->midi_out_cache_len > 1) ?
                                  ns6->midi_out_cache[1] : 0x00;
                buf[buf_pos++] = (ns6->midi_out_cache_len > 2) ?
                                  ns6->midi_out_cache[2] : 0x00;
                ns6->midi_out_cache_len = 0;
                has_data = true;
            } else {
                break;
            }
        }

        if (!has_data) break;

        ns6->midi_out_busy[idx] = true;

        while (buf_pos < NS6_MIDI_PKT_SIZE - 1)
            buf[buf_pos++] = NS6_IDLE_BYTE;
        buf[NS6_MIDI_PKT_SIZE - 1] = NS6_PKT_TERM;

        if (usb_endpoint_xfer_int(ns6->ep_midi_out))
            usb_fill_int_urb(u, ns6->udev,
                usb_sndintpipe(ns6->udev, NS6_EP_MIDI_OUT),
                buf, NS6_MIDI_PKT_SIZE, ns6_midi_out_urb_cb, ns6,
                ns6->ep_midi_out->bInterval);
        else
            usb_fill_bulk_urb(u, ns6->udev,
                usb_sndbulkpipe(ns6->udev, NS6_EP_MIDI_OUT),
                buf, NS6_MIDI_PKT_SIZE, ns6_midi_out_urb_cb, ns6);

        if (usb_submit_urb(u, GFP_ATOMIC)) {
            ns6->midi_out_busy[idx] = false;
            break;
        }
    }

    spin_unlock_irqrestore(&ns6->midi_out_lock, flags);
}

static int ns6_midi_in_open(struct snd_rawmidi_substream *sub)
{
    ((struct ns6 *)sub->rmidi->private_data)->midi_in_sub = sub;
    return 0;
}
static int ns6_midi_in_close(struct snd_rawmidi_substream *sub)
{
    ((struct ns6 *)sub->rmidi->private_data)->midi_in_sub = NULL;
    return 0;
}
static void ns6_midi_in_trigger(struct snd_rawmidi_substream *sub, int up) {}

static int ns6_midi_out_open(struct snd_rawmidi_substream *sub)
{
    struct ns6 *ns6 = sub->rmidi->private_data;
    ns6->midi_out_sub = sub;
    ns6->midi_out_cache_len = 0;
    ns6->midi_out_cache[3] = 0;
    memset(ns6->midi_out_busy, 0, sizeof(ns6->midi_out_busy));
    return 0;
}
static int ns6_midi_out_close(struct snd_rawmidi_substream *sub)
{
    ((struct ns6 *)sub->rmidi->private_data)->midi_out_sub = NULL;
    return 0;
}
static void ns6_midi_out_trigger(struct snd_rawmidi_substream *sub, int up)
{
    if (up)
        ns6_process_midi_out(sub->rmidi->private_data);
}

static const struct snd_rawmidi_ops ns6_midi_in_ops = {
    .open = ns6_midi_in_open, .close = ns6_midi_in_close,
    .trigger = ns6_midi_in_trigger
};
static const struct snd_rawmidi_ops ns6_midi_out_ops = {
    .open = ns6_midi_out_open, .close = ns6_midi_out_close,
    .trigger = ns6_midi_out_trigger
};

/* ========================================================================= *
 * ALSA PCM OPS
 * ========================================================================= */
static const struct snd_pcm_hardware ns6_hw = {
    .info             = SNDRV_PCM_INFO_INTERLEAVED |
                        SNDRV_PCM_INFO_BLOCK_TRANSFER |
                        SNDRV_PCM_INFO_MMAP |
                        SNDRV_PCM_INFO_MMAP_VALID |
                        SNDRV_PCM_INFO_BATCH,
    .formats          = SNDRV_PCM_FMTBIT_S24_3LE,
    .rates            = SNDRV_PCM_RATE_44100,
    .rate_min         = NS6_RATE,
    .rate_max         = NS6_RATE,
    .channels_min     = NS6_CHANNELS,
    .channels_max     = NS6_CHANNELS,
    .buffer_bytes_max = NS6_URB_BYTES * NS6_NURBS * 8,
    .period_bytes_min = NS6_FRAME_BYTES * 176,   /* 1 URB worth */
    .period_bytes_max = NS6_URB_BYTES * NS6_NURBS,
    .periods_min      = 2,
    .periods_max      = 16,
};

static int ns6_pcm_open(struct snd_pcm_substream *sub)
{
    struct ns6 *ns6 = snd_pcm_substream_chip(sub);
    sub->runtime->hw = ns6_hw;
    ns6->play.substream = sub;
    return 0;
}

static int ns6_pcm_close(struct snd_pcm_substream *sub)
{
    struct ns6 *ns6 = snd_pcm_substream_chip(sub);
    unsigned long flags;

    spin_lock_irqsave(&ns6->play.lock, flags);
    ns6->play.substream = NULL;
    ns6->play.active_pcm = false;
    spin_unlock_irqrestore(&ns6->play.lock, flags);
    return 0;
}

static int ns6_pcm_hw_params(struct snd_pcm_substream *sub,
                              struct snd_pcm_hw_params *p)
{
    return 0;
}

static int ns6_pcm_hw_free(struct snd_pcm_substream *sub)
{
    /* Do NOT kill URBs here — they run continuously like macOS.
     * Just stop feeding audio data (active_pcm = false). */
    struct ns6 *ns6 = snd_pcm_substream_chip(sub);
    unsigned long flags;

    spin_lock_irqsave(&ns6->play.lock, flags);
    ns6->play.active_pcm = false;
    spin_unlock_irqrestore(&ns6->play.lock, flags);
    return 0;
}

static int ns6_pcm_prepare(struct snd_pcm_substream *sub)
{
    struct ns6 *ns6 = snd_pcm_substream_chip(sub);
    struct ns6_stream *s = &ns6->play;
    unsigned long flags;

    spin_lock_irqsave(&s->lock, flags);
    s->hwptr = 0;
    s->period_pos = 0;
    s->frac_acc = 0;
    s->warmup_cnt = s->active_usb ? 0 : NS6_WARMUP_URBS;
    spin_unlock_irqrestore(&s->lock, flags);

    return 0;
}

static int ns6_pcm_trigger(struct snd_pcm_substream *sub, int cmd)
{
    struct ns6 *ns6 = snd_pcm_substream_chip(sub);
    struct ns6_stream *s = &ns6->play;
    unsigned long flags;

    switch (cmd) {
    case SNDRV_PCM_TRIGGER_START:
        spin_lock_irqsave(&s->lock, flags);
        s->active_pcm = true;
        spin_unlock_irqrestore(&s->lock, flags);
        return 0;
    case SNDRV_PCM_TRIGGER_STOP:
        spin_lock_irqsave(&s->lock, flags);
        s->active_pcm = false;
        spin_unlock_irqrestore(&s->lock, flags);
        return 0;
    }
    return -EINVAL;
}

static snd_pcm_uframes_t ns6_pcm_pointer(struct snd_pcm_substream *sub)
{
    struct ns6 *ns6 = snd_pcm_substream_chip(sub);
    return ns6->play.hwptr;
}

static const struct snd_pcm_ops ns6_pcm_ops = {
    .open      = ns6_pcm_open,
    .close     = ns6_pcm_close,
    .hw_params = ns6_pcm_hw_params,
    .hw_free   = ns6_pcm_hw_free,
    .prepare   = ns6_pcm_prepare,
    .trigger   = ns6_pcm_trigger,
    .pointer   = ns6_pcm_pointer,
};

/* ========================================================================= *
 * URB ALLOCATION
 * ========================================================================= */
static int ns6_alloc_iso_urbs(struct ns6 *ns6, struct ns6_stream *s,
                               struct usb_endpoint_descriptor *epd,
                               void (*cb)(struct urb *))
{
    int i, j;
    u8 ep;
    unsigned int pipe;

    if (!epd)
        return -ENODEV;

    ep = epd->bEndpointAddress;
    pipe = (ep & USB_DIR_IN) ?
           usb_rcvisocpipe(ns6->udev, ep & 0x7f) :
           usb_sndisocpipe(ns6->udev, ep & 0x7f);

    for (i = 0; i < NS6_NURBS; i++) {
        s->urbs[i] = usb_alloc_urb(NS6_PKTS_PER_URB, GFP_KERNEL);
        if (!s->urbs[i])
            return -ENOMEM;

        s->bufs[i] = usb_alloc_coherent(ns6->udev, NS6_URB_BYTES,
                                         GFP_KERNEL, &s->dma[i]);
        if (!s->bufs[i])
            return -ENOMEM;

        s->urbs[i]->dev = ns6->udev;
        s->urbs[i]->pipe = pipe;
        s->urbs[i]->transfer_buffer = s->bufs[i];
        s->urbs[i]->transfer_dma = s->dma[i];
        s->urbs[i]->transfer_flags = URB_ISO_ASAP | URB_NO_TRANSFER_DMA_MAP;
        s->urbs[i]->transfer_buffer_length = NS6_URB_BYTES;
        s->urbs[i]->number_of_packets = NS6_PKTS_PER_URB;
        s->urbs[i]->complete = cb;
        s->urbs[i]->context = ns6;
        s->urbs[i]->interval = 1;  /* every microframe */

        for (j = 0; j < NS6_PKTS_PER_URB; j++) {
            s->urbs[i]->iso_frame_desc[j].offset = j * NS6_PKT_SIZE;
            s->urbs[i]->iso_frame_desc[j].length = NS6_PKT_SIZE;
        }
    }
    return 0;
}

static void ns6_free_iso_urbs(struct ns6 *ns6, struct ns6_stream *s)
{
    int i;
    for (i = 0; i < NS6_NURBS; i++) {
        if (s->urbs[i]) {
            usb_kill_urb(s->urbs[i]);
            if (s->bufs[i])
                usb_free_coherent(ns6->udev, NS6_URB_BYTES,
                                  s->bufs[i], s->dma[i]);
            usb_free_urb(s->urbs[i]);
            s->urbs[i] = NULL;
        }
    }
}

/* Start audio + sync URBs — called once from probe, run until disconnect */
static int ns6_start_streaming(struct ns6 *ns6)
{
    struct ns6_stream *s = &ns6->play;
    int i, j, err;

    /* Start sync URBs (EP 0x81 feedback — dummy) */
    if (ns6->ep_sync) {
        ns6->sync_active = true;
        for (i = 0; i < NS6_SYNC_NURBS; i++) {
            if (ns6->sync_urbs[i]) {
                memset(ns6->sync_bufs[i], 0, NS6_SYNC_URB_BYTES);
                err = usb_submit_urb(ns6->sync_urbs[i], GFP_KERNEL);
                if (err)
                    dev_warn(&ns6->udev->dev,
                             "NS6: sync URB %d submit err %d\n", i, err);
            }
        }
    }

    /* Start audio URBs (EP 0x02) with silence */
    s->active_usb = true;
    s->frac_acc = 0;
    s->warmup_cnt = NS6_WARMUP_URBS;

    for (i = 0; i < NS6_NURBS; i++) {
        if (!s->urbs[i])
            continue;

        unsigned int total_bytes = 0;
        for (j = 0; j < NS6_PKTS_PER_URB; j++) {
            s->frac_acc += NS6_RATE;
            unsigned int pk_frames = s->frac_acc / 8000;
            s->frac_acc %= 8000;
            unsigned int pk_bytes = pk_frames * NS6_FRAME_BYTES;
            if (pk_bytes > NS6_PKT_SIZE)
                pk_bytes = NS6_PKT_SIZE;

            s->urbs[i]->iso_frame_desc[j].offset = total_bytes;
            s->urbs[i]->iso_frame_desc[j].length = pk_bytes;
            total_bytes += pk_bytes;
        }
        s->urbs[i]->transfer_buffer_length = total_bytes;
        memset(s->urbs[i]->transfer_buffer, 0, total_bytes);

        err = usb_submit_urb(s->urbs[i], GFP_KERNEL);
        if (err) {
            dev_err(&ns6->udev->dev,
                    "NS6: audio URB %d submit err %d\n", i, err);
            s->active_usb = false;
            return err;
        }
    }

    dev_info(&ns6->udev->dev, "NS6: Streaming started (%d URBs).\n",
             NS6_NURBS);

    /* Start waveform drain URBs (EP 0x86 bulk IN — discard data) */
    if (ns6->wf_urbs[0]) {
        ns6->wf_active = true;
        for (i = 0; i < NS6_WF_NURBS; i++) {
            if (ns6->wf_urbs[i]) {
                err = usb_submit_urb(ns6->wf_urbs[i], GFP_KERNEL);
                if (err)
                    dev_warn(&ns6->udev->dev,
                             "NS6: waveform URB %d submit err %d\n", i, err);
            }
        }
        dev_info(&ns6->udev->dev, "NS6: Waveform drain active (%d URBs).\n",
                 NS6_WF_NURBS);
    }

    return 0;
}

/* ========================================================================= *
 * PROBE & DISCONNECT
 * ========================================================================= */
static int ns6_probe(struct usb_interface *intf,
                      const struct usb_device_id *id)
{
    struct usb_device *udev = interface_to_usbdev(intf);
    struct ns6 *ns6;
    struct snd_card *card;
    struct snd_pcm *pcm;
    int i, j, err;

    if (intf->cur_altsetting->desc.bInterfaceNumber != 0)
        return -ENODEV;

    err = snd_card_new(&intf->dev, -1, "NS6", THIS_MODULE,
                       sizeof(*ns6), &card);
    if (err)
        return err;

    ns6 = card->private_data;
    ns6->udev = udev;
    ns6->card = card;
    ns6->disconnecting = false;

    usb_set_intfdata(intf, ns6);

    spin_lock_init(&ns6->play.lock);
    spin_lock_init(&ns6->midi_out_lock);

    strscpy(card->driver, DRV_NAME, sizeof(card->driver));
    strscpy(card->shortname, "Numark NS6");
    strscpy(card->longname, "Numark NS6 DJ Controller");

    /* Find endpoints */
    ns6->ep_play = ns6_get_ep_desc(udev, NS6_EP_PLAY);
    ns6->ep_midi_in = ns6_get_ep_desc(udev, NS6_EP_MIDI_IN);
    ns6->ep_midi_out = ns6_get_ep_desc(udev, NS6_EP_MIDI_OUT);

    /* Claim Interface 1 for sync EP 0x81 */
    ns6->intf1 = usb_ifnum_to_if(udev, 1);
    if (ns6->intf1) {
        err = usb_driver_claim_interface(&ns6_driver, ns6->intf1, ns6);
        if (!err) {
            usb_set_interface(udev, 1, 1);
            ns6->ep_sync = ns6_get_ep_desc(udev, NS6_EP_SYNC);
        }
    }

    /* Clear halt on endpoints (macOS does this) */
    usb_clear_halt(udev, usb_sndbulkpipe(udev, NS6_EP_PLAY & 0x7f));
    usb_clear_halt(udev, usb_rcvbulkpipe(udev, NS6_EP_MIDI_IN & 0x7f));
    usb_clear_halt(udev, usb_rcvbulkpipe(udev, 0x86 & 0x7f));

    /* Allocate audio URBs */
    err = ns6_alloc_iso_urbs(ns6, &ns6->play, ns6->ep_play,
                              ns6_play_urb_cb);
    if (err)
        goto err_free_play;

    /* Allocate sync URBs */
    if (ns6->ep_sync) {
        for (i = 0; i < NS6_SYNC_NURBS; i++) {
            ns6->sync_urbs[i] = usb_alloc_urb(NS6_SYNC_PKTS_PER_URB,
                                                GFP_KERNEL);
            if (!ns6->sync_urbs[i]) { err = -ENOMEM; goto err_free_sync; }

            ns6->sync_bufs[i] = usb_alloc_coherent(udev, NS6_SYNC_URB_BYTES,
                                    GFP_KERNEL, &ns6->sync_dma[i]);
            if (!ns6->sync_bufs[i]) { err = -ENOMEM; goto err_free_sync; }

            ns6->sync_urbs[i]->dev = udev;
            ns6->sync_urbs[i]->pipe = usb_rcvisocpipe(udev,
                                          NS6_EP_SYNC & 0x7f);
            ns6->sync_urbs[i]->transfer_buffer = ns6->sync_bufs[i];
            ns6->sync_urbs[i]->transfer_dma = ns6->sync_dma[i];
            ns6->sync_urbs[i]->transfer_flags =
                URB_ISO_ASAP | URB_NO_TRANSFER_DMA_MAP;
            ns6->sync_urbs[i]->transfer_buffer_length = NS6_SYNC_URB_BYTES;
            ns6->sync_urbs[i]->number_of_packets = NS6_SYNC_PKTS_PER_URB;
            ns6->sync_urbs[i]->complete = ns6_sync_urb_cb;
            ns6->sync_urbs[i]->context = ns6;
            ns6->sync_urbs[i]->interval =
                1 << (ns6->ep_sync->bInterval - 1);

            for (j = 0; j < NS6_SYNC_PKTS_PER_URB; j++) {
                ns6->sync_urbs[i]->iso_frame_desc[j].offset =
                    j * NS6_SYNC_PKT_SIZE;
                ns6->sync_urbs[i]->iso_frame_desc[j].length =
                    NS6_SYNC_PKT_SIZE;
            }
        }
    }

    /* Allocate waveform drain URBs (EP 0x86 bulk IN) */
    for (i = 0; i < NS6_WF_NURBS; i++) {
        ns6->wf_urbs[i] = usb_alloc_urb(0, GFP_KERNEL);
        if (!ns6->wf_urbs[i]) { err = -ENOMEM; goto err_free_wf; }

        ns6->wf_bufs[i] = usb_alloc_coherent(udev, NS6_WF_PKT_SIZE, GFP_KERNEL, &ns6->wf_dma[i]);
        if (!ns6->wf_bufs[i]) { err = -ENOMEM; goto err_free_wf; }

        usb_fill_bulk_urb(ns6->wf_urbs[i], udev,
            usb_rcvbulkpipe(udev, NS6_EP_WAVEFORM & 0x7f),
            ns6->wf_bufs[i], NS6_WF_PKT_SIZE,
            ns6_wf_urb_cb, ns6);
            
        ns6->wf_urbs[i]->transfer_dma = ns6->wf_dma[i];
        ns6->wf_urbs[i]->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
    }

    /* MIDI IN URBs */
    for (i = 0; i < NS6_MIDI_NURBS; i++) {
        ns6->midi_in_urbs[i] = usb_alloc_urb(0, GFP_KERNEL);
        if (!ns6->midi_in_urbs[i]) { err = -ENOMEM; goto err_free_midi; }

        if (usb_endpoint_xfer_int(ns6->ep_midi_in))
            usb_fill_int_urb(ns6->midi_in_urbs[i], udev,
                usb_rcvintpipe(udev, NS6_EP_MIDI_IN),
                ns6->midi_in_buf[i], NS6_MIDI_PKT_SIZE,
                ns6_midi_in_urb_cb, ns6,
                ns6->ep_midi_in->bInterval);
        else
            usb_fill_bulk_urb(ns6->midi_in_urbs[i], udev,
                usb_rcvbulkpipe(udev, NS6_EP_MIDI_IN),
                ns6->midi_in_buf[i], NS6_MIDI_PKT_SIZE,
                ns6_midi_in_urb_cb, ns6);
        usb_submit_urb(ns6->midi_in_urbs[i], GFP_KERNEL);
    }

    /* MIDI OUT URBs */
    for (i = 0; i < NS6_MIDI_OUT_NURBS; i++) {
        ns6->midi_out_urbs[i] = usb_alloc_urb(0, GFP_KERNEL);
        if (!ns6->midi_out_urbs[i]) { err = -ENOMEM; goto err_free_midi; }
    }

    /* PCM device: 1 playback, 0 capture */
    err = snd_pcm_new(card, "NS6 Audio", 0, 1, 0, &pcm);
    if (err)
        goto err_free_pcm;

    ns6->pcm = pcm;
    pcm->private_data = ns6;
    strscpy(pcm->name, "Numark NS6");
    snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &ns6_pcm_ops);
    snd_pcm_set_managed_buffer_all(pcm, SNDRV_DMA_TYPE_VMALLOC, NULL, 0, 0);

    /* MIDI device */
    err = snd_rawmidi_new(card, "NS6 MIDI", 0, 1, 1, &ns6->rmidi);
    if (err)
        goto err_free_pcm;

    ns6->rmidi->private_data = ns6;
    strscpy(ns6->rmidi->name, "Numark NS6 MIDI");
    snd_rawmidi_set_ops(ns6->rmidi, SNDRV_RAWMIDI_STREAM_INPUT,
                        &ns6_midi_in_ops);
    snd_rawmidi_set_ops(ns6->rmidi, SNDRV_RAWMIDI_STREAM_OUTPUT,
                        &ns6_midi_out_ops);
    ns6->rmidi->info_flags = SNDRV_RAWMIDI_INFO_OUTPUT |
                             SNDRV_RAWMIDI_INFO_INPUT |
                             SNDRV_RAWMIDI_INFO_DUPLEX;

    err = snd_card_register(card);
    if (err)
        goto err_free_pcm;

    /* Start streaming immediately (macOS starts URBs right after init) */
    // err = ns6_start_streaming(ns6);
    // if (err)
    //     dev_warn(&udev->dev, "NS6: streaming start err %d\n", err);

    /* Deferred init (vendor commands, SysEx) */
    INIT_WORK(&ns6->init_work, ns6_deferred_init);
    schedule_work(&ns6->init_work);

    dev_info(&udev->dev, "NS6: Probe complete. %d audio URBs, "
             "feedback=dummy.\n", NS6_NURBS);
    return 0;

err_free_pcm:
    for (i = 0; i < NS6_MIDI_OUT_NURBS; i++) {
        if (ns6->midi_out_urbs[i]) {
            usb_kill_urb(ns6->midi_out_urbs[i]);
            usb_free_urb(ns6->midi_out_urbs[i]);
        }
    }
err_free_midi:
    for (i = 0; i < NS6_MIDI_NURBS; i++) {
        if (ns6->midi_in_urbs[i]) {
            usb_kill_urb(ns6->midi_in_urbs[i]);
            usb_free_urb(ns6->midi_in_urbs[i]);
        }
    }
err_free_wf:
    for (i = 0; i < NS6_WF_NURBS; i++) {
        if (ns6->wf_urbs[i]) {
            usb_kill_urb(ns6->wf_urbs[i]);
            if (ns6->wf_bufs[i])
                usb_free_coherent(ns6->udev, NS6_WF_PKT_SIZE,
                                  ns6->wf_bufs[i], ns6->wf_dma[i]);
            usb_free_urb(ns6->wf_urbs[i]);
        }
    }
err_free_sync:
    for (i = 0; i < NS6_SYNC_NURBS; i++) {
        if (ns6->sync_urbs[i]) {
            usb_kill_urb(ns6->sync_urbs[i]);
            if (ns6->sync_bufs[i])
                usb_free_coherent(ns6->udev, NS6_SYNC_URB_BYTES,
                                  ns6->sync_bufs[i], ns6->sync_dma[i]);
            usb_free_urb(ns6->sync_urbs[i]);
        }
    }
err_free_play:
    ns6_free_iso_urbs(ns6, &ns6->play);
    if (ns6->intf1) {
        usb_set_interface(ns6->udev, 1, 0);
        usb_driver_release_interface(&ns6_driver, ns6->intf1);
    }
    usb_set_intfdata(intf, NULL);
    snd_card_free(card);
    return err;
}

static void ns6_disconnect(struct usb_interface *intf)
{
    struct ns6 *ns6 = usb_get_intfdata(intf);
    int i;

    if (!ns6)
        return;

    /* 1. Prevenção de Loop Recursivo!
     * Se o kernel chamou isto para a Interface 1, nós ignoramos 
     * porque a Interface 0 é a dona da placa e vai limpar tudo. */
    if (ns6->intf1 == intf) {
        usb_set_intfdata(intf, NULL);
        return;
    }

    /* Limpamos os registos imediatamente para evitar que outras threads nos achem */
    usb_set_intfdata(intf, NULL);

    /* Soltamos a Interface 1 logo no início, de forma segura */
    if (ns6->intf1) {
        struct usb_interface *i1 = ns6->intf1;
        ns6->intf1 = NULL;
        usb_set_intfdata(i1, NULL); /* Corta o mal pela raiz */
        usb_set_interface(ns6->udev, 1, 0);
        usb_driver_release_interface(&ns6_driver, i1);
    }

    /* 2. Avisar o ALSA que o hardware desapareceu */
    snd_card_disconnect(ns6->card);

    ns6->disconnecting = true;
    ns6->play.active_usb = false;
    ns6->play.active_pcm = false;
    ns6->sync_active = false;
    ns6->wf_active = false;

    cancel_work_sync(&ns6->init_work);

    /* 3. Matar todos os URBs em voo */
    for (i = 0; i < NS6_NURBS; i++) {
        if (ns6->play.urbs[i]) usb_kill_urb(ns6->play.urbs[i]);
    }
    for (i = 0; i < NS6_SYNC_NURBS; i++) {
        if (ns6->sync_urbs[i]) usb_kill_urb(ns6->sync_urbs[i]);
    }
    for (i = 0; i < NS6_WF_NURBS; i++) {
        if (ns6->wf_urbs[i]) usb_kill_urb(ns6->wf_urbs[i]);
    }
    for (i = 0; i < NS6_MIDI_NURBS; i++) {
        if (ns6->midi_in_urbs[i]) usb_kill_urb(ns6->midi_in_urbs[i]);
    }
    for (i = 0; i < NS6_MIDI_OUT_NURBS; i++) {
        if (ns6->midi_out_urbs[i]) usb_kill_urb(ns6->midi_out_urbs[i]);
    }

    /* 4. Limpar toda a memória DMA e anular os ponteiros (Prevenção Double-Free) */
    ns6_free_iso_urbs(ns6, &ns6->play);

    for (i = 0; i < NS6_SYNC_NURBS; i++) {
        if (ns6->sync_urbs[i]) {
            if (ns6->sync_bufs[i])
                usb_free_coherent(ns6->udev, NS6_SYNC_URB_BYTES,
                                  ns6->sync_bufs[i], ns6->sync_dma[i]);
            usb_free_urb(ns6->sync_urbs[i]);
            ns6->sync_urbs[i] = NULL;
            ns6->sync_bufs[i] = NULL;
        }
    }

    for (i = 0; i < NS6_WF_NURBS; i++) {
        if (ns6->wf_urbs[i]) {
            if (ns6->wf_bufs[i])
                usb_free_coherent(ns6->udev, NS6_WF_PKT_SIZE,
                                  ns6->wf_bufs[i], ns6->wf_dma[i]);
            usb_free_urb(ns6->wf_urbs[i]);
            ns6->wf_urbs[i] = NULL;
            ns6->wf_bufs[i] = NULL;
        }
    }

    for (i = 0; i < NS6_MIDI_NURBS; i++) {
        if (ns6->midi_in_urbs[i]) {
            usb_free_urb(ns6->midi_in_urbs[i]);
            ns6->midi_in_urbs[i] = NULL;
        }
    }
    for (i = 0; i < NS6_MIDI_OUT_NURBS; i++) {
        if (ns6->midi_out_urbs[i]) {
            usb_free_urb(ns6->midi_out_urbs[i]);
            ns6->midi_out_urbs[i] = NULL;
        }
    }

    /* 5. Destruir a placa e a struct ns6 */
    snd_card_free(ns6->card);
}

static const struct usb_device_id ns6_id_table[] = {
    { USB_DEVICE_INTERFACE_NUMBER(NS6_VID, NS6_PID, 0) },
    { }
};
MODULE_DEVICE_TABLE(usb, ns6_id_table);

static struct usb_driver ns6_driver = {
    .name                = DRV_NAME,
    .probe               = ns6_probe,
    .disconnect          = ns6_disconnect,
    .id_table            = ns6_id_table,
    .supports_autosuspend = 0,
};

module_usb_driver(ns6_driver);
MODULE_AUTHOR("ns6d project");
MODULE_DESCRIPTION("Numark NS6 USB audio/MIDI driver");
MODULE_LICENSE("GPL v2");
