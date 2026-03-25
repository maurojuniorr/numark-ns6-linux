#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include "ns6.h"

/* ------------------------------------------------------------------ */
/* Contexto interno do USB                                              */
/* ------------------------------------------------------------------ */
typedef struct {
    /* Bulk IN transfers (EP 0x83) — controle MIDI */
    struct libusb_transfer *ctrl_in_xfer[4];
    uint8_t ctrl_in_buf[4][NS6_CTRL_PKT_SIZE];

    /* Waveform IN transfers (EP 0x86) — descartado */
    struct libusb_transfer *wave_xfer[1];
    uint8_t wave_buf[1][NS6_WAVEFORM_PKT_SIZE];

    /* ISO OUT dummy (EP 0x02) — mantém hardware acordado para MIDI */
    struct libusb_transfer *iso_out_xfer[2];
} ns6_usb_priv_t;

/* ------------------------------------------------------------------ */
/* Callback — Bulk IN EP 0x83 (controle MIDI)                          */
/* ------------------------------------------------------------------ */
static void LIBUSB_CALL ctrl_in_cb(struct libusb_transfer *xfer)
{
    ns6_device_t *dev = (ns6_device_t *)xfer->user_data;

    if (xfer->status == LIBUSB_TRANSFER_COMPLETED &&
        xfer->actual_length == NS6_CTRL_PKT_SIZE) {
        ns6_parse_packet(dev, xfer->buffer);
    } else if (xfer->status != LIBUSB_TRANSFER_CANCELLED) {
        fprintf(stderr, "ns6: ctrl_in status=%d len=%d\n",
                xfer->status, xfer->actual_length);
    }

    if (dev->running)
        libusb_submit_transfer(xfer);
}

/* ------------------------------------------------------------------ */
/* Callback — Bulk IN EP 0x86 (waveform — descartado)                  */
/* ------------------------------------------------------------------ */
static void LIBUSB_CALL wave_in_cb(struct libusb_transfer *xfer)
{
    ns6_device_t *dev = (ns6_device_t *)xfer->user_data;

    if (xfer->status != LIBUSB_TRANSFER_COMPLETED &&
        xfer->status != LIBUSB_TRANSFER_CANCELLED) {
        fprintf(stderr, "ns6: wave_in status=%d\n", xfer->status);
    }

    if (dev->running)
        libusb_submit_transfer(xfer);
}

/* ------------------------------------------------------------------ */
/* Callback — ISO OUT EP 0x02 (silêncio — mantém hardware acordado)   */
/* ------------------------------------------------------------------ */
static void LIBUSB_CALL audio_dummy_cb(struct libusb_transfer *xfer)
{
    ns6_device_t *dev = (ns6_device_t *)xfer->user_data;
    /* Resubmete com silêncio — só para manter o hardware acordado */
    if (dev->running)
        libusb_submit_transfer(xfer);
}

/* ------------------------------------------------------------------ */
/* Aloca ISO transfer                                                   */
/* ------------------------------------------------------------------ */
static struct libusb_transfer *alloc_iso_transfer(
    libusb_device_handle *usb, uint8_t ep,
    int n_packets, int pkt_size,
    libusb_transfer_cb_fn cb, void *user_data)
{
    struct libusb_transfer *xfer = libusb_alloc_transfer(n_packets);
    if (!xfer) return NULL;

    uint8_t *buf = calloc(n_packets * pkt_size, 1);
    if (!buf) { libusb_free_transfer(xfer); return NULL; }

    libusb_fill_iso_transfer(xfer, usb, ep, buf,
                              n_packets * pkt_size,
                              n_packets, cb, user_data, 0);
    libusb_set_iso_packet_lengths(xfer, pkt_size);
    return xfer;
}
/* ------------------------------------------------------------------ */
#define NS6_OUT_QUEUE_SIZE 256

typedef struct { uint8_t status, note, value; } ns6_midi_msg_t;

static ns6_midi_msg_t  out_queue[NS6_OUT_QUEUE_SIZE];
static int             out_queue_head = 0;
static int             out_queue_tail = 0;
static pthread_mutex_t out_queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  out_queue_cond  = PTHREAD_COND_INITIALIZER;

static void out_queue_push(uint8_t status, uint8_t note, uint8_t value)
{
    pthread_mutex_lock(&out_queue_mutex);
    int next = (out_queue_tail + 1) % NS6_OUT_QUEUE_SIZE;
    if (next != out_queue_head) {
        out_queue[out_queue_tail] = (ns6_midi_msg_t){ status, note, value };
        out_queue_tail = next;
        pthread_cond_signal(&out_queue_cond);
    }
    pthread_mutex_unlock(&out_queue_mutex);
}

void ns6_midi_out_wake(void)
{
    pthread_mutex_lock(&out_queue_mutex);
    pthread_cond_broadcast(&out_queue_cond);
    pthread_mutex_unlock(&out_queue_mutex);
}

void *ns6_midi_out_worker(void *arg)
{
    ns6_device_t *dev = (ns6_device_t *)arg;
    uint8_t pkt[NS6_CTRL_PKT_SIZE];

    while (dev->running) {
        pthread_mutex_lock(&out_queue_mutex);
        while (out_queue_head == out_queue_tail && dev->running)
            pthread_cond_wait(&out_queue_cond, &out_queue_mutex);

        if (!dev->running) {
            pthread_mutex_unlock(&out_queue_mutex);
            break;
        }

        ns6_midi_msg_t msg = out_queue[out_queue_head];
        out_queue_head = (out_queue_head + 1) % NS6_OUT_QUEUE_SIZE;
        pthread_mutex_unlock(&out_queue_mutex);

        memset(pkt, NS6_IDLE_BYTE, NS6_CTRL_PKT_SIZE);
        pkt[0] = msg.status;
        pkt[1] = msg.note;
        pkt[2] = msg.value;
        pkt[NS6_CTRL_PKT_SIZE - 1] = NS6_PKT_TERMINATOR;

        int transferred = 0;
        libusb_bulk_transfer(dev->usb, NS6_EP_CTRL_OUT,
                             pkt, NS6_CTRL_PKT_SIZE,
                             &transferred, 100);
    }
    return NULL;
}

int ns6_send_midi(ns6_device_t *dev, uint8_t status, uint8_t note, uint8_t value)
{
    (void)dev;
    out_queue_push(status, note, value);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Inicia transfers USB                                                 */
/* ------------------------------------------------------------------ */
int ns6_run(ns6_device_t *dev)
{
    ns6_usb_priv_t *priv = calloc(1, sizeof(ns6_usb_priv_t));
    if (!priv) return -1;
    dev->priv = priv;

    /* Bulk IN: EP 0x83 × 4 — MIDI control */
    for (int i = 0; i < 4; i++) {
        priv->ctrl_in_xfer[i] = libusb_alloc_transfer(0);
        if (!priv->ctrl_in_xfer[i]) goto fail;

        libusb_fill_bulk_transfer(
            priv->ctrl_in_xfer[i], dev->usb,
            NS6_EP_CTRL_IN,
            priv->ctrl_in_buf[i], NS6_CTRL_PKT_SIZE,
            ctrl_in_cb, dev, 0);

        int r = libusb_submit_transfer(priv->ctrl_in_xfer[i]);
        if (r < 0) {
            fprintf(stderr, "ns6: ctrl_in submit[%d]: %s\n",
                    i, libusb_strerror(r));
            goto fail;
        }
    }

    /* Bulk IN: EP 0x86 × 1 — waveform (descartado) */
    priv->wave_xfer[0] = libusb_alloc_transfer(0);
    if (!priv->wave_xfer[0]) goto fail;

    libusb_fill_bulk_transfer(
        priv->wave_xfer[0], dev->usb,
        NS6_EP_WAVEFORM,
        priv->wave_buf[0], NS6_WAVEFORM_PKT_SIZE,
        wave_in_cb, dev, 0);

    libusb_submit_transfer(priv->wave_xfer[0]);

    /* ISO OUT dummy × 2 — silêncio, só para manter EP 0x02 ativo */
    /* Sem isso o hardware não envia dados no EP 0x83 (MIDI)        */
    for (int i = 0; i < 2; i++) {
        priv->iso_out_xfer[i] = alloc_iso_transfer(
            dev->usb, 0x02, 32, 66, audio_dummy_cb, dev);
        if (priv->iso_out_xfer[i])
            libusb_submit_transfer(priv->iso_out_xfer[i]);
    }

    printf("ns6: all transfers running\n");

    while (dev->running) {
        struct timeval tv = { 0, 10000 };
        int r = libusb_handle_events_timeout_completed(dev->ctx, &tv, NULL);
        if (r < 0 && r != LIBUSB_ERROR_INTERRUPTED) {
            fprintf(stderr, "ns6: handle_events: %s\n", libusb_strerror(r));
            break;
        }
    }
    return 0;

fail:
    dev->running = false;
    free(priv);
    dev->priv = NULL;
    return -1;
}

/* ------------------------------------------------------------------ */
/* Abre/fecha dispositivo USB                                           */
/* ------------------------------------------------------------------ */
ns6_device_t *ns6_open(void)
{
    ns6_device_t *dev = calloc(1, sizeof(ns6_device_t));
    if (!dev) return NULL;

    int r = libusb_init(&dev->ctx);
    if (r < 0) {
        fprintf(stderr, "ns6: libusb_init: %s\n", libusb_strerror(r));
        free(dev);
        return NULL;
    }

    dev->usb = libusb_open_device_with_vid_pid(
        dev->ctx, NS6_VENDOR_ID, NS6_PRODUCT_ID);

    if (!dev->usb) {
        fprintf(stderr, "ns6: device not found (VID=0x%04x PID=0x%04x)\n",
                NS6_VENDOR_ID, NS6_PRODUCT_ID);
        libusb_exit(dev->ctx);
        free(dev);
        return NULL;
    }

    libusb_set_auto_detach_kernel_driver(dev->usb, 1);
    printf("ns6: device opened OK\n");
    return dev;
}

void ns6_close(ns6_device_t *dev)
{
    if (!dev) return;

    dev->running = false;

    ns6_usb_priv_t *priv = dev->priv;
    if (priv) {
        for (int i = 0; i < 4; i++) {
            if (priv->ctrl_in_xfer[i])
                libusb_cancel_transfer(priv->ctrl_in_xfer[i]);
        }
        if (priv->wave_xfer[0])
            libusb_cancel_transfer(priv->wave_xfer[0]);
        free(priv);
    }

    libusb_release_interface(dev->usb, 0);
    libusb_close(dev->usb);
    libusb_exit(dev->ctx);
    free(dev);
    printf("ns6: closed\n");
}
