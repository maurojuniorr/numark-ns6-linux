// SPDX-License-Identifier: GPL-2.0
/*
 * snd-ns6-audio.c — Numark NS6 USB audio kernel module
 *
 * Handles only Interface 1 (ISO audio):
 *   EP 0x02 OUT — PCM playback  (host → device), 24-bit 4ch 44100Hz
 *   EP 0x81 IN  — PCM capture   (device → host), 24-bit 4ch 44100Hz
 *
 * MIDI (Interface 0, bulk EPs) remains handled by ns6d userspace daemon.
 *
 * Build:  make
 * Load:   sudo insmod snd-ns6-audio.ko
 * Unload: sudo rmmod snd-ns6-audio
 */

#include <linux/module.h>
#include <linux/usb.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/initval.h>

#define DRV_NAME        "snd-ns6-audio"
#define DRV_VERSION     "0.1"

#define NS6_VID         0x15e4
#define NS6_PID         0x0079

/* Audio interface */
#define NS6_IFACE_AUDIO     1
#define NS6_IFACE_ALT       1

/* ISO endpoints */
#define NS6_EP_PLAY         0x02   /* OUT — playback */
#define NS6_EP_CAP          0x81   /* IN  — capture  */

/* PCM format */
#define NS6_RATE            44100
#define NS6_CHANNELS        4
#define NS6_SAMPLE_BYTES    3      /* S24_3LE */
#define NS6_FRAME_BYTES     (NS6_CHANNELS * NS6_SAMPLE_BYTES)  /* 12 */
#define NS6_FRAMES_PER_URB  176    /* 4ms @ 44100Hz */
#define NS6_URB_BYTES       (NS6_FRAMES_PER_URB * NS6_FRAME_BYTES)  /* 2112 */
#define NS6_NURBS           4      /* concurrent URBs per direction */

/* ------------------------------------------------------------------ */
/* Per-direction (playback/capture) state                              */
/* ------------------------------------------------------------------ */
struct ns6_stream {
    struct urb          *urbs[NS6_NURBS];
    unsigned char       *bufs[NS6_NURBS];
    dma_addr_t           dma[NS6_NURBS];

    struct snd_pcm_substream *substream;
    spinlock_t           lock;

    unsigned int         hwptr;        /* frames written/read in DMA buf */
    unsigned int         period_pos;   /* frames into current period     */
    bool                 running;
};

/* ------------------------------------------------------------------ */
/* Main device structure                                               */
/* ------------------------------------------------------------------ */
struct ns6_audio {
    struct usb_device   *udev;
    struct snd_card     *card;
    struct snd_pcm      *pcm;

    struct ns6_stream    play;
    struct ns6_stream    cap;

    /* DMA-coherent PCM ring buffer */
    unsigned char       *play_buf;
    dma_addr_t           play_dma;
    unsigned char       *cap_buf;
    dma_addr_t           cap_dma;
    unsigned int         buf_bytes;
};

/* ------------------------------------------------------------------ */
/* PCM hardware definition                                             */
/* ------------------------------------------------------------------ */
static const struct snd_pcm_hardware ns6_hw = {
    .info               = SNDRV_PCM_INFO_INTERLEAVED |
                          SNDRV_PCM_INFO_BLOCK_TRANSFER |
                          SNDRV_PCM_INFO_MMAP |
                          SNDRV_PCM_INFO_MMAP_VALID,
    .formats            = SNDRV_PCM_FMTBIT_S24_3LE,
    .rates              = SNDRV_PCM_RATE_44100,
    .rate_min           = NS6_RATE,
    .rate_max           = NS6_RATE,
    .channels_min       = NS6_CHANNELS,
    .channels_max       = NS6_CHANNELS,
    .buffer_bytes_max   = NS6_URB_BYTES * NS6_NURBS * 4,
    .period_bytes_min   = NS6_URB_BYTES,
    .period_bytes_max   = NS6_URB_BYTES * NS6_NURBS,
    .periods_min        = 2,
    .periods_max        = 16,
};

/* ------------------------------------------------------------------ */
/* Playback URB callback                                               */
/* ------------------------------------------------------------------ */
static void ns6_play_urb_complete(struct urb *urb)
{
    struct ns6_audio *ns6 = urb->context;
    struct ns6_stream *s  = &ns6->play;
    struct snd_pcm_substream *sub;
    unsigned long flags;
    unsigned int frames, buf_frames;
    int err;

    if (urb->status == -ENOENT || urb->status == -ECONNRESET ||
        urb->status == -ESHUTDOWN)
        return;

    spin_lock_irqsave(&s->lock, flags);

    sub = s->substream;
    if (!sub || !s->running) {
        spin_unlock_irqrestore(&s->lock, flags);
        return;
    }

    buf_frames = snd_pcm_lib_buffer_bytes(sub) / NS6_FRAME_BYTES;
    frames     = NS6_FRAMES_PER_URB;

    /* Copy from PCM ring buffer into URB */
    if (s->hwptr + frames <= buf_frames) {
        memcpy(urb->transfer_buffer,
               ns6->play_buf + s->hwptr * NS6_FRAME_BYTES,
               frames * NS6_FRAME_BYTES);
    } else {
        unsigned int first = buf_frames - s->hwptr;
        memcpy(urb->transfer_buffer,
               ns6->play_buf + s->hwptr * NS6_FRAME_BYTES,
               first * NS6_FRAME_BYTES);
        memcpy(urb->transfer_buffer + first * NS6_FRAME_BYTES,
               ns6->play_buf,
               (frames - first) * NS6_FRAME_BYTES);
    }

    s->hwptr = (s->hwptr + frames) % buf_frames;
    s->period_pos += frames;

    if (s->period_pos >= snd_pcm_lib_period_bytes(sub) / NS6_FRAME_BYTES) {
        s->period_pos = 0;
        spin_unlock_irqrestore(&s->lock, flags);
        snd_pcm_period_elapsed(sub);
    } else {
        spin_unlock_irqrestore(&s->lock, flags);
    }

    err = usb_submit_urb(urb, GFP_ATOMIC);
    if (err && err != -ENODEV)
        dev_err(&ns6->udev->dev, "play urb resubmit: %d\n", err);
}

/* ------------------------------------------------------------------ */
/* Capture URB callback                                                */
/* ------------------------------------------------------------------ */
static void ns6_cap_urb_complete(struct urb *urb)
{
    struct ns6_audio *ns6 = urb->context;
    struct ns6_stream *s  = &ns6->cap;
    struct snd_pcm_substream *sub;
    unsigned long flags;
    unsigned int frames, buf_frames;
    int err;

    if (urb->status == -ENOENT || urb->status == -ECONNRESET ||
        urb->status == -ESHUTDOWN)
        return;

    spin_lock_irqsave(&s->lock, flags);

    sub = s->substream;
    if (!sub || !s->running) {
        spin_unlock_irqrestore(&s->lock, flags);
        goto resubmit;
    }

    buf_frames = snd_pcm_lib_buffer_bytes(sub) / NS6_FRAME_BYTES;
    frames     = urb->actual_length / NS6_FRAME_BYTES;
    if (frames > NS6_FRAMES_PER_URB)
        frames = NS6_FRAMES_PER_URB;

    /* Copy from URB into PCM ring buffer */
    if (s->hwptr + frames <= buf_frames) {
        memcpy(ns6->cap_buf + s->hwptr * NS6_FRAME_BYTES,
               urb->transfer_buffer,
               frames * NS6_FRAME_BYTES);
    } else {
        unsigned int first = buf_frames - s->hwptr;
        memcpy(ns6->cap_buf + s->hwptr * NS6_FRAME_BYTES,
               urb->transfer_buffer,
               first * NS6_FRAME_BYTES);
        memcpy(ns6->cap_buf,
               urb->transfer_buffer + first * NS6_FRAME_BYTES,
               (frames - first) * NS6_FRAME_BYTES);
    }

    s->hwptr = (s->hwptr + frames) % buf_frames;
    s->period_pos += frames;

    if (s->period_pos >= snd_pcm_lib_period_bytes(sub) / NS6_FRAME_BYTES) {
        s->period_pos = 0;
        spin_unlock_irqrestore(&s->lock, flags);
        snd_pcm_period_elapsed(sub);
    } else {
        spin_unlock_irqrestore(&s->lock, flags);
    }

resubmit:
    err = usb_submit_urb(urb, GFP_ATOMIC);
    if (err && err != -ENODEV)
        dev_err(&ns6->udev->dev, "cap urb resubmit: %d\n", err);
}

/* ------------------------------------------------------------------ */
/* Allocate and init URBs for a stream                                 */
/* ------------------------------------------------------------------ */
static int ns6_alloc_urbs(struct ns6_audio *ns6, struct ns6_stream *s,
                          u8 ep, usb_complete_t complete)
{
    int i;
    unsigned int pipe = usb_pipein(ep << 0) ?
        usb_rcvisocpipe(ns6->udev, ep & 0x7f) :
        usb_sndisocpipe(ns6->udev, ep & 0x7f);

    /* For OUT ep the pipe direction needs to be snd */
    if (!(ep & USB_DIR_IN))
        pipe = usb_sndisocpipe(ns6->udev, ep & 0x7f);
    else
        pipe = usb_rcvisocpipe(ns6->udev, ep & 0x7f);

    for (i = 0; i < NS6_NURBS; i++) {
        s->urbs[i] = usb_alloc_urb(1, GFP_KERNEL);
        if (!s->urbs[i])
            return -ENOMEM;

        s->bufs[i] = usb_alloc_coherent(ns6->udev, NS6_URB_BYTES,
                                         GFP_KERNEL, &s->dma[i]);
        if (!s->bufs[i])
            return -ENOMEM;

        s->urbs[i]->dev             = ns6->udev;
        s->urbs[i]->pipe            = pipe;
        s->urbs[i]->transfer_buffer = s->bufs[i];
        s->urbs[i]->transfer_buffer_length = NS6_URB_BYTES;
        s->urbs[i]->number_of_packets = 1;
        s->urbs[i]->complete        = complete;
        s->urbs[i]->context         = ns6;
        s->urbs[i]->interval        = 1;
        s->urbs[i]->iso_frame_desc[0].offset = 0;
        s->urbs[i]->iso_frame_desc[0].length = NS6_URB_BYTES;
        s->urbs[i]->transfer_flags  = URB_ISO_ASAP;
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

static int ns6_start_urbs(struct ns6_stream *s)
{
    int i, err;
    for (i = 0; i < NS6_NURBS; i++) {
        err = usb_submit_urb(s->urbs[i], GFP_KERNEL);
        if (err)
            return err;
    }
    return 0;
}

static void ns6_stop_urbs(struct ns6_stream *s)
{
    int i;
    for (i = 0; i < NS6_NURBS; i++)
        usb_kill_urb(s->urbs[i]);
}

/* ------------------------------------------------------------------ */
/* PCM ops — playback                                                  */
/* ------------------------------------------------------------------ */
static int ns6_play_open(struct snd_pcm_substream *sub)
{
    struct ns6_audio *ns6 = snd_pcm_substream_chip(sub);
    int err;

    sub->runtime->hw = ns6_hw;

    /* Força 44100Hz — hardware não suporta outras taxas */
    err = snd_pcm_hw_constraint_single(sub->runtime,
                                        SNDRV_PCM_HW_PARAM_RATE,
                                        NS6_RATE);
    if (err < 0) return err;

    err = snd_pcm_hw_constraint_single(sub->runtime,
                                        SNDRV_PCM_HW_PARAM_CHANNELS,
                                        NS6_CHANNELS);
    if (err < 0) return err;

    ns6->play.substream = sub;
    return 0;
}

static int ns6_play_close(struct snd_pcm_substream *sub)
{
    struct ns6_audio *ns6 = snd_pcm_substream_chip(sub);
    ns6->play.substream = NULL;
    return 0;
}

static int ns6_play_prepare(struct snd_pcm_substream *sub)
{
    struct ns6_audio *ns6 = snd_pcm_substream_chip(sub);
    ns6->play.hwptr     = 0;
    ns6->play.period_pos = 0;
    return 0;
}

static int ns6_play_trigger(struct snd_pcm_substream *sub, int cmd)
{
    struct ns6_audio *ns6 = snd_pcm_substream_chip(sub);
    struct ns6_stream *s  = &ns6->play;
    unsigned long flags;

    switch (cmd) {
    case SNDRV_PCM_TRIGGER_START:
        spin_lock_irqsave(&s->lock, flags);
        s->running = true;
        spin_unlock_irqrestore(&s->lock, flags);
        return ns6_start_urbs(s);
    case SNDRV_PCM_TRIGGER_STOP:
        spin_lock_irqsave(&s->lock, flags);
        s->running = false;
        spin_unlock_irqrestore(&s->lock, flags);
        ns6_stop_urbs(s);
        return 0;
    }
    return -EINVAL;
}

static snd_pcm_uframes_t ns6_play_pointer(struct snd_pcm_substream *sub)
{
    struct ns6_audio *ns6 = snd_pcm_substream_chip(sub);
    return ns6->play.hwptr;
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

static const struct snd_pcm_ops ns6_play_ops = {
    .open      = ns6_play_open,
    .close     = ns6_play_close,
    .hw_params = ns6_pcm_hw_params,
    .hw_free   = ns6_pcm_hw_free,
    .prepare   = ns6_play_prepare,
    .trigger   = ns6_play_trigger,
    .pointer   = ns6_play_pointer,
};

/* ------------------------------------------------------------------ */
/* PCM ops — capture                                                   */
/* ------------------------------------------------------------------ */
static int ns6_cap_open(struct snd_pcm_substream *sub)
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

    ns6->cap.substream = sub;
    return 0;
}

static int ns6_cap_close(struct snd_pcm_substream *sub)
{
    struct ns6_audio *ns6 = snd_pcm_substream_chip(sub);
    ns6->cap.substream = NULL;
    return 0;
}

static int ns6_cap_prepare(struct snd_pcm_substream *sub)
{
    struct ns6_audio *ns6 = snd_pcm_substream_chip(sub);
    ns6->cap.hwptr     = 0;
    ns6->cap.period_pos = 0;
    return 0;
}

static int ns6_cap_trigger(struct snd_pcm_substream *sub, int cmd)
{
    struct ns6_audio *ns6 = snd_pcm_substream_chip(sub);
    struct ns6_stream *s  = &ns6->cap;
    unsigned long flags;

    switch (cmd) {
    case SNDRV_PCM_TRIGGER_START:
        spin_lock_irqsave(&s->lock, flags);
        s->running = true;
        spin_unlock_irqrestore(&s->lock, flags);
        return ns6_start_urbs(s);
    case SNDRV_PCM_TRIGGER_STOP:
        spin_lock_irqsave(&s->lock, flags);
        s->running = false;
        spin_unlock_irqrestore(&s->lock, flags);
        ns6_stop_urbs(s);
        return 0;
    }
    return -EINVAL;
}

static snd_pcm_uframes_t ns6_cap_pointer(struct snd_pcm_substream *sub)
{
    struct ns6_audio *ns6 = snd_pcm_substream_chip(sub);
    return ns6->cap.hwptr;
}

static const struct snd_pcm_ops ns6_cap_ops = {
    .open      = ns6_cap_open,
    .close     = ns6_cap_close,
    .hw_params = ns6_pcm_hw_params,
    .hw_free   = ns6_pcm_hw_free,
    .prepare   = ns6_cap_prepare,
    .trigger   = ns6_cap_trigger,
    .pointer   = ns6_cap_pointer,
};

/* ------------------------------------------------------------------ */
/* USB probe                                                           */
/* ------------------------------------------------------------------ */
static int ns6_probe(struct usb_interface *intf,
                     const struct usb_device_id *id)
{
    struct usb_device *udev = interface_to_usbdev(intf);
    struct ns6_audio  *ns6;
    struct snd_card   *card;
    struct snd_pcm    *pcm;
    int err;

    /* Only claim Interface 1 (audio ISO) */
    if (intf->cur_altsetting->desc.bInterfaceNumber != NS6_IFACE_AUDIO)
        return -ENODEV;

    dev_info(&intf->dev, "Numark NS6 audio interface found\n");

    /* Switch to alternate setting 1 (ISO endpoints active) */
    err = usb_set_interface(udev, NS6_IFACE_AUDIO, NS6_IFACE_ALT);
    if (err) {
        dev_err(&intf->dev, "set_interface failed: %d\n", err);
        return err;
    }

    /* Create ALSA card */
    err = snd_card_new(&intf->dev, SNDRV_DEFAULT_IDX1, "NS6",
                       THIS_MODULE, sizeof(*ns6), &card);
    if (err)
        return err;

    ns6       = card->private_data;
    ns6->udev = udev;
    ns6->card = card;

    spin_lock_init(&ns6->play.lock);
    spin_lock_init(&ns6->cap.lock);

    strscpy(card->driver,   DRV_NAME,    sizeof(card->driver));
    strscpy(card->shortname, "Numark NS6", sizeof(card->shortname));
    strscpy(card->longname,  "Numark NS6 DJ Controller", sizeof(card->longname));

    /* Allocate PCM ring buffers (vmalloc — no DMA needed, USB handles it) */
    ns6->buf_bytes = ns6_hw.buffer_bytes_max;
    ns6->play_buf  = vzalloc(ns6->buf_bytes);
    ns6->cap_buf   = vzalloc(ns6->buf_bytes);
    if (!ns6->play_buf || !ns6->cap_buf) {
        err = -ENOMEM;
        goto err_free_bufs;
    }

    /* Allocate ISO URBs */
    err = ns6_alloc_urbs(ns6, &ns6->play, NS6_EP_PLAY, ns6_play_urb_complete);
    if (err) goto err_free_urbs;

    err = ns6_alloc_urbs(ns6, &ns6->cap, NS6_EP_CAP, ns6_cap_urb_complete);
    if (err) goto err_free_urbs;

    /* Create PCM device */
    err = snd_pcm_new(card, "NS6 Audio", 0, 1, 1, &pcm);
    if (err) goto err_free_urbs;

    ns6->pcm = pcm;
    pcm->private_data = ns6;
    strscpy(pcm->name, "Numark NS6", sizeof(pcm->name));

    snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &ns6_play_ops);
    snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE,  &ns6_cap_ops);

    /* Use vmalloc buffer */
    snd_pcm_set_managed_buffer_all(pcm, SNDRV_DMA_TYPE_VMALLOC,
                                   NULL, 0, 0);

    err = snd_card_register(card);
    if (err) goto err_free_urbs;

    usb_set_intfdata(intf, ns6);

    dev_info(&intf->dev, "Numark NS6 audio registered as card %d\n",
             card->number);
    return 0;

err_free_urbs:
    ns6_free_urbs(ns6, &ns6->play);
    ns6_free_urbs(ns6, &ns6->cap);
err_free_bufs:
    vfree(ns6->play_buf);
    vfree(ns6->cap_buf);
    snd_card_free(card);
    return err;
}

static void ns6_disconnect(struct usb_interface *intf)
{
    struct ns6_audio *ns6 = usb_get_intfdata(intf);
    if (!ns6) return;

    ns6_stop_urbs(&ns6->play);
    ns6_stop_urbs(&ns6->cap);
    ns6_free_urbs(ns6, &ns6->play);
    ns6_free_urbs(ns6, &ns6->cap);

    vfree(ns6->play_buf);
    vfree(ns6->cap_buf);

    snd_card_free(ns6->card);
    dev_info(&intf->dev, "Numark NS6 audio disconnected\n");
}

/* ------------------------------------------------------------------ */
/* Module metadata                                                     */
/* ------------------------------------------------------------------ */
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
MODULE_VERSION(DRV_VERSION);
