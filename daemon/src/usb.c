#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#include "ns6.h"
#include <sched.h>

/* ------------------------------------------------------------------ */
/* Declarações externas do audio.c                                      */
/* ------------------------------------------------------------------ */
extern void ns6_audio_pump(void);
extern int  ns6_audio_ring_fill(void);

/* Constantes de drift compensation (devem coincidir com audio.c) */
#define TARGET_FILL 4410
#define DRIFT_ZONE  220     /* ±5ms de zona morta (era 441) */

/* ------------------------------------------------------------------ */
/* Motor de MIDI OUT                                                    */
/* ------------------------------------------------------------------ */
#define NS6_OUT_QUEUE_SIZE 256
typedef struct { uint8_t status, note, value; } ns6_midi_msg_t;

static ns6_midi_msg_t  out_queue[NS6_OUT_QUEUE_SIZE];
static int             out_queue_head = 0;
static int             out_queue_tail = 0;
static pthread_mutex_t out_queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  out_queue_cond  = PTHREAD_COND_INITIALIZER;

void out_queue_push(uint8_t status, uint8_t note, uint8_t value) {
    pthread_mutex_lock(&out_queue_mutex);
    int next = (out_queue_tail + 1) % NS6_OUT_QUEUE_SIZE;
    if (next != out_queue_head) {
        out_queue[out_queue_tail] = (ns6_midi_msg_t){ status, note, value };
        out_queue_tail = next;
        pthread_cond_signal(&out_queue_cond);
    }
    pthread_mutex_unlock(&out_queue_mutex);
}

void ns6_midi_out_wake(void) {
    pthread_mutex_lock(&out_queue_mutex);
    pthread_cond_broadcast(&out_queue_cond);
    pthread_mutex_unlock(&out_queue_mutex);
}

int ns6_send_midi(ns6_device_t *dev, uint8_t status, uint8_t note, uint8_t value) {
    (void)dev;
    out_queue_push(status, note, value);
    return 0;
}

void *ns6_midi_out_worker(void *arg) {
    ns6_device_t *dev = (ns6_device_t *)arg;
    uint8_t pkt[NS6_CTRL_PKT_SIZE];
    while (dev->running) {
        pthread_mutex_lock(&out_queue_mutex);
        while (out_queue_head == out_queue_tail && dev->running)
            pthread_cond_wait(&out_queue_cond, &out_queue_mutex);
        if (!dev->running) { pthread_mutex_unlock(&out_queue_mutex); break; }
        int buf_pos = 0;
        memset(pkt, NS6_IDLE_BYTE, NS6_CTRL_PKT_SIZE);
        while (out_queue_head != out_queue_tail && buf_pos + 3 <= NS6_CTRL_PKT_SIZE - 1) {
            ns6_midi_msg_t msg = out_queue[out_queue_head];
            out_queue_head = (out_queue_head + 1) % NS6_OUT_QUEUE_SIZE;
            pkt[buf_pos++] = msg.status;
            pkt[buf_pos++] = msg.note;
            pkt[buf_pos++] = msg.value;
        }
        pthread_mutex_unlock(&out_queue_mutex);
        pkt[NS6_CTRL_PKT_SIZE - 1] = NS6_PKT_TERMINATOR;
        int transferred = 0;
        libusb_bulk_transfer(dev->usb, NS6_EP_CTRL_OUT, pkt,
                             NS6_CTRL_PKT_SIZE, &transferred, 100);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Contexto USB e Audio                                                 */
/* ------------------------------------------------------------------ */
typedef struct {
    struct libusb_transfer *ctrl_in_xfer[4];
    uint8_t ctrl_in_buf[4][NS6_CTRL_PKT_SIZE];
    struct libusb_transfer *sync_xfer[4];
    uint8_t sync_buf[4][NS6_SYNC_PKT_SIZE * NS6_SYNC_PKTS_PER_URB];
    struct libusb_transfer *iso_out_xfer[4];
    uint8_t iso_out_buf[4][NS6_PLAY_PKT_SIZE * NS6_PLAY_PKTS_PER_URB];
    unsigned int frac_acc;
} ns6_usb_priv_t;

/* ------------------------------------------------------------------ */
/* Fill & Submit — acumulador fracional com drift compensation          */
/* ------------------------------------------------------------------ */
static void fill_and_submit_iso_out(ns6_device_t *dev,
                                     struct libusb_transfer *xfer) {
    ns6_usb_priv_t *priv = dev->priv;
    int total_bytes = 0;
    int pkt_sizes[NS6_PLAY_PKTS_PER_URB];
    int urb_total_frames = 0;

    /* 1. Acumulador fracional fixo a 44100 Hz */
    for (int i = 0; i < xfer->num_iso_packets; i++) {
        priv->frac_acc += 44100;
        int pk_frames = priv->frac_acc / 8000;
        priv->frac_acc %= 8000;
        pkt_sizes[i] = pk_frames * 12;
        urb_total_frames += pk_frames;
    }

    /* 2. Drift compensation: ajusta ±1 frame no último pacote
     *    baseado no nível do ring buffer.
     *    Se ring enchendo → consome 1 frame extra (acelera)
     *    Se ring esvaziando → consome 1 frame a menos (freia)
     *    Zona morta de ±10ms ao redor do target evita oscilação. */
    int fill_now = ns6_audio_ring_fill();
    int last = xfer->num_iso_packets - 1;

    if (fill_now > TARGET_FILL + DRIFT_ZONE) {
        /* Ring acumulando: consumir 1 frame extra */
        pkt_sizes[last] += 12;
        urb_total_frames += 1;
    } else if (fill_now < TARGET_FILL - DRIFT_ZONE && fill_now > 176) {
        /* Ring baixando: consumir 1 frame a menos */
        if (pkt_sizes[last] > 24) {   /* mínimo 2 frames no pacote */
            pkt_sizes[last] -= 12;
            urb_total_frames -= 1;
        }
    }

    /* 3. Pull do ring buffer — uma única chamada por URB */
    int urb_total_bytes = urb_total_frames * 12;
    uint8_t urb_audio[2148];   /* 179 * 12 = margem de segurança */
    ns6_audio_pull_playback(urb_audio, urb_total_bytes);

    /* 4. Distribui nos pacotes ISO */
    int audio_offset = 0;
    for (int i = 0; i < xfer->num_iso_packets; i++) {
        xfer->iso_packet_desc[i].length = pkt_sizes[i];
        memcpy(xfer->buffer + total_bytes,
               urb_audio + audio_offset, pkt_sizes[i]);
        total_bytes += pkt_sizes[i];
        audio_offset += pkt_sizes[i];
    }

    xfer->length = total_bytes;
    if (dev->running) libusb_submit_transfer(xfer);
}

/* ------------------------------------------------------------------ */
/* Callbacks USB                                                        */
/* ------------------------------------------------------------------ */
static void LIBUSB_CALL ctrl_in_cb(struct libusb_transfer *xfer) {
    ns6_device_t *dev = (ns6_device_t *)xfer->user_data;
    if (xfer->status == LIBUSB_TRANSFER_COMPLETED)
        ns6_parse_packet(dev, xfer->buffer);
    if (dev->running) libusb_submit_transfer(xfer);
}

static void LIBUSB_CALL sync_in_cb(struct libusb_transfer *xfer) {
    ns6_device_t *dev = (ns6_device_t *)xfer->user_data;
    /* Feedback 0xAAAAAA = dummy, apenas re-submete */
    if (dev->running) libusb_submit_transfer(xfer);
}

static void LIBUSB_CALL iso_out_cb(struct libusb_transfer *xfer) {
    ns6_device_t *dev = (ns6_device_t *)xfer->user_data;
    if (dev->running) fill_and_submit_iso_out(dev, xfer);
}

static struct libusb_transfer *alloc_iso_transfer(
        libusb_device_handle *usb, uint8_t ep,
        int n_packets, int pkt_size,
        libusb_transfer_cb_fn cb, void *user_data, uint8_t *buf) {
    struct libusb_transfer *xfer = libusb_alloc_transfer(n_packets);
    libusb_fill_iso_transfer(xfer, usb, ep, buf,
                             n_packets * pkt_size, n_packets,
                             cb, user_data, 0);
    return xfer;
}

/* ------------------------------------------------------------------ */
/* Thread dedicada do Audio Pump                                        */
/* ------------------------------------------------------------------ */
static void *audio_pump_thread(void *arg) {
    ns6_device_t *dev = (ns6_device_t *)arg;
    while (dev->running) {
        ns6_audio_pump();
        usleep(500);   /* 0.5ms → ~2000 pumps/s */
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* ns6_run — loop principal                                             */
/* ------------------------------------------------------------------ */
int ns6_run(ns6_device_t *dev) {
    /* Real-time scheduling (prioridade segura) */
    struct sched_param param;
    param.sched_priority = 70;
    if (sched_setscheduler(0, SCHED_FIFO, &param) == -1) {
        fprintf(stderr, "ns6: AVISO — Falha SCHED_FIFO (sudo?)\n");
    } else {
        printf("ns6: Real-Time SCHED_FIFO ativado (prio=70).\n");
    }

    ns6_usb_priv_t *priv = calloc(1, sizeof(ns6_usb_priv_t));
    dev->priv = priv;

    /* 1. MIDI In (EP 0x83) — 4 URBs */
    for (int i = 0; i < 4; i++) {
        priv->ctrl_in_xfer[i] = libusb_alloc_transfer(0);
        libusb_fill_bulk_transfer(priv->ctrl_in_xfer[i], dev->usb,
                                  NS6_EP_CTRL_IN, priv->ctrl_in_buf[i],
                                  NS6_CTRL_PKT_SIZE, ctrl_in_cb, dev, 0);
        libusb_submit_transfer(priv->ctrl_in_xfer[i]);
    }

    /* 2. Feedback Sync (EP 0x81) — 3 URBs (dummy, re-submit only) */
    for (int i = 0; i < 3; i++) {
        priv->sync_xfer[i] = alloc_iso_transfer(
            dev->usb, NS6_EP_SYNC, NS6_SYNC_PKTS_PER_URB,
            NS6_SYNC_PKT_SIZE, sync_in_cb, dev, priv->sync_buf[i]);
        for (int p = 0; p < NS6_SYNC_PKTS_PER_URB; p++)
            priv->sync_xfer[i]->iso_packet_desc[p].length = NS6_SYNC_PKT_SIZE;
        libusb_submit_transfer(priv->sync_xfer[i]);
    }

    /* 3. Audio Playback (EP 0x02) — 3 URBs */
    for (int i = 0; i < 3; i++) {
        priv->iso_out_xfer[i] = alloc_iso_transfer(
            dev->usb, NS6_EP_PLAY, NS6_PLAY_PKTS_PER_URB,
            NS6_PLAY_PKT_SIZE, iso_out_cb, dev, priv->iso_out_buf[i]);
        fill_and_submit_iso_out(dev, priv->iso_out_xfer[i]);
    }

    /* 4. Thread MIDI Worker */
    pthread_t midi_thread;
    pthread_create(&midi_thread, NULL, ns6_midi_out_worker, dev);
    pthread_detach(midi_thread);

    /* 5. Thread Audio Pump */
    pthread_t thr_pump;
    pthread_create(&thr_pump, NULL, audio_pump_thread, dev);

    /* 6. Main Event Loop — só libusb */
    while (dev->running) {
        struct timeval tv = { 0, 1000 };
        libusb_handle_events_timeout_completed(dev->ctx, &tv, NULL);
    }

    pthread_join(thr_pump, NULL);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Open / Close                                                         */
/* ------------------------------------------------------------------ */
ns6_device_t *ns6_open(void) {
    ns6_device_t *dev = calloc(1, sizeof(ns6_device_t));
    libusb_init(&dev->ctx);
    dev->usb = libusb_open_device_with_vid_pid(dev->ctx,
                                                NS6_VENDOR_ID, NS6_PRODUCT_ID);
    if (!dev->usb) { free(dev); return NULL; }
    libusb_set_auto_detach_kernel_driver(dev->usb, 1);
    dev->running = true;
    return dev;
}

void ns6_close(ns6_device_t *dev) {
    if (!dev) return;
    dev->running = false;
    usleep(100000);
    libusb_close(dev->usb);
    libusb_exit(dev->ctx);
    free(dev);
}
