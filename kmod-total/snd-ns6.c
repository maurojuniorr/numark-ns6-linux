// SPDX-License-Identifier: GPL-2.0
/*
 * snd-ns6.c — Numark NS6 complete USB driver
 * ALSA STABLE EDITION (Double-Buffer & Anti-Crash)
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
#define NS6_EP_PLAY     0x02
#define NS6_EP_MIDI_IN  0x83
#define NS6_EP_MIDI_OUT 0x04
#define NS6_EP_CAP      0x81

/* PCM format */
#define NS6_RATE            44100
#define NS6_CHANNELS        4
#define NS6_SAMPLE_BYTES    3
#define NS6_FRAME_BYTES     (NS6_CHANNELS * NS6_SAMPLE_BYTES)  /* 12 */
#define NS6_PKTS_PER_URB    32
#define NS6_PKT_SIZE        72   
#define NS6_URB_BYTES       (NS6_PKTS_PER_URB * NS6_PKT_SIZE)  
#define NS6_NURBS           2

/* MIDI */
#define NS6_MIDI_PKT_SIZE   42
#define NS6_MIDI_NURBS      4
#define NS6_IDLE_BYTE       0xFD
#define NS6_PKT_TERM        0x00



struct ns6_stream {
    struct urb               *urbs[NS6_NURBS];
    unsigned char            *bufs[NS6_NURBS];
    dma_addr_t                dma[NS6_NURBS];
    struct snd_pcm_substream *substream;
    spinlock_t                lock;
    unsigned int              hwptr;
    unsigned int              period_pos;
    unsigned int              phase; 
    bool                      active_usb; /* Mantém o cabo vivo enviando silêncio */
    bool                      active_pcm; /* Libera o áudio real da música */
    

};

struct ns6 {
    struct usb_device    *udev;
    struct snd_card      *card;
    struct snd_pcm       *pcm;
    struct ns6_stream     play;
    struct ns6_stream     cap;

    struct snd_rawmidi       *rmidi;
    struct snd_rawmidi_substream *midi_in_sub;
    struct snd_rawmidi_substream *midi_out_sub;

    struct urb           *midi_in_urbs[NS6_MIDI_NURBS];
    uint8_t               midi_in_buf[NS6_MIDI_NURBS][NS6_MIDI_PKT_SIZE];
    struct urb           *midi_out_urb;
    uint8_t               midi_out_buf[NS6_MIDI_PKT_SIZE];
    spinlock_t            midi_out_lock;
    bool                  midi_out_busy;
    
    /* VARIÁVEIS DO CACHE DA FILA DE LEDS */
    uint8_t               midi_out_cache[3];
    int                   midi_out_cache_len;
    
    struct work_struct    init_work;

    struct usb_endpoint_descriptor *ep_play;
    struct usb_endpoint_descriptor *ep_cap;
    struct usb_endpoint_descriptor *ep_midi_in;
    struct usb_endpoint_descriptor *ep_midi_out;
};
/* ========================================================================= *
 * DYNAMIC ENDPOINT FINDER
 * ========================================================================= */
static struct usb_endpoint_descriptor *ns6_get_ep_desc(struct usb_device *udev, u8 ep_addr) {
    int i, a, e;
    for (i = 0; i < udev->actconfig->desc.bNumInterfaces; i++) {
        struct usb_interface *intf = udev->actconfig->interface[i];
        for (a = 0; a < intf->num_altsetting; a++) {
            struct usb_host_interface *alt = &intf->altsetting[a];
            for (e = 0; e < alt->desc.bNumEndpoints; e++) {
                if (alt->endpoint[e].desc.bEndpointAddress == ep_addr) {
                    if (intf->cur_altsetting != alt) {
                        pr_info("NS6: EP 0x%02x achado escondido! Ativando Interface %d Altsetting %d...\n", 
                                ep_addr, alt->desc.bInterfaceNumber, alt->desc.bAlternateSetting);
                        usb_set_interface(udev, alt->desc.bInterfaceNumber, alt->desc.bAlternateSetting);
                    }
                    return &alt->endpoint[e].desc;
                }
            }
        }
    }
    return NULL;
}

/* ========================================================================= *
 * NS6 INITIALIZATION SEQUENCE
 * ========================================================================= */
#define NS6_BREQ_VENDOR_MODE     73   
#define NS6_VENDOR_MODE_ACTIVATE 0x0032
#define NS6_CTRL_PKT_SIZE        42
#define NS6_PKT_TERMINATOR       0x00

static void ns6_deferred_init(struct work_struct *work) {
    struct ns6 *ns6 = container_of(work, struct ns6, init_work);
    int actual_len; /* Retiramos o 'err' daqui para limpar o warning! */
    u8 *buf = kzalloc(NS6_CTRL_PKT_SIZE, GFP_KERNEL);

    if (!buf) return;

    usb_control_msg(ns6->udev, usb_sndctrlpipe(ns6->udev, 0),
                    NS6_BREQ_VENDOR_MODE, 0x40, NS6_VENDOR_MODE_ACTIVATE, 0, NULL, 0, 1000);

    u8 sysex_init[] = {
        0xF0, 0x00, 0x01, 0x3F, 0x00, 0x79, 0x51, 0x00,
        0x10, 0x49, 0x01, 0x08, 0x01, 0x01, 0x08, 0x04,
        0x0C, 0x0D, 0x01, 0x0A, 0x0A, 0x05, 0x06, 0x05,
        0x0D, 0x07, 0x0E, 0x08, 0x07, 0x0D, 0xF7
    };

    memset(buf, NS6_IDLE_BYTE, NS6_CTRL_PKT_SIZE);
    buf[NS6_CTRL_PKT_SIZE - 1] = NS6_PKT_TERMINATOR;
    memcpy(buf, sysex_init, sizeof(sysex_init));

    if (!ns6->ep_midi_out) {
        kfree(buf); return;
    }

    if (usb_endpoint_xfer_int(ns6->ep_midi_out)) {
        usb_interrupt_msg(ns6->udev, usb_sndintpipe(ns6->udev, NS6_EP_MIDI_OUT), buf, NS6_CTRL_PKT_SIZE, &actual_len, 2000);
    } else {
        usb_bulk_msg(ns6->udev, usb_sndbulkpipe(ns6->udev, NS6_EP_MIDI_OUT), buf, NS6_CTRL_PKT_SIZE, &actual_len, 2000);
    }

    pr_info("NS6: Inicializacao Concluida!\n");
    kfree(buf);
}

/* ========================================================================= *
 * URB CALLBACKS & AUDIO LOGIC
 * ========================================================================= */
static void ns6_play_urb_cb(struct urb *urb) {
    struct ns6 *ns6 = urb->context;
    struct ns6_stream *s = &ns6->play;
    struct snd_pcm_substream *sub;
    unsigned long flags;
    bool elapsed = false;
    unsigned int total_frames = 0, total_bytes = 0;
    int i;
    bool fill_silence = false;

    if (urb->status != 0 && urb->status != -EXDEV) return;
    if (!s->active_usb) return; 

    spin_lock_irqsave(&s->lock, flags);
    sub = s->substream;

    /* Verifica se devemos mandar silêncio (deck pausado ou descarregado) */
    if (!s->active_pcm || !sub || !snd_pcm_running(sub) || !sub->runtime->dma_area) {
        fill_silence = true;
    }

    /* 1. SEMPRE RODA A MATEMÁTICA DO RELÓGIO (Mesmo no silêncio absoluto!)
     * Isso mantém o PLL da placa travado em perfeitos 44.100Hz o tempo todo. */
    for (i = 0; i < urb->number_of_packets; i++) {
        unsigned int pk_frames = 5; 
        s->phase += 41;          
        if (s->phase >= 80) { pk_frames += 1; s->phase -= 80; }
        unsigned int pk_bytes = pk_frames * NS6_FRAME_BYTES;
        
        urb->iso_frame_desc[i].offset = total_bytes;
        urb->iso_frame_desc[i].length = pk_bytes;
        
        total_frames += pk_frames;
        total_bytes  += pk_bytes;
    }

    /* 2. PREENCHE COM ÁUDIO OU SILÊNCIO */
    if (fill_silence) {
        /* Manda zeros, mas com o tamanho fracionário matematicamente perfeito */
        memset(urb->transfer_buffer, 0, total_bytes);
    } else {
        /* Manda a música real do ALSA */
        unsigned int buf_frames = sub->runtime->buffer_size;
        unsigned char *dma = sub->runtime->dma_area;
        unsigned int hwptr_old = s->hwptr;

        if (hwptr_old + total_frames > buf_frames) {
            unsigned int chunk1 = buf_frames - hwptr_old;
            memcpy(urb->transfer_buffer, dma + hwptr_old * NS6_FRAME_BYTES, chunk1 * NS6_FRAME_BYTES);
            memcpy(urb->transfer_buffer + chunk1 * NS6_FRAME_BYTES, dma, (total_frames - chunk1) * NS6_FRAME_BYTES);
        } else {
            memcpy(urb->transfer_buffer, dma + hwptr_old * NS6_FRAME_BYTES, total_frames * NS6_FRAME_BYTES);
        }
        
        s->hwptr += total_frames;
        if (s->hwptr >= buf_frames) s->hwptr -= buf_frames;

        s->period_pos += total_frames;
        if (s->period_pos >= sub->runtime->period_size) {
            s->period_pos -= sub->runtime->period_size;
            elapsed = true;
        }
    }
    spin_unlock_irqrestore(&s->lock, flags);

    /* Só avisa o Mixxx que o tempo passou se a música estiver rodando */
    if (elapsed && !fill_silence) snd_pcm_period_elapsed(sub);

    if (s->active_usb) usb_submit_urb(urb, GFP_ATOMIC);
}

static void ns6_cap_urb_cb(struct urb *urb) {
    struct ns6 *ns6 = urb->context;
    if (urb->status != 0 && urb->status != -EXDEV) return;
    if (ns6->cap.active_usb) usb_submit_urb(urb, GFP_ATOMIC);
}

/* ========================================================================= *
 * MIDI (COM FILA DE ESPERA CONTÍNUA ANTI-PERDA DE PACOTES)
 * ========================================================================= */
static void ns6_parse_midi(struct ns6 *ns6, const uint8_t *pkt, int len) {
    struct snd_rawmidi_substream *sub = ns6->midi_in_sub;
    int i = 0;
    if (!sub) return;
    while (i < len) {
        uint8_t b = pkt[i];
        if (b == NS6_IDLE_BYTE || b == NS6_PKT_TERM) break;
        if (i + 2 < len) {
            uint8_t msg[3] = { pkt[i], pkt[i+1], pkt[i+2] };
            snd_rawmidi_receive(sub, msg, 3); i += 3;
        } else break;
    }
}

static void ns6_midi_in_urb_cb(struct urb *urb) {
    struct ns6 *ns6 = urb->context;
    if (urb->status != 0) return; 
    if (urb->actual_length == NS6_MIDI_PKT_SIZE) ns6_parse_midi(ns6, urb->transfer_buffer, urb->actual_length);
    usb_submit_urb(urb, GFP_ATOMIC);
}

/* ---> DECLARAÇÃO PRÉVIA: Ensina ao compilador que o Callback existe! <--- */
static void ns6_midi_out_urb_cb(struct urb *urb);

/* O ROBÔ DA FILA: Puxa pacotes do Mixxx, empacota em 42 bytes e despacha */
static void ns6_process_midi_out(struct ns6 *ns6) {
    uint8_t byte;
    int err;

    if (!ns6->ep_midi_out || !ns6->midi_out_sub) {
        ns6->midi_out_busy = false;
        return;
    }

    /* Puxa byte por byte do buffer do ALSA */
    while (snd_rawmidi_transmit(ns6->midi_out_sub, &byte, 1) == 1) {
        
        /* Ressincroniza o robô se encontrar um byte de comando (ex: 0x90) */
        if (byte >= 0x80) ns6->midi_out_cache_len = 0;
        
        if (ns6->midi_out_cache_len < 3)
            ns6->midi_out_cache[ns6->midi_out_cache_len++] = byte;

        /* Quando acumular 3 bytes (Comando MIDI completo), enviamos! */
        if (ns6->midi_out_cache_len == 3) {
            memset(ns6->midi_out_buf, NS6_IDLE_BYTE, NS6_MIDI_PKT_SIZE);
            ns6->midi_out_buf[0] = ns6->midi_out_cache[0];
            ns6->midi_out_buf[1] = ns6->midi_out_cache[1];
            ns6->midi_out_buf[2] = ns6->midi_out_cache[2];
            ns6->midi_out_buf[NS6_MIDI_PKT_SIZE - 1] = NS6_PKT_TERM;

            ns6->midi_out_cache_len = 0; /* Zera para não poluir o próximo */

            if (usb_endpoint_xfer_int(ns6->ep_midi_out)) {
                usb_fill_int_urb(ns6->midi_out_urb, ns6->udev, 
                                 usb_sndintpipe(ns6->udev, NS6_EP_MIDI_OUT), 
                                 ns6->midi_out_buf, NS6_MIDI_PKT_SIZE, 
                                 ns6_midi_out_urb_cb, ns6, 
                                 ns6->ep_midi_out->bInterval);
            } else {
                usb_fill_bulk_urb(ns6->midi_out_urb, ns6->udev, 
                                  usb_sndbulkpipe(ns6->udev, NS6_EP_MIDI_OUT), 
                                  ns6->midi_out_buf, NS6_MIDI_PKT_SIZE, 
                                  ns6_midi_out_urb_cb, ns6);
            }
            
            err = usb_submit_urb(ns6->midi_out_urb, GFP_ATOMIC);
            if (err) {
                pr_err("NS6: Falha no URB MIDI de LEDs (%d)\n", err);
                ns6->midi_out_busy = false;
            }
            /* Sai da função! O callback (urb_cb) se encarrega de puxar a próxima luz */
            return; 
        }
    }

    /* Se não houver mais luzes pra acender, desocupa a porta */
    ns6->midi_out_busy = false;
}

/* O CORPO DO CALLBACK FICA AQUI EMBAIXO */
static void ns6_midi_out_urb_cb(struct urb *urb) {
    struct ns6 *ns6 = urb->context; 
    unsigned long flags;
    
    /* Quando a luz passada terminou de acender, avisa o robô para mandar a próxima */
    spin_lock_irqsave(&ns6->midi_out_lock, flags); 
    ns6_process_midi_out(ns6);
    spin_unlock_irqrestore(&ns6->midi_out_lock, flags);
}

static int ns6_midi_in_open(struct snd_rawmidi_substream *sub) { ((struct ns6*)sub->rmidi->private_data)->midi_in_sub = sub; return 0; }
static int ns6_midi_in_close(struct snd_rawmidi_substream *sub) { ((struct ns6*)sub->rmidi->private_data)->midi_in_sub = NULL; return 0; }
static void ns6_midi_in_trigger(struct snd_rawmidi_substream *sub, int up) {}
static int ns6_midi_out_open(struct snd_rawmidi_substream *sub) { ((struct ns6*)sub->rmidi->private_data)->midi_out_sub = sub; return 0; }
static int ns6_midi_out_close(struct snd_rawmidi_substream *sub) { ((struct ns6*)sub->rmidi->private_data)->midi_out_sub = NULL; return 0; }

static void ns6_midi_out_trigger(struct snd_rawmidi_substream *sub, int up) {
    struct ns6 *ns6 = sub->rmidi->private_data; 
    unsigned long flags;

    if (!up) return;

    spin_lock_irqsave(&ns6->midi_out_lock, flags);
    if (!ns6->midi_out_busy) {
        ns6->midi_out_busy = true;
        ns6_process_midi_out(ns6);
    }
    spin_unlock_irqrestore(&ns6->midi_out_lock, flags);
}

static const struct snd_rawmidi_ops ns6_midi_in_ops = { .open = ns6_midi_in_open, .close = ns6_midi_in_close, .trigger = ns6_midi_in_trigger };
static const struct snd_rawmidi_ops ns6_midi_out_ops = { .open = ns6_midi_out_open, .close = ns6_midi_out_close, .trigger = ns6_midi_out_trigger };

/* ========================================================================= *
 * ALSA PCM CONFIG (TOTALMENTE BLINDADO AGORA)
 * ========================================================================= */
static const struct snd_pcm_hardware ns6_hw = {
    .info             = SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BLOCK_TRANSFER | SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID | SNDRV_PCM_INFO_BATCH,
    .formats          = SNDRV_PCM_FMTBIT_S24_3LE,
    .rates            = SNDRV_PCM_RATE_44100,
    .rate_min         = NS6_RATE, .rate_max = NS6_RATE,
    .channels_min     = NS6_CHANNELS, .channels_max = NS6_CHANNELS,
    .buffer_bytes_max = NS6_URB_BYTES * NS6_NURBS * 8,
    .period_bytes_min = NS6_URB_BYTES,
    .period_bytes_max = NS6_URB_BYTES * NS6_NURBS,
    .periods_min      = 2, .periods_max = 16,
};

static int ns6_pcm_open(struct snd_pcm_substream *sub) {
    struct ns6 *ns6 = snd_pcm_substream_chip(sub);
    sub->runtime->hw = ns6_hw;
    snd_pcm_hw_constraint_step(sub->runtime, 0, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, 176);
    snd_pcm_hw_constraint_step(sub->runtime, 0, SNDRV_PCM_HW_PARAM_BUFFER_SIZE, 176);
    if (sub->stream == SNDRV_PCM_STREAM_PLAYBACK) ns6->play.substream = sub; else ns6->cap.substream = sub;
    return 0;
}

static int ns6_pcm_close(struct snd_pcm_substream *sub) {
    struct ns6 *ns6 = snd_pcm_substream_chip(sub);
    unsigned long flags;
    struct ns6_stream *s = (sub->stream == SNDRV_PCM_STREAM_PLAYBACK) ? &ns6->play : &ns6->cap;
    
    spin_lock_irqsave(&s->lock, flags);
    s->substream = NULL;
    spin_unlock_irqrestore(&s->lock, flags);
    return 0;
}

static int ns6_pcm_hw_params(struct snd_pcm_substream *sub, struct snd_pcm_hw_params *p) { return 0; }

static int ns6_pcm_hw_free(struct snd_pcm_substream *sub) { 
    struct ns6 *ns6 = snd_pcm_substream_chip(sub);
    struct ns6_stream *s = (sub->stream == SNDRV_PCM_STREAM_PLAYBACK) ? &ns6->play : &ns6->cap;
    unsigned long flags;
    int i;
    
    /* Ao fechar a música, cortamos a força dos cabos e matamos os URBs pacificamente */
    spin_lock_irqsave(&s->lock, flags);
    s->active_usb = false; 
    s->active_pcm = false;
    spin_unlock_irqrestore(&s->lock, flags);

    for (i = 0; i < NS6_NURBS; i++) {
        if (s->urbs[i]) usb_kill_urb(s->urbs[i]);
    }
    return 0; 
}

static int ns6_pcm_prepare(struct snd_pcm_substream *sub) {
    struct ns6 *ns6 = snd_pcm_substream_chip(sub);
    struct ns6_stream *s = (sub->stream == SNDRV_PCM_STREAM_PLAYBACK) ? &ns6->play : &ns6->cap;
    unsigned long flags;
    int i, err;
    
    spin_lock_irqsave(&s->lock, flags);
    s->hwptr = 0; s->period_pos = 0; s->phase = 0;
    spin_unlock_irqrestore(&s->lock, flags);

    /* O Mixxx chama prepare várias vezes. Só ligamos a força 1 vez para não dar Panic! */
    if (!s->active_usb) {
        s->active_usb = true;
        for (i = 0; i < NS6_NURBS; i++) {
            if (s->urbs[i]) {
                err = usb_submit_urb(s->urbs[i], GFP_KERNEL);
                if (err) pr_err("NS6: Falha ao ligar URB %d (%d)\n", i, err);
            }
        }
    }
    return 0;
}

static int ns6_pcm_trigger(struct snd_pcm_substream *sub, int cmd) {
    struct ns6 *ns6 = snd_pcm_substream_chip(sub);
    struct ns6_stream *s = (sub->stream == SNDRV_PCM_STREAM_PLAYBACK) ? &ns6->play : &ns6->cap;
    unsigned long flags;
    
    /* Trigger atômico (Gatilho ultra-rápido): Apenas libera o fluxo do som! */
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

static snd_pcm_uframes_t ns6_pcm_pointer(struct snd_pcm_substream *sub) {
    struct ns6 *ns6 = snd_pcm_substream_chip(sub);
    return (sub->stream == SNDRV_PCM_STREAM_PLAYBACK) ? ns6->play.hwptr : ns6->cap.hwptr;
}

static const struct snd_pcm_ops ns6_pcm_ops = {
    .open = ns6_pcm_open, .close = ns6_pcm_close, .hw_params = ns6_pcm_hw_params, .hw_free = ns6_pcm_hw_free,
    .prepare = ns6_pcm_prepare, .trigger = ns6_pcm_trigger, .pointer = ns6_pcm_pointer,
};

/* ========================================================================= *
 * URB ALLOCATION & MEMORY
 * ========================================================================= */
static int ns6_alloc_iso_urbs(struct ns6 *ns6, struct ns6_stream *s, struct usb_endpoint_descriptor *epd, void (*cb)(struct urb *)) {
    int i, j;
    if (!epd) return -ENODEV;
    
    u8 ep = epd->bEndpointAddress;
    unsigned int pipe = (ep & USB_DIR_IN) ? usb_rcvisocpipe(ns6->udev, ep & 0x7f) : usb_sndisocpipe(ns6->udev, ep & 0x7f);
    unsigned int pkt_size = (ep == NS6_EP_PLAY) ? NS6_PKT_SIZE : (NS6_FRAME_BYTES * 5); 
    unsigned int urb_bytes = NS6_PKTS_PER_URB * pkt_size;

    for (i = 0; i < NS6_NURBS; i++) {
        s->urbs[i] = usb_alloc_urb(NS6_PKTS_PER_URB, GFP_KERNEL);
        if (!s->urbs[i]) return -ENOMEM;

        s->bufs[i] = usb_alloc_coherent(ns6->udev, urb_bytes, GFP_KERNEL, &s->dma[i]);
        if (!s->bufs[i]) return -ENOMEM;

        s->urbs[i]->dev = ns6->udev;
        s->urbs[i]->pipe = pipe;
        s->urbs[i]->transfer_buffer = s->bufs[i];
        
        s->urbs[i]->transfer_dma = s->dma[i];
        s->urbs[i]->transfer_flags = URB_ISO_ASAP | URB_NO_TRANSFER_DMA_MAP;

        s->urbs[i]->transfer_buffer_length = urb_bytes;
        s->urbs[i]->number_of_packets = NS6_PKTS_PER_URB;
        s->urbs[i]->complete = cb;
        s->urbs[i]->context = ns6;
        s->urbs[i]->interval = 1;

        for (j = 0; j < NS6_PKTS_PER_URB; j++) {
            s->urbs[i]->iso_frame_desc[j].offset = j * pkt_size;
            s->urbs[i]->iso_frame_desc[j].length = pkt_size;
        }
    }
    return 0;
}

static void ns6_free_iso_urbs(struct ns6 *ns6, struct ns6_stream *s, u8 ep) {
    int i;
    unsigned int pkt_size = (ep == NS6_EP_PLAY) ? NS6_PKT_SIZE : (NS6_FRAME_BYTES * 5); 
    unsigned int urb_bytes = NS6_PKTS_PER_URB * pkt_size;

    for (i = 0; i < NS6_NURBS; i++) {
        if (s->urbs[i]) {
            usb_kill_urb(s->urbs[i]);
            if (s->bufs[i]) usb_free_coherent(ns6->udev, urb_bytes, s->bufs[i], s->dma[i]);
            usb_free_urb(s->urbs[i]); s->urbs[i] = NULL;
        }
    }
}

/* ========================================================================= *
 * PROBE & DISCONNECT
 * ========================================================================= */
static int ns6_probe(struct usb_interface *intf, const struct usb_device_id *id) {
    struct usb_device *udev = interface_to_usbdev(intf);
    struct ns6 *ns6;
    struct snd_card *card;
    struct snd_pcm *pcm;
    int i, err;

    if (intf->cur_altsetting->desc.bInterfaceNumber != 0) return -ENODEV;

    err = snd_card_new(&intf->dev, -1, "NS6", THIS_MODULE, sizeof(*ns6), &card);
    if (err) return err;

    ns6 = card->private_data;
    ns6->udev = udev;
    ns6->card = card;

    usb_set_intfdata(intf, ns6);

    spin_lock_init(&ns6->play.lock);
    spin_lock_init(&ns6->cap.lock);
    spin_lock_init(&ns6->midi_out_lock);

    strscpy(card->driver, DRV_NAME, sizeof(card->driver));
    strscpy(card->shortname, "Numark NS6");
    strscpy(card->longname, "Numark NS6 DJ Controller");

    ns6->ep_play = ns6_get_ep_desc(udev, NS6_EP_PLAY);
    ns6->ep_cap  = ns6_get_ep_desc(udev, NS6_EP_CAP);
    ns6->ep_midi_in = ns6_get_ep_desc(udev, NS6_EP_MIDI_IN);
    ns6->ep_midi_out = ns6_get_ep_desc(udev, NS6_EP_MIDI_OUT);

    if (!ns6->ep_midi_in || !ns6->ep_midi_out) {
        pr_err("NS6: Faltam endpoints MIDI! Verifique o hardware.\n");
        err = -ENODEV;
        goto err_free_card;
    }

    err = ns6_alloc_iso_urbs(ns6, &ns6->play, ns6->ep_play, ns6_play_urb_cb);
    if (err) goto err_free_play;

    err = ns6_alloc_iso_urbs(ns6, &ns6->cap, ns6->ep_cap, ns6_cap_urb_cb);
    if (err) goto err_free_cap;

    for (i = 0; i < NS6_MIDI_NURBS; i++) {
        ns6->midi_in_urbs[i] = usb_alloc_urb(0, GFP_KERNEL);
        if (!ns6->midi_in_urbs[i]) { err = -ENOMEM; goto err_free_midi; }
        
        if (usb_endpoint_xfer_int(ns6->ep_midi_in)) {
            usb_fill_int_urb(ns6->midi_in_urbs[i], udev, usb_rcvintpipe(udev, NS6_EP_MIDI_IN),
                             ns6->midi_in_buf[i], NS6_MIDI_PKT_SIZE, ns6_midi_in_urb_cb, ns6, ns6->ep_midi_in->bInterval);
        } else {
            usb_fill_bulk_urb(ns6->midi_in_urbs[i], udev, usb_rcvbulkpipe(udev, NS6_EP_MIDI_IN),
                              ns6->midi_in_buf[i], NS6_MIDI_PKT_SIZE, ns6_midi_in_urb_cb, ns6);
        }
        usb_submit_urb(ns6->midi_in_urbs[i], GFP_KERNEL);
    }

    ns6->midi_out_urb = usb_alloc_urb(0, GFP_KERNEL);
    if (!ns6->midi_out_urb) { err = -ENOMEM; goto err_free_midi; }

    err = snd_pcm_new(card, "NS6 Audio", 0, 1, 1, &pcm);
    if (err) goto err_free_pcm;

    ns6->pcm = pcm;
    pcm->private_data = ns6;
    strscpy(pcm->name, "Numark NS6");
    snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &ns6_pcm_ops);
    snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &ns6_pcm_ops);
    snd_pcm_set_managed_buffer_all(pcm, SNDRV_DMA_TYPE_VMALLOC, NULL, 0, 0);

    err = snd_rawmidi_new(card, "NS6 MIDI", 0, 1, 1, &ns6->rmidi);
    if (err) goto err_free_pcm;

    ns6->rmidi->private_data = ns6;
    strscpy(ns6->rmidi->name, "Numark NS6 MIDI");
    snd_rawmidi_set_ops(ns6->rmidi, SNDRV_RAWMIDI_STREAM_INPUT, &ns6_midi_in_ops);
    snd_rawmidi_set_ops(ns6->rmidi, SNDRV_RAWMIDI_STREAM_OUTPUT, &ns6_midi_out_ops);
    ns6->rmidi->info_flags = SNDRV_RAWMIDI_INFO_OUTPUT | SNDRV_RAWMIDI_INFO_INPUT | SNDRV_RAWMIDI_INFO_DUPLEX;

    err = snd_card_register(card);
    if (err) goto err_free_pcm;

    INIT_WORK(&ns6->init_work, ns6_deferred_init);
    schedule_work(&ns6->init_work);

    return 0;

err_free_pcm:
    if (ns6->midi_out_urb) { usb_kill_urb(ns6->midi_out_urb); usb_free_urb(ns6->midi_out_urb); }
err_free_midi:
    for (i = 0; i < NS6_MIDI_NURBS; i++) {
        if (ns6->midi_in_urbs[i]) { usb_kill_urb(ns6->midi_in_urbs[i]); usb_free_urb(ns6->midi_in_urbs[i]); }
    }
err_free_cap:
    ns6_free_iso_urbs(ns6, &ns6->cap, NS6_EP_CAP);
err_free_play:
    ns6_free_iso_urbs(ns6, &ns6->play, NS6_EP_PLAY);
err_free_card:
    usb_set_intfdata(intf, NULL);
    snd_card_free(card);
    return err;
}

static void ns6_disconnect(struct usb_interface *intf) {
    struct ns6 *ns6 = usb_get_intfdata(intf);
    int i;

    if (!ns6) return;

    cancel_work_sync(&ns6->init_work);

    ns6->play.active_usb = false;
    ns6->cap.active_usb  = false;

    for (i = 0; i < NS6_MIDI_NURBS; i++) {
        if (ns6->midi_in_urbs[i]) usb_kill_urb(ns6->midi_in_urbs[i]);
    }
    if (ns6->midi_out_urb) usb_kill_urb(ns6->midi_out_urb);

    snd_card_free(ns6->card);

    ns6_free_iso_urbs(ns6, &ns6->play, NS6_EP_PLAY);
    ns6_free_iso_urbs(ns6, &ns6->cap, NS6_EP_CAP);

    for (i = 0; i < NS6_MIDI_NURBS; i++) {
        if (ns6->midi_in_urbs[i]) usb_free_urb(ns6->midi_in_urbs[i]);
    }
    if (ns6->midi_out_urb) usb_free_urb(ns6->midi_out_urb);

    usb_set_intfdata(intf, NULL);
}

static const struct usb_device_id ns6_id_table[] = { { USB_DEVICE_INTERFACE_NUMBER(NS6_VID, NS6_PID, 0) }, { } };
MODULE_DEVICE_TABLE(usb, ns6_id_table);
static struct usb_driver ns6_driver = { .name = DRV_NAME, .probe = ns6_probe, .disconnect = ns6_disconnect, .id_table = ns6_id_table, };
module_usb_driver(ns6_driver);
MODULE_AUTHOR("ns6d project"); MODULE_DESCRIPTION("Numark NS6 complete USB driver"); MODULE_LICENSE("GPL v2");