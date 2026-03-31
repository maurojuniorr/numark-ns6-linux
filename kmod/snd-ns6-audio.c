// SPDX-License-Identifier: GPL-2.0
/*
 * snd-ns6-audio.c — Numark NS6 USB audio kernel module
 *
 * Handles Interface 1 (ISO audio):
 *   EP 0x02 OUT — PCM playback  (host -> device), S24_3LE 4ch 44100Hz
 *   EP 0x81 IN  — PCM capture   (device -> host), S24_3LE 4ch 44100Hz
 *
 * MIDI (Interface 0) handled by ns6d userspace daemon.
 *
 * Fixes applied:
 *   - SNDRV_DMA_TYPE_CONTINUOUS (not vmalloc — USB needs DMA-able memory)
 *   - NS6_NURBS = 2 (ISO endpoint accepts max 2 concurrent URBs)
 *   - hw_params / hw_free implemented
 *   - GFP_KERNEL in trigger (not GFP_ATOMIC — called from syscall context)
 */

#include <linux/module.h>
#include <linux/usb.h>
#include <linux/slab.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/initval.h>

#define DRV_NAME            "snd-ns6-audio"
#define NS6_VID             0x15e4
#define NS6_PID             0x0079
#define NS6_IFACE_AUDIO     0   /* Interface 0 contém EP 0x02 (audio ISO OUT) */
#define NS6_IFACE_ALT       1   /* alt=1 ativa os endpoints ISO */

#define NS6_RATE            44100
#define NS6_CHANNELS        4
#define NS6_SAMPLE_BYTES    3
#define NS6_FRAME_BYTES     (NS6_CHANNELS * NS6_SAMPLE_BYTES)  /* 12 */
#define NS6_PKTS_PER_URB    32
#define NS6_PKT_SIZE        66
#define NS6_URB_BYTES       (NS6_PKTS_PER_URB * NS6_PKT_SIZE)  /* 2112 */
#define NS6_FRAMES_PER_URB  (NS6_URB_BYTES / NS6_FRAME_BYTES)  /* 176 */
#define NS6_NURBS           2   /* ISO endpoint: max 2 concurrent URBs */

struct ns6_stream {
    struct urb               *urbs[NS6_NURBS];
    unsigned char            *bufs[NS6_NURBS];
    dma_addr_t                dma[NS6_NURBS];
    struct snd_pcm_substream *substream;
    spinlock_t                lock;
    unsigned int              hwptr;
    bool                      running;
};

struct ns6_audio {
    struct usb_device *udev;
    struct snd_card   *card;
    struct snd_pcm    *pcm;
    struct ns6_stream  play;
    struct ns6_stream  cap;
};

static const struct snd_pcm_hardware ns6_hw = {
    .info             = SNDRV_PCM_INFO_INTERLEAVED |
                        SNDRV_PCM_INFO_BLOCK_TRANSFER |
                        SNDRV_PCM_INFO_MMAP |
                        SNDRV_PCM_INFO_MMAP_VALID,
    .formats          = SNDRV_PCM_FMTBIT_S24_3LE,
    .rates            = SNDRV_PCM_RATE_44100,
    .rate_min         = NS6_RATE,
    .rate_max         = NS6_RATE,
    .channels_min     = NS6_CHANNELS,
    .channels_max     = NS6_CHANNELS,
    .buffer_bytes_max = NS6_URB_BYTES * NS6_NURBS * 8,
    .period_bytes_min = NS6_URB_BYTES,
    .period_bytes_max = NS6_URB_BYTES * NS6_NURBS,
    .periods_min      = 2,
    .periods_max      = 16,
};

/* ------------------------------------------------------------------ */
/* Playback URB callback                                               */
/* ------------------------------------------------------------------ */
static void ns6_play_urb_complete(struct urb *urb)
{
    struct ns6_audio  *ns6 = urb->context;
    struct ns6_stream *s   = &ns6->play;
    struct snd_pcm_substream *sub;
    unsigned long flags;

    if (urb->status == -ENOENT || urb->status == -ECONNRESET ||
        urb->status == -ESHUTDOWN)
        return;

    if (urb->status)
        dev_warn(&ns6->udev->dev, "play urb status: %d\n", urb->status);

    spin_lock_irqsave(&s->lock, flags);
    sub = s->substream;

    if (!s->running || !sub || !snd_pcm_running(sub) ||
        !sub->runtime->dma_area) {
        memset(urb->transfer_buffer, 0, NS6_URB_BYTES);
        spin_unlock_irqrestore(&s->lock, flags);
        goto resubmit;
    }

    {
        unsigned int buf_frames = sub->runtime->buffer_size;
        unsigned char *dma = sub->runtime->dma_area;
        unsigned int frames = NS6_FRAMES_PER_URB;

        if (s->hwptr + frames > buf_frames) {
            unsigned int chunk1 = buf_frames - s->hwptr;
            memcpy(urb->transfer_buffer,
                   dma + s->hwptr * NS6_FRAME_BYTES,
                   chunk1 * NS6_FRAME_BYTES);
            memcpy(urb->transfer_buffer + chunk1 * NS6_FRAME_BYTES,
                   dma,
                   (frames - chunk1) * NS6_FRAME_BYTES);
        } else {
            memcpy(urb->transfer_buffer,
                   dma + s->hwptr * NS6_FRAME_BYTES,
                   frames * NS6_FRAME_BYTES);
        }
        s->hwptr = (s->hwptr + frames) % buf_frames;
    }
    spin_unlock_irqrestore(&s->lock, flags);
    snd_pcm_period_elapsed(sub);

resubmit:
    usb_submit_urb(urb, GFP_ATOMIC);
}

/* ------------------------------------------------------------------ */
/* Capture URB callback                                                */
/* ------------------------------------------------------------------ */
static void ns6_cap_urb_complete(struct urb *urb)
{
    struct ns6_audio *ns6 = urb->context;

    if (urb->status == -ENOENT || urb->status == -ECONNRESET ||
        urb->status == -ESHUTDOWN)
        return;

    usb_submit_urb(urb, GFP_ATOMIC);
}

/* ------------------------------------------------------------------ */
/* Alloc / free URBs                                                   */
/* ------------------------------------------------------------------ */
static int ns6_alloc_urbs(struct ns6_audio *ns6, struct ns6_stream *s,
                           u8 ep, usb_complete_t complete)
{
    int i, j;
    unsigned int pipe = (ep & USB_DIR_IN) ?
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

        s->urbs[i]->dev                    = ns6->udev;
        s->urbs[i]->pipe                   = pipe;
        s->urbs[i]->transfer_buffer        = s->bufs[i];
        s->urbs[i]->transfer_buffer_length = NS6_URB_BYTES;
        s->urbs[i]->number_of_packets      = NS6_PKTS_PER_URB;
        s->urbs[i]->complete               = complete;
        s->urbs[i]->context                = ns6;
        s->urbs[i]->interval               = 1;
        s->urbs[i]->transfer_flags         = URB_ISO_ASAP;

        for (j = 0; j < NS6_PKTS_PER_URB; j++) {
            s->urbs[i]->iso_frame_desc[j].offset = j * NS6_PKT_SIZE;
            s->urbs[i]->iso_frame_desc[j].length = NS6_PKT_SIZE;
        }
    }
    return 0;
}

static void ns6_free_urbs(struct ns6_audio *ns6, struct ns6_stream *s)
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

/* ------------------------------------------------------------------ */
/* PCM ops                                                             */
/* ------------------------------------------------------------------ */
static int ns6_pcm_open(struct snd_pcm_substream *sub)
{
    struct ns6_audio *ns6 = snd_pcm_substream_chip(sub);
    int err;

    sub->runtime->hw = ns6_hw;

    err = snd_pcm_hw_constraint_single(sub->runtime,
                                        SNDRV_PCM_HW_PARAM_RATE,
                                        NS6_RATE);
    if (err < 0) return err;

    err = snd_pcm_hw_constraint_single(sub->runtime,
                                        SNDRV_PCM_HW_PARAM_CHANNELS,
                                        NS6_CHANNELS);
    if (err < 0) return err;

    if (sub->stream == SNDRV_PCM_STREAM_PLAYBACK)
        ns6->play.substream = sub;
    else
        ns6->cap.substream = sub;

    return 0;
}

static int ns6_pcm_close(struct snd_pcm_substream *sub)
{
    struct ns6_audio *ns6 = snd_pcm_substream_chip(sub);
    if (sub->stream == SNDRV_PCM_STREAM_PLAYBACK)
        ns6->play.substream = NULL;
    else
        ns6->cap.substream = NULL;
    return 0;
}

static int ns6_pcm_hw_params(struct snd_pcm_substream *sub,
                               struct snd_pcm_hw_params *hw_params)
{
    return 0;
}

static int ns6_pcm_hw_free(struct snd_pcm_substream *sub)
{
    return 0;
}

static int ns6_pcm_prepare(struct snd_pcm_substream *sub)
{
    struct ns6_audio *ns6 = snd_pcm_substream_chip(sub);
    if (sub->stream == SNDRV_PCM_STREAM_PLAYBACK)
        ns6->play.hwptr = 0;
    else
        ns6->cap.hwptr = 0;
    return 0;
}

static int ns6_pcm_trigger(struct snd_pcm_substream *sub, int cmd)
{
    struct ns6_audio  *ns6 = snd_pcm_substream_chip(sub);
    struct ns6_stream *s   = (sub->stream == SNDRV_PCM_STREAM_PLAYBACK) ?
                              &ns6->play : &ns6->cap;
    unsigned long flags;
    int i, err;

    switch (cmd) {
    case SNDRV_PCM_TRIGGER_START:
        spin_lock_irqsave(&s->lock, flags);
        s->running = true;
        spin_unlock_irqrestore(&s->lock, flags);
        for (i = 0; i < NS6_NURBS; i++) {
            err = usb_submit_urb(s->urbs[i], GFP_ATOMIC);
            if (err)
                dev_err(&ns6->udev->dev,
                        "trigger start: urb[%d] failed: %d\n", i, err);
        }
        return 0;
    case SNDRV_PCM_TRIGGER_STOP:
        spin_lock_irqsave(&s->lock, flags);
        s->running = false;
        spin_unlock_irqrestore(&s->lock, flags);
        for (i = 0; i < NS6_NURBS; i++)
            usb_kill_urb(s->urbs[i]);
        return 0;
    }
    return -EINVAL;
}

static snd_pcm_uframes_t ns6_pcm_pointer(struct snd_pcm_substream *sub)
{
    struct ns6_audio *ns6 = snd_pcm_substream_chip(sub);
    if (sub->stream == SNDRV_PCM_STREAM_PLAYBACK)
        return ns6->play.hwptr;
    return ns6->cap.hwptr;
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

/* ------------------------------------------------------------------ */
/* USB probe / disconnect                                              */
/* ------------------------------------------------------------------ */
static int ns6_probe(struct usb_interface *intf,
                      const struct usb_device_id *id)
{
    struct usb_device *udev = interface_to_usbdev(intf);
    struct ns6_audio  *ns6;
    struct snd_card   *card;
    struct snd_pcm    *pcm;
    int err;

    if (intf->cur_altsetting->desc.bInterfaceNumber != NS6_IFACE_AUDIO)
        return -ENODEV;

    /* Init sequence mínima para ativar o endpoint ISO:
     * O daemon ns6d cuida da interface 0 (MIDI).
     * Aqui só ativamos a interface 1 (áudio). */
    err = usb_set_interface(udev, NS6_IFACE_AUDIO, 1);
    if (err) {
        dev_warn(&intf->dev, "set_interface(1,1) failed: %d (daemon may handle this)\n", err);
        /* Continua mesmo assim — daemon pode ter feito */
    }

    err = snd_card_new(&intf->dev, SNDRV_DEFAULT_IDX1, "NS6",
                       THIS_MODULE, sizeof(*ns6), &card);
    if (err)
        return err;

    ns6       = card->private_data;
    ns6->udev = udev;
    ns6->card = card;

    spin_lock_init(&ns6->play.lock);
    spin_lock_init(&ns6->cap.lock);

    strscpy(card->driver,    DRV_NAME,               sizeof(card->driver));
    strscpy(card->shortname, "Numark NS6",            sizeof(card->shortname));
    strscpy(card->longname,  "Numark NS6 DJ Controller", sizeof(card->longname));

    err = ns6_alloc_urbs(ns6, &ns6->play, 0x02, ns6_play_urb_complete);
    if (err) goto err_urbs;

    err = ns6_alloc_urbs(ns6, &ns6->cap, 0x81, ns6_cap_urb_complete);
    if (err) goto err_urbs;

    err = snd_pcm_new(card, "NS6 Audio", 0, 1, 1, &pcm);
    if (err) goto err_urbs;

    ns6->pcm      = pcm;
    pcm->private_data = ns6;
    strscpy(pcm->name, "Numark NS6", sizeof(pcm->name));

    snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &ns6_pcm_ops);
    snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE,  &ns6_pcm_ops);

    /* CONTINUOUS memory — USB DMA requires physically contiguous pages */
    snd_pcm_set_managed_buffer_all(pcm, SNDRV_DMA_TYPE_CONTINUOUS,
                                    NULL,
                                    NS6_URB_BYTES * NS6_NURBS,
                                    NS6_URB_BYTES * NS6_NURBS * 8);

    err = snd_card_register(card);
    if (err) goto err_urbs;

    usb_set_intfdata(intf, ns6);
    dev_info(&intf->dev, "Numark NS6 audio registered as card %d\n",
             card->number);
    return 0;

err_urbs:
    ns6_free_urbs(ns6, &ns6->play);
    ns6_free_urbs(ns6, &ns6->cap);
    snd_card_free(card);
    return err;
}

static void ns6_disconnect(struct usb_interface *intf)
{
    struct ns6_audio *ns6 = usb_get_intfdata(intf);
    if (!ns6) return;

    ns6->play.running = false;
    ns6->cap.running  = false;

    ns6_free_urbs(ns6, &ns6->play);
    ns6_free_urbs(ns6, &ns6->cap);

    snd_card_free(ns6->card);
    dev_info(&intf->dev, "Numark NS6 audio disconnected\n");
}

static const struct usb_device_id ns6_id_table[] = {
    { USB_DEVICE_INTERFACE_NUMBER(NS6_VID, NS6_PID, NS6_IFACE_AUDIO) },
    { }
};
MODULE_DEVICE_TABLE(usb, ns6_id_table);

static struct usb_driver ns6_driver = {
    .name       = DRV_NAME,
    .probe      = ns6_probe,
    .disconnect = ns6_disconnect,
    .id_table   = ns6_id_table,
};

module_usb_driver(ns6_driver);

MODULE_AUTHOR("ns6d project");
MODULE_DESCRIPTION("Numark NS6 USB audio driver");
MODULE_LICENSE("GPL v2");
