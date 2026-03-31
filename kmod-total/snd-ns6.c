// SPDX-License-Identifier: GPL-2.0
/*
 * snd-ns6.c — Numark NS6 complete USB driver
 * THE MASTERPIECE EDITION (Dynamic Sync, Smart MIDI & ALSA Clean State)
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

/* Endpoints (Interface 0, Alt 1) */
#define NS6_EP_PLAY     0x02
#define NS6_EP_MIDI_IN  0x83
#define NS6_EP_MIDI_OUT 0x04

/* Endpoint de Feedback Explícito (Interface 1, Alt 1) */
#define NS6_EP_SYNC     0x81

/* PCM format (Playback Only) */
#define NS6_RATE            44100
#define NS6_CHANNELS        4
#define NS6_SAMPLE_BYTES    3
#define NS6_FRAME_BYTES     (NS6_CHANNELS * NS6_SAMPLE_BYTES)  /* 12 */
#define NS6_PKTS_PER_URB    32
#define NS6_PKT_SIZE        156    /* wMaxPacketSize real do descriptor */
#define NS6_URB_BYTES       (NS6_PKTS_PER_URB * NS6_PKT_SIZE)  
#define NS6_NURBS           6

/* Configurações do EP Sync (0x81) */
#define NS6_SYNC_PKTS_PER_URB 16
#define NS6_SYNC_PKT_SIZE     64
#define NS6_SYNC_URB_BYTES    (NS6_SYNC_PKTS_PER_URB * NS6_SYNC_PKT_SIZE)

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
    bool                      active_usb; 
    bool                      active_pcm; 
    int                       warmup_cnt; 
    
    /* ACELERADOR DO ÁUDIO DINÂMICO */
    unsigned int              frac_acc;
    unsigned int              frac_num; /* Velocidade real do hardware */
};

struct ns6 {
    struct usb_device    *udev;
    struct snd_card      *card;
    struct snd_pcm       *pcm;
    struct ns6_stream     play;
    
    /* EP 0x81: URBs de feedback ISO (Apenas Leitura de Relógio) */
    struct urb           *sync_urbs[NS6_NURBS];
    unsigned char        *sync_bufs[NS6_NURBS];
    dma_addr_t            sync_dma[NS6_NURBS];
    bool                  sync_active;

    struct snd_rawmidi             *rmidi;
    struct snd_rawmidi_substream   *midi_in_sub;
    struct snd_rawmidi_substream   *midi_out_sub;

    struct urb           *midi_in_urbs[NS6_MIDI_NURBS];
    uint8_t               midi_in_buf[NS6_MIDI_NURBS][NS6_MIDI_PKT_SIZE];
    
    /* Pool BLINDADO de URBs de MIDI OUT */
    #define NS6_MIDI_OUT_NURBS  4
    struct urb           *midi_out_urbs[NS6_MIDI_OUT_NURBS];
    uint8_t               midi_out_bufs[NS6_MIDI_OUT_NURBS][NS6_MIDI_PKT_SIZE];
    bool                  midi_out_busy[NS6_MIDI_OUT_NURBS];
    spinlock_t            midi_out_lock;

    /* Cache Inteligente de MIDI */
    uint8_t               midi_out_cache[4]; 
    int                   midi_out_cache_len;
    
    struct usb_interface  *intf1; 
    struct work_struct    init_work;

    struct usb_endpoint_descriptor *ep_play;
    struct usb_endpoint_descriptor *ep_sync;
    struct usb_endpoint_descriptor *ep_midi_in;
    struct usb_endpoint_descriptor *ep_midi_out;
};

static struct usb_driver ns6_driver; 

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
    int actual_len; 
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

    if (ns6->ep_midi_out) {
        if (usb_endpoint_xfer_int(ns6->ep_midi_out)) {
            usb_interrupt_msg(ns6->udev, usb_sndintpipe(ns6->udev, NS6_EP_MIDI_OUT), buf, NS6_CTRL_PKT_SIZE, &actual_len, 2000);
        } else {
            usb_bulk_msg(ns6->udev, usb_sndbulkpipe(ns6->udev, NS6_EP_MIDI_OUT), buf, NS6_CTRL_PKT_SIZE, &actual_len, 2000);
        }
    }
    pr_info("NS6: Vendor Mode Ativado.\n");
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

    if (s->warmup_cnt > 0 || !s->active_pcm || !sub || !snd_pcm_running(sub)) {
        fill_silence = true;
        if (s->warmup_cnt > 0) s->warmup_cnt--;
    }

    /* 1. ACELERADOR DINÂMICO (High-Speed USB @ 8000 microframes/s) */
    unsigned int target_rate = s->frac_num ? s->frac_num : NS6_RATE;

    for (i = 0; i < urb->number_of_packets; i++) {
        s->frac_acc += target_rate;
        unsigned int pk_frames = s->frac_acc / 8000;
        s->frac_acc %= 8000;
        
        unsigned int pk_bytes = pk_frames * NS6_FRAME_BYTES;
        
        /* Limite de segurança do buffer */
        if (pk_bytes > NS6_PKT_SIZE) {
            pk_bytes = NS6_PKT_SIZE;
            pk_frames = pk_bytes / NS6_FRAME_BYTES;
        }

        urb->iso_frame_desc[i].offset = total_bytes;
        urb->iso_frame_desc[i].length = pk_bytes;
        
        total_frames += pk_frames;
        total_bytes  += pk_bytes;
    }

    /* 2. PREENCHE COM ÁUDIO OU SILÊNCIO */
    if (fill_silence) {
        memset(urb->transfer_buffer, 0, total_bytes);
    } else {
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

    /* O FIX DO CLAUDE: O ALSA precisa ser avisado mesmo durante o warmup para não engasgar! */
    if (elapsed && sub) snd_pcm_period_elapsed(sub);

    if (s->active_usb) usb_submit_urb(urb, GFP_ATOMIC);
}

/* ========================================================================= *
 * EP 0x81 SYNC — O Ouvido do Cristal (A Cura do Drift)
 * ========================================================================= */
static void ns6_sync_urb_cb(struct urb *urb)
{
    struct ns6 *ns6 = urb->context;
    int i;
    
    if (urb->status == 0 && ns6->play.active_usb) {
        for (i = 0; i < urb->number_of_packets; i++) {
            if (urb->iso_frame_desc[i].actual_length >= 3) {
                unsigned char *data = urb->transfer_buffer + urb->iso_frame_desc[i].offset;
                unsigned int feedback = (data[2] << 16) | (data[1] << 8) | data[0];
                
                /* Cálculo oficial para High-Speed USB Asynchronous (Formato 16.16) */
                unsigned int real_rate = (feedback * 125) / 1024;
                
                /* Filtro de sanidade e estabilizador de Jitter */
                if (real_rate > 44000 && real_rate < 44300) {
                    if (ns6->play.frac_num == 0) {
                        ns6->play.frac_num = real_rate; /* Primeira leitura bruta */
                    } else {
                        /* Suavização passa-baixa para o Mixxx não notar flutuações bruscas */
                        ns6->play.frac_num = ((ns6->play.frac_num * 31) + real_rate) / 32;
                    }
                }
            }
        }
    }

    if (ns6->sync_active) usb_submit_urb(urb, GFP_ATOMIC);
}


/* ========================================================================= *
 * DECLARAÇÕES PRÉVIAS
 * ========================================================================= */
static void ns6_process_midi_out(struct ns6 *ns6);

/* ========================================================================= *
 * MIDI (COM INTELIGÊNCIA RUNNING STATUS E ANTI-CRASH)
 * ========================================================================= */
static inline int get_midi_msg_len(uint8_t status) {
    if (status >= 0xF8) return 1; 
    uint8_t cmd = status & 0xF0;
    if (cmd == 0xC0 || cmd == 0xD0) return 2; 
    return 3; 
}

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

static void ns6_midi_out_urb_cb(struct urb *urb) {
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
    
    /* URB liberado! Chama o robô pra continuar esvaziando a fila */
    ns6_process_midi_out(ns6);
}

static void ns6_process_midi_out(struct ns6 *ns6) {
    uint8_t byte;
    unsigned long flags;

    spin_lock_irqsave(&ns6->midi_out_lock, flags);

    if (!ns6->ep_midi_out || !ns6->midi_out_sub) {
        spin_unlock_irqrestore(&ns6->midi_out_lock, flags);
        return;
    }

    /* Loop principal: Otimiza e esvazia o ALSA o mais rápido possível */
    while (1) {
        int idx = -1, i;
        
        /* 1. Procura caminhão (URB) livre no estacionamento */
        for (i = 0; i < NS6_MIDI_OUT_NURBS; i++) {
            if (!ns6->midi_out_busy[i]) {
                idx = i; break;
            }
        }
        
        /* Sem caminhão livre? Paramos e deixamos o ALSA esperar (Evita Crash) */
        if (idx == -1) break; 

        struct urb *u = ns6->midi_out_urbs[idx];
        uint8_t    *buf = ns6->midi_out_bufs[idx];
        int buf_pos = 0;
        bool has_data = false;

        /* 2. Enche o caminhão com até 13 comandos de LED (39 bytes) */
        while (buf_pos + 3 <= NS6_MIDI_PKT_SIZE - 1) {
            
            /* Lê bytes do Mixxx até completar o comando atual */
            while (ns6->midi_out_cache[3] == 0 || ns6->midi_out_cache_len < ns6->midi_out_cache[3]) {
                if (snd_rawmidi_transmit(ns6->midi_out_sub, &byte, 1) != 1) {
                    break; /* Secou a fonte do ALSA */
                }
                
                if (byte >= 0x80) {
                    ns6->midi_out_cache[0] = byte;
                    ns6->midi_out_cache_len = 1;
                    ns6->midi_out_cache[3] = get_midi_msg_len(byte); 
                } else {
                    /* Lógica do Running Status */
                    if (ns6->midi_out_cache_len == 0 && ns6->midi_out_cache[3] > 0) {
                        ns6->midi_out_cache_len = 1; 
                    }
                    if (ns6->midi_out_cache[3] > 0) {
                        ns6->midi_out_cache[ns6->midi_out_cache_len++] = byte;
                    }
                }
            }

            /* Se completamos uma mensagem inteira, joga para dentro do buffer USB! */
            if (ns6->midi_out_cache[3] > 0 && ns6->midi_out_cache_len == ns6->midi_out_cache[3]) {
                
                /* O hardware da Numark exige alinhamento perfeito de 3 bytes */
                buf[buf_pos++] = ns6->midi_out_cache[0];
                buf[buf_pos++] = (ns6->midi_out_cache_len > 1) ? ns6->midi_out_cache[1] : 0x00;
                buf[buf_pos++] = (ns6->midi_out_cache_len > 2) ? ns6->midi_out_cache[2] : 0x00;
                
                ns6->midi_out_cache_len = 0; /* Zera cache para o próximo comando */
                has_data = true;
            } else {
                /* Se não completou, o ALSA esvaziou. Saímos do loop de empacotamento. */
                break; 
            }
        }

        /* Se o caminhão estiver totalmente vazio, não despacha. */
        if (!has_data) break;

        ns6->midi_out_busy[idx] = true;
        
        /* 3. Preenche os buracos vazios restantes do pacote com NS6_IDLE_BYTE (0xFD) */
        while (buf_pos < NS6_MIDI_PKT_SIZE - 1) {
            buf[buf_pos++] = NS6_IDLE_BYTE;
        }
        /* Coloca o terminador no último byte */
        buf[NS6_MIDI_PKT_SIZE - 1] = NS6_PKT_TERM;

        /* 4. Despacha para a controladora! */
        if (usb_endpoint_xfer_int(ns6->ep_midi_out)) {
            usb_fill_int_urb(u, ns6->udev, usb_sndintpipe(ns6->udev, NS6_EP_MIDI_OUT), 
                             buf, NS6_MIDI_PKT_SIZE, ns6_midi_out_urb_cb, ns6, ns6->ep_midi_out->bInterval);
        } else {
            usb_fill_bulk_urb(u, ns6->udev, usb_sndbulkpipe(ns6->udev, NS6_EP_MIDI_OUT), 
                              buf, NS6_MIDI_PKT_SIZE, ns6_midi_out_urb_cb, ns6);
        }
        
        if (usb_submit_urb(u, GFP_ATOMIC)) {
            ns6->midi_out_busy[idx] = false; /* Deu erro? Libera a vaga. */
            break; 
        }
    }
    
    spin_unlock_irqrestore(&ns6->midi_out_lock, flags);
}

static int ns6_midi_in_open(struct snd_rawmidi_substream *sub) { ((struct ns6*)sub->rmidi->private_data)->midi_in_sub = sub; return 0; }
static int ns6_midi_in_close(struct snd_rawmidi_substream *sub) { ((struct ns6*)sub->rmidi->private_data)->midi_in_sub = NULL; return 0; }
static void ns6_midi_in_trigger(struct snd_rawmidi_substream *sub, int up) {}

static int ns6_midi_out_open(struct snd_rawmidi_substream *sub) { 
    struct ns6 *ns6 = sub->rmidi->private_data;
    ns6->midi_out_sub = sub;
    ns6->midi_out_cache_len = 0;
    ns6->midi_out_cache[3] = 0; 
    memset(ns6->midi_out_busy, 0, sizeof(ns6->midi_out_busy));
    return 0; 
}
static int ns6_midi_out_close(struct snd_rawmidi_substream *sub) { 
    ((struct ns6*)sub->rmidi->private_data)->midi_out_sub = NULL; 
    return 0; 
}
static void ns6_midi_out_trigger(struct snd_rawmidi_substream *sub, int up) {
    if (up) ns6_process_midi_out(sub->rmidi->private_data);
}

static const struct snd_rawmidi_ops ns6_midi_in_ops = { .open = ns6_midi_in_open, .close = ns6_midi_in_close, .trigger = ns6_midi_in_trigger };
static const struct snd_rawmidi_ops ns6_midi_out_ops = { .open = ns6_midi_out_open, .close = ns6_midi_out_close, .trigger = ns6_midi_out_trigger };

/* ========================================================================= *
 * ALSA PCM CONFIG 
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
    ns6->play.substream = sub; 
    return 0;
}

static int ns6_pcm_close(struct snd_pcm_substream *sub) {
    struct ns6 *ns6 = snd_pcm_substream_chip(sub);
    unsigned long flags;
    
    spin_lock_irqsave(&ns6->play.lock, flags);
    ns6->play.substream = NULL;
    spin_unlock_irqrestore(&ns6->play.lock, flags);
    return 0;
}

static int ns6_pcm_hw_params(struct snd_pcm_substream *sub, struct snd_pcm_hw_params *p) { return 0; }

static int ns6_pcm_hw_free(struct snd_pcm_substream *sub) { 
    struct ns6 *ns6 = snd_pcm_substream_chip(sub);
    struct ns6_stream *s = &ns6->play;
    unsigned long flags;
    int i;
    
    spin_lock_irqsave(&s->lock, flags);
    s->active_usb = false; 
    s->active_pcm = false;
    ns6->sync_active = false;
    spin_unlock_irqrestore(&s->lock, flags);

    for (i = 0; i < NS6_NURBS; i++) {
        if (s->urbs[i]) usb_kill_urb(s->urbs[i]);
        if (ns6->sync_urbs[i]) usb_kill_urb(ns6->sync_urbs[i]);
    }
    return 0; 
}

static int ns6_pcm_prepare(struct snd_pcm_substream *sub) {
    struct ns6 *ns6 = snd_pcm_substream_chip(sub);
    struct ns6_stream *s = &ns6->play;
    unsigned long flags;
    int i, j, err;

    spin_lock_irqsave(&s->lock, flags);
    
    /* FIX DO CLAUDE: Zera TUDO no ALSA ao trocar de música para evitar Slip! */
    s->hwptr = 0; 
    s->period_pos = 0; 
    s->frac_acc = 0;
    
    if (!s->active_usb) {
        s->frac_num = NS6_RATE; 
        s->warmup_cnt = 50; 
    } else {
        s->warmup_cnt = 0; 
    }
    spin_unlock_irqrestore(&s->lock, flags);

    if (!s->active_usb) {
        s->active_usb = true;
        
        if (!ns6->sync_active && ns6->ep_sync) {
            ns6->sync_active = true;
            for (i = 0; i < NS6_NURBS; i++) {
                if (ns6->sync_urbs[i]) {
                    memset(ns6->sync_bufs[i], 0, NS6_SYNC_URB_BYTES);
                    usb_submit_urb(ns6->sync_urbs[i], GFP_KERNEL);
                }
            }
        }

        for (i = 0; i < NS6_NURBS; i++) {
            if (s->urbs[i]) {
                unsigned int total_bytes = 0;
                for (j = 0; j < s->urbs[i]->number_of_packets; j++) {
                    s->frac_acc += s->frac_num;
                    unsigned int pk_frames = s->frac_acc / 8000;
                    s->frac_acc %= 8000;
                    unsigned int pk_bytes = pk_frames * NS6_FRAME_BYTES;
                    if (pk_bytes > NS6_PKT_SIZE) pk_bytes = NS6_PKT_SIZE;
                    
                    s->urbs[i]->iso_frame_desc[j].offset = total_bytes;
                    s->urbs[i]->iso_frame_desc[j].length = pk_bytes;
                    total_bytes += pk_bytes;
                }
                memset(s->urbs[i]->transfer_buffer, 0, NS6_URB_BYTES);

                err = usb_submit_urb(s->urbs[i], GFP_KERNEL);
                if (err) {
                    s->active_usb = false; 
                    return err;
                }
            }
        }
    } 

    return 0;
}

static int ns6_pcm_trigger(struct snd_pcm_substream *sub, int cmd) {
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

static snd_pcm_uframes_t ns6_pcm_pointer(struct snd_pcm_substream *sub) {
    struct ns6 *ns6 = snd_pcm_substream_chip(sub);
    return ns6->play.hwptr;
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

    for (i = 0; i < NS6_NURBS; i++) {
        s->urbs[i] = usb_alloc_urb(NS6_PKTS_PER_URB, GFP_KERNEL);
        if (!s->urbs[i]) return -ENOMEM;

        s->bufs[i] = usb_alloc_coherent(ns6->udev, NS6_URB_BYTES, GFP_KERNEL, &s->dma[i]);
        if (!s->bufs[i]) return -ENOMEM;

        s->urbs[i]->dev = ns6->udev;
        s->urbs[i]->pipe = pipe;
        s->urbs[i]->transfer_buffer = s->bufs[i];
        
        s->urbs[i]->transfer_dma = s->dma[i];
        s->urbs[i]->transfer_flags = URB_ISO_ASAP | URB_NO_TRANSFER_DMA_MAP;

        s->urbs[i]->transfer_buffer_length = NS6_URB_BYTES;
        s->urbs[i]->number_of_packets = NS6_PKTS_PER_URB;
        s->urbs[i]->complete = cb;
        s->urbs[i]->context = ns6;
        s->urbs[i]->interval = 1;

        for (j = 0; j < NS6_PKTS_PER_URB; j++) {
            s->urbs[i]->iso_frame_desc[j].offset = j * NS6_PKT_SIZE;
            s->urbs[i]->iso_frame_desc[j].length = NS6_PKT_SIZE;
        }
    }
    return 0;
}

static void ns6_free_iso_urbs(struct ns6 *ns6, struct ns6_stream *s) {
    int i;
    for (i = 0; i < NS6_NURBS; i++) {
        if (s->urbs[i]) {
            usb_kill_urb(s->urbs[i]);
            if (s->bufs[i]) usb_free_coherent(ns6->udev, NS6_URB_BYTES, s->bufs[i], s->dma[i]);
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
    int i, j, err;

    if (intf->cur_altsetting->desc.bInterfaceNumber != 0) return -ENODEV;

    err = snd_card_new(&intf->dev, -1, "NS6", THIS_MODULE, sizeof(*ns6), &card);
    if (err) return err;

    ns6 = card->private_data;
    ns6->udev = udev;
    ns6->card = card;

    usb_set_intfdata(intf, ns6);

    spin_lock_init(&ns6->play.lock);
    spin_lock_init(&ns6->midi_out_lock);

    strscpy(card->driver, DRV_NAME, sizeof(card->driver));
    strscpy(card->shortname, "Numark NS6");
    strscpy(card->longname, "Numark NS6 DJ Controller");

    ns6->ep_play = ns6_get_ep_desc(udev, NS6_EP_PLAY);
    ns6->ep_midi_in = ns6_get_ep_desc(udev, NS6_EP_MIDI_IN);
    ns6->ep_midi_out = ns6_get_ep_desc(udev, NS6_EP_MIDI_OUT);

    ns6->intf1 = usb_ifnum_to_if(udev, 1);
    if (ns6->intf1) {
        err = usb_driver_claim_interface(&ns6_driver, ns6->intf1, ns6);
        if (!err) {
            usb_set_interface(udev, 1, 1);
            ns6->ep_sync = ns6_get_ep_desc(udev, NS6_EP_SYNC);
        }
    }

    err = ns6_alloc_iso_urbs(ns6, &ns6->play, ns6->ep_play, ns6_play_urb_cb);
    if (err) goto err_free_play;

    if (ns6->ep_sync) {
        for (i = 0; i < NS6_NURBS; i++) {
            ns6->sync_urbs[i] = usb_alloc_urb(NS6_SYNC_PKTS_PER_URB, GFP_KERNEL);
            if (!ns6->sync_urbs[i]) { err = -ENOMEM; goto err_free_sync; }

            ns6->sync_bufs[i] = usb_alloc_coherent(udev, NS6_SYNC_URB_BYTES, GFP_KERNEL, &ns6->sync_dma[i]);
            if (!ns6->sync_bufs[i]) { err = -ENOMEM; goto err_free_sync; }

            ns6->sync_urbs[i]->dev = udev;
            ns6->sync_urbs[i]->pipe = usb_rcvisocpipe(udev, NS6_EP_SYNC);
            ns6->sync_urbs[i]->transfer_buffer = ns6->sync_bufs[i];
            ns6->sync_urbs[i]->transfer_dma = ns6->sync_dma[i];
            ns6->sync_urbs[i]->transfer_flags = URB_ISO_ASAP | URB_NO_TRANSFER_DMA_MAP;
            ns6->sync_urbs[i]->transfer_buffer_length = NS6_SYNC_URB_BYTES;
            ns6->sync_urbs[i]->number_of_packets = NS6_SYNC_PKTS_PER_URB;
            ns6->sync_urbs[i]->complete = ns6_sync_urb_cb;
            ns6->sync_urbs[i]->context = ns6;
            
            /* O Intervalo revelado pelo LSUSB */
            ns6->sync_urbs[i]->interval = 1 << (ns6->ep_sync->bInterval - 1); 

            for (j = 0; j < NS6_SYNC_PKTS_PER_URB; j++) {
                ns6->sync_urbs[i]->iso_frame_desc[j].offset = j * NS6_SYNC_PKT_SIZE;
                ns6->sync_urbs[i]->iso_frame_desc[j].length = NS6_SYNC_PKT_SIZE;
            }
        }
    }

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

    for (i = 0; i < NS6_MIDI_OUT_NURBS; i++) {
        ns6->midi_out_urbs[i] = usb_alloc_urb(0, GFP_KERNEL);
        if (!ns6->midi_out_urbs[i]) { err = -ENOMEM; goto err_free_midi; }
    }

    /* Agora pedimos ao ALSA 1 Playback e 0 Capturas! (Remoção da Captura Fake) */
    err = snd_pcm_new(card, "NS6 Audio", 0, 1, 0, &pcm); 
    if (err) goto err_free_pcm;

    ns6->pcm = pcm;
    pcm->private_data = ns6;
    strscpy(pcm->name, "Numark NS6");
    snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &ns6_pcm_ops);
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
    for (i = 0; i < NS6_MIDI_OUT_NURBS; i++) {
        if (ns6->midi_out_urbs[i]) { usb_kill_urb(ns6->midi_out_urbs[i]); usb_free_urb(ns6->midi_out_urbs[i]); }
    }
err_free_midi:
    for (i = 0; i < NS6_MIDI_NURBS; i++) {
        if (ns6->midi_in_urbs[i]) { usb_kill_urb(ns6->midi_in_urbs[i]); usb_free_urb(ns6->midi_in_urbs[i]); }
    }
err_free_sync:
    for (i = 0; i < NS6_NURBS; i++) {
        if (ns6->sync_urbs[i]) {
            usb_kill_urb(ns6->sync_urbs[i]);
            if (ns6->sync_bufs[i]) usb_free_coherent(ns6->udev, NS6_SYNC_URB_BYTES, ns6->sync_bufs[i], ns6->sync_dma[i]);
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

static void ns6_disconnect(struct usb_interface *intf) {
    struct ns6 *ns6 = usb_get_intfdata(intf);
    int i;

    if (!ns6) return;

    cancel_work_sync(&ns6->init_work);

    ns6->play.active_usb = false;
    ns6->sync_active     = false;

    for (i = 0; i < NS6_MIDI_NURBS; i++) {
        if (ns6->midi_in_urbs[i]) usb_kill_urb(ns6->midi_in_urbs[i]);
    }
    for (i = 0; i < NS6_MIDI_OUT_NURBS; i++) {
        if (ns6->midi_out_urbs[i]) usb_kill_urb(ns6->midi_out_urbs[i]);
    }
    
    for (i = 0; i < NS6_NURBS; i++) {
        if (ns6->sync_urbs[i]) usb_kill_urb(ns6->sync_urbs[i]);
    }

    snd_card_free(ns6->card);

    ns6_free_iso_urbs(ns6, &ns6->play);

    for (i = 0; i < NS6_NURBS; i++) {
        if (ns6->sync_urbs[i]) {
            if (ns6->sync_bufs[i]) usb_free_coherent(ns6->udev, NS6_SYNC_URB_BYTES, ns6->sync_bufs[i], ns6->sync_dma[i]);
            usb_free_urb(ns6->sync_urbs[i]);
        }
    }

    for (i = 0; i < NS6_MIDI_NURBS; i++) {
        if (ns6->midi_in_urbs[i]) usb_free_urb(ns6->midi_in_urbs[i]);
    }
    for (i = 0; i < NS6_MIDI_OUT_NURBS; i++) {
        if (ns6->midi_out_urbs[i]) usb_free_urb(ns6->midi_out_urbs[i]);
    }

    if (ns6->intf1) {
        struct usb_interface *i1 = ns6->intf1;
        ns6->intf1 = NULL;
        usb_set_interface(ns6->udev, 1, 0);
        usb_driver_release_interface(&ns6_driver, i1);
    }

    usb_set_intfdata(intf, NULL);
}

static const struct usb_device_id ns6_id_table[] = { { USB_DEVICE_INTERFACE_NUMBER(NS6_VID, NS6_PID, 0) }, { } };
MODULE_DEVICE_TABLE(usb, ns6_id_table);
static struct usb_driver ns6_driver = { 
    .name = DRV_NAME, 
    .probe = ns6_probe, 
    .disconnect = ns6_disconnect, 
    .id_table = ns6_id_table,
    .supports_autosuspend = 0, 
};
module_usb_driver(ns6_driver);
MODULE_AUTHOR("ns6d project"); MODULE_DESCRIPTION("Numark NS6 Masterpiece Driver"); MODULE_LICENSE("GPL v2");