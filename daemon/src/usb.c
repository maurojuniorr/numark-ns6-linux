#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <time.h>
#include "ns6.h"
#include "ns6_clock_control.h"
#include "ns6_iso_schedule.h"

enum { NS6_FEEDBACK_PACKETS = 32, NS6_FEEDBACK_MAX_PACKET = 64,
       NS6_FEEDBACK_SLOTS = 2, NS6_CAPTURE_READ_SLOTS = 8 };

typedef struct {
    ns6_device_t *dev;
    struct libusb_transfer *transfer;
    unsigned millisecond_frames[8];
} ns6_iso_slot_t;

typedef struct {
    ns6_device_t *dev;
    struct libusb_transfer *transfer;
    uint8_t buffer[NS6_CAPTURE_READ_SIZE];
    uint64_t sequence;
    size_t actual_length;
    bool completed;
} ns6_capture_slot_t;

/* ------------------------------------------------------------------ */
/* Contexto interno do USB                                              */
/* ------------------------------------------------------------------ */
typedef struct {
    /* Bulk IN transfers (EP 0x83) — controle MIDI */
    struct libusb_transfer *ctrl_in_xfer[4];
    uint8_t ctrl_in_buf[4][NS6_CTRL_PKT_SIZE];

    /* Capture IN (EP 0x86) — packed stereo PCM → PipeWire source ring. */
    ns6_capture_slot_t capture[NS6_CAPTURE_READ_SLOTS];
    uint64_t capture_next_submit;
    uint64_t capture_next_process;

    struct libusb_transfer *feedback_xfer[NS6_FEEDBACK_SLOTS];
    uint8_t feedback_buf[NS6_FEEDBACK_SLOTS][NS6_FEEDBACK_PACKETS * NS6_FEEDBACK_MAX_PACKET];
    unsigned feedback_packet_bytes;
    ns6_clock_control_t clock;

    /* ISO OUT (EP 0x02) — four-channel PipeWire PCM at 44.1 kHz */
    ns6_iso_slot_t iso_out[2];
} ns6_usb_priv_t;

static uint64_t monotonic_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

static void mark_transport_lost(ns6_device_t *dev, const char *operation,
                               int error) {
    if (error != LIBUSB_ERROR_NO_DEVICE) return;
    fprintf(stderr, "ns6: USB device disconnected during %s\n", operation);
    dev->transport_lost = true;
    dev->running = false;
}

static void LIBUSB_CALL feedback_in_cb(struct libusb_transfer *xfer) {
    ns6_device_t *dev = xfer->user_data;
    if (xfer->status == LIBUSB_TRANSFER_NO_DEVICE) {
        mark_transport_lost(dev, "feedback IN", LIBUSB_ERROR_NO_DEVICE);
        return;
    }
    if (!dev->running || xfer->status == LIBUSB_TRANSFER_CANCELLED) return;
    ns6_usb_priv_t *priv = dev->priv;
    if (!priv) return;
    if (xfer->status == LIBUSB_TRANSFER_COMPLETED) {
        uint64_t now_ms = monotonic_ms();
        for (int i = 0; i < xfer->num_iso_packets; ++i) {
            struct libusb_iso_packet_descriptor *packet = &xfer->iso_packet_desc[i];
            if (packet->status != LIBUSB_TRANSFER_COMPLETED || packet->actual_length < 3)
                continue;
            bool was_locked = priv->clock.locked;
            const uint8_t *data = xfer->buffer +
                    (size_t)i * priv->feedback_packet_bytes;
            if (ns6_clock_control_observe(&priv->clock,
                    data[0], now_ms) && !was_locked)
                fprintf(stderr, "ns6: USB clock feedback locked at %u frames/ms\n",
                        data[0]);
        }
    }
    int r = libusb_submit_transfer(xfer);
    if (r < 0) {
        fprintf(stderr, "ns6: feedback IN resubmit: %s\n", libusb_strerror(r));
        mark_transport_lost(dev, "feedback IN resubmit", r);
    }
}

/* ------------------------------------------------------------------ */
/* Callback — Bulk IN EP 0x83 (controle MIDI)                          */
/* ------------------------------------------------------------------ */
static void LIBUSB_CALL ctrl_in_cb(struct libusb_transfer *xfer)
{
    ns6_device_t *dev = (ns6_device_t *)xfer->user_data;

    if (xfer->status == LIBUSB_TRANSFER_COMPLETED &&
        xfer->actual_length == NS6_CTRL_PKT_SIZE) {
        ns6_parse_packet(dev, xfer->buffer);
    } else if (xfer->status == LIBUSB_TRANSFER_NO_DEVICE) {
        mark_transport_lost(dev, "MIDI IN", LIBUSB_ERROR_NO_DEVICE);
    } else if (xfer->status != LIBUSB_TRANSFER_CANCELLED) {
        fprintf(stderr, "ns6: ctrl_in status=%d len=%d\n",
                xfer->status, xfer->actual_length);
    }

    if (dev->running) {
        int r = libusb_submit_transfer(xfer);
        if (r < 0) mark_transport_lost(dev, "MIDI IN resubmit", r);
    }
}

/* ------------------------------------------------------------------ */
/* Callback — Bulk IN EP 0x86 (packed stereo record mix)               */
/* ------------------------------------------------------------------ */
static void LIBUSB_CALL capture_in_cb(struct libusb_transfer *xfer)
{
    ns6_capture_slot_t *slot = xfer->user_data;
    ns6_device_t *dev = slot->dev;
    ns6_usb_priv_t *priv = dev->priv;
    if (!priv) return;

    if (xfer->status == LIBUSB_TRANSFER_NO_DEVICE) {
        mark_transport_lost(dev, "capture IN", LIBUSB_ERROR_NO_DEVICE);
        return;
    }
    if (xfer->status == LIBUSB_TRANSFER_CANCELLED) return;
    if (xfer->status != LIBUSB_TRANSFER_COMPLETED) {
        fprintf(stderr, "ns6: capture_in status=%d\n", xfer->status);
    }
    slot->actual_length = xfer->status == LIBUSB_TRANSFER_COMPLETED ?
                          (size_t)xfer->actual_length : 0;
    slot->completed = true;

    while (dev->running) {
        ns6_capture_slot_t *ready = NULL;
        for (size_t i = 0; i < NS6_CAPTURE_READ_SLOTS; ++i) {
            if (priv->capture[i].completed &&
                priv->capture[i].sequence == priv->capture_next_process) {
                ready = &priv->capture[i];
                break;
            }
        }
        if (!ready) break;

        if (ready->actual_length > 0)
            ns6_audio_capture_usb_block(dev, ready->buffer,
                                        ready->actual_length);
        ready->completed = false;
        ready->sequence = priv->capture_next_submit++;
        ++priv->capture_next_process;
        int r = libusb_submit_transfer(ready->transfer);
        if (r < 0) {
            fprintf(stderr, "ns6: capture IN resubmit: %s\n", libusb_strerror(r));
            mark_transport_lost(dev, "capture IN resubmit", r);
            dev->running = false;
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Callback — ISO OUT EP 0x02 (scheduled four-channel PCM)             */
/* ------------------------------------------------------------------ */
static void LIBUSB_CALL audio_out_cb(struct libusb_transfer *xfer)
{
    ns6_iso_slot_t *slot = (ns6_iso_slot_t *)xfer->user_data;
    if (xfer->status == LIBUSB_TRANSFER_NO_DEVICE) {
        mark_transport_lost(slot->dev, "ISO output", LIBUSB_ERROR_NO_DEVICE);
        return;
    }
    if (!slot->dev->running) return;
    ns6_usb_priv_t *priv = slot->dev->priv;
    uint64_t now_ms = monotonic_ms();
    if (priv->clock.locked && now_ms - priv->clock.last_feedback_ms > 2000)
        ns6_clock_control_reset(&priv->clock);

    size_t bytes = 0;
    memset(xfer->buffer, 0,
           NS6_ISO_PACKETS_PER_TRANSFER *
               NS6_ISO_MAX_FRAMES_PER_PACKET * NS6_AUDIO_FRAME_BYTES);
    for (unsigned packet = 0; packet < NS6_ISO_PACKETS_PER_TRANSFER; ++packet) {
        if (packet % NS6_ISO_PACKETS_PER_MS == 0)
            ns6_clock_control_next_millisecond(&priv->clock,
                                                slot->millisecond_frames);
        unsigned packet_frames = slot->millisecond_frames[
            packet % NS6_ISO_PACKETS_PER_MS];
        struct libusb_iso_packet_descriptor *descriptor =
            &xfer->iso_packet_desc[packet];
        descriptor->length = packet_frames * NS6_AUDIO_FRAME_BYTES;
        descriptor->actual_length = 0;
        descriptor->status = 0;
        ns6_audio_output_read(slot->dev, xfer->buffer + bytes, packet_frames,
                ns6_clock_control_source_step(&priv->clock,
                    ns6_audio_output_queued(slot->dev)));
        bytes += descriptor->length;
    }
    xfer->length = (int)bytes;
    int r = libusb_submit_transfer(xfer);
    if (r < 0) {
        fprintf(stderr, "ns6: ISO output resubmit: %s\n", libusb_strerror(r));
        mark_transport_lost(slot->dev, "ISO output resubmit", r);
    }
}

/* ------------------------------------------------------------------ */
/* Aloca ISO transfer                                                   */
/* ------------------------------------------------------------------ */
static struct libusb_transfer *alloc_iso_transfer(
    libusb_device_handle *usb, uint8_t ep, ns6_iso_slot_t *slot,
    unsigned packet_count)
{
    const int n_packets = (int)packet_count;
    const int max_packet_bytes =
        NS6_ISO_MAX_FRAMES_PER_PACKET * NS6_AUDIO_FRAME_BYTES;
    const int buffer_bytes = n_packets * max_packet_bytes;
    struct libusb_transfer *xfer = libusb_alloc_transfer(n_packets);
    if (!xfer) return NULL;

    uint8_t *buf = calloc((size_t)buffer_bytes, 1);
    if (!buf) { libusb_free_transfer(xfer); return NULL; }

    libusb_fill_iso_transfer(xfer, usb, ep, buf,
                             buffer_bytes, n_packets, audio_out_cb, slot, 0);
    return xfer;
}

static void start_feedback_reads(ns6_device_t *dev, ns6_usb_priv_t *priv) {
    int packet_bytes = libusb_get_max_iso_packet_size(
            libusb_get_device(dev->usb), NS6_EP_FEEDBACK);
    if (packet_bytes < 1) {
        fprintf(stderr, "ns6: feedback endpoint unavailable; using nominal USB clock\n");
        return;
    }
    if (packet_bytes > NS6_FEEDBACK_MAX_PACKET)
        packet_bytes = NS6_FEEDBACK_MAX_PACKET;
    priv->feedback_packet_bytes = (unsigned)packet_bytes;
    for (int i = 0; i < NS6_FEEDBACK_SLOTS; ++i) {
        struct libusb_transfer *xfer = libusb_alloc_transfer(NS6_FEEDBACK_PACKETS);
        if (!xfer) break;
        priv->feedback_xfer[i] = xfer;
        libusb_fill_iso_transfer(xfer, dev->usb, NS6_EP_FEEDBACK,
                priv->feedback_buf[i], NS6_FEEDBACK_PACKETS * packet_bytes,
                NS6_FEEDBACK_PACKETS, feedback_in_cb, dev, 0);
        libusb_set_iso_packet_lengths(xfer, (unsigned)packet_bytes);
        int r = libusb_submit_transfer(xfer);
        if (r < 0) {
            fprintf(stderr, "ns6: feedback IN submit[%d]: %s\n",
                    i, libusb_strerror(r));
            libusb_free_transfer(xfer);
            priv->feedback_xfer[i] = NULL;
            break;
        }
    }
    if (priv->feedback_xfer[0])
        printf("ns6: clock feedback active on endpoint 0x81 (%u-byte packets, %d transfers)\n",
               priv->feedback_packet_bytes,
               priv->feedback_xfer[1] ? NS6_FEEDBACK_SLOTS : 1);
    else
        fprintf(stderr, "ns6: clock feedback unavailable; using nominal USB clock\n");
}
/* ------------------------------------------------------------------ */
#define NS6_OUT_QUEUE_SIZE 2048

static uint8_t         out_queue[NS6_OUT_QUEUE_SIZE][NS6_CTRL_PKT_SIZE];
static int             out_queue_head = 0;
static int             out_queue_tail = 0;
static bool            out_queue_overflow_reported = false;
static pthread_mutex_t out_queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  out_queue_cond  = PTHREAD_COND_INITIALIZER;

static int out_queue_push(const uint8_t packet[NS6_CTRL_PKT_SIZE])
{
    int result = 0;
    pthread_mutex_lock(&out_queue_mutex);
    int next = (out_queue_tail + 1) % NS6_OUT_QUEUE_SIZE;
    if (next != out_queue_head) {
        memcpy(out_queue[out_queue_tail], packet, NS6_CTRL_PKT_SIZE);
        out_queue_tail = next;
        out_queue_overflow_reported = false;
        pthread_cond_signal(&out_queue_cond);
    } else {
        result = -1;
        if (!out_queue_overflow_reported) {
            fprintf(stderr, "ns6: MIDI output queue full; events are being dropped\n");
            out_queue_overflow_reported = true;
        }
    }
    pthread_mutex_unlock(&out_queue_mutex);
    return result;
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

        memcpy(pkt, out_queue[out_queue_head], NS6_CTRL_PKT_SIZE);
        out_queue_head = (out_queue_head + 1) % NS6_OUT_QUEUE_SIZE;
        pthread_mutex_unlock(&out_queue_mutex);

        int transferred = 0;
        int r = libusb_bulk_transfer(dev->usb, NS6_EP_CTRL_OUT,
                                     pkt, NS6_CTRL_PKT_SIZE,
                                     &transferred, 100);
        mark_transport_lost(dev, "MIDI OUT", r);
    }
    return NULL;
}

int ns6_send_midi(ns6_device_t *dev, uint8_t status, uint8_t note, uint8_t value)
{
    if (!dev) return -1;
    uint8_t packet[NS6_CTRL_PKT_SIZE];
    memset(packet, NS6_IDLE_BYTE, sizeof(packet));
    packet[0] = status;
    packet[1] = note;
    packet[2] = value;
    packet[NS6_CTRL_PKT_SIZE - 1] = NS6_PKT_TERMINATOR;
    if (out_queue_push(packet) < 0) return -1;
    return dev->transport_lost ? -1 : 0;
}

int ns6_send_ctrl_packet(ns6_device_t *dev,
                         const uint8_t packet[NS6_CTRL_PKT_SIZE])
{
    if (!dev || !packet || dev->transport_lost) return -1;
    return out_queue_push(packet);
}

/* ------------------------------------------------------------------ */
/* Inicia transfers USB                                                 */
/* ------------------------------------------------------------------ */
int ns6_run(ns6_device_t *dev)
{
    ns6_usb_priv_t *priv = calloc(1, sizeof(ns6_usb_priv_t));
    if (!priv) return -1;
    dev->priv = priv;
    ns6_clock_control_init(&priv->clock);

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

    /* Keep ordered reads queued so the bulk endpoint never waits on one callback. */
    for (size_t i = 0; i < NS6_CAPTURE_READ_SLOTS; ++i) {
        ns6_capture_slot_t *slot = &priv->capture[i];
        slot->dev = dev;
        slot->transfer = libusb_alloc_transfer(0);
        if (!slot->transfer) goto fail;
        slot->sequence = priv->capture_next_submit++;
        libusb_fill_bulk_transfer(slot->transfer, dev->usb, NS6_EP_CAPTURE,
                slot->buffer, NS6_CAPTURE_READ_SIZE, capture_in_cb, slot, 0);
        int r = libusb_submit_transfer(slot->transfer);
        if (r < 0) {
            fprintf(stderr, "ns6: capture IN submit[%zu]: %s\n",
                    i, libusb_strerror(r));
            goto fail;
        }
    }
    printf("ns6: capture input active (%d ordered USB reads queued)\n",
           NS6_CAPTURE_READ_SLOTS);

    /* The device's feedback stream is the clock reference for ISO OUT. */
    start_feedback_reads(dev, priv);

    /* Pace EP 0x02 from feedback, falling back to the nominal 44.1 kHz rate. */
    for (int i = 0; i < 2; i++) {
        ns6_iso_slot_t *slot = &priv->iso_out[i];
        slot->dev = dev;
        slot->transfer = alloc_iso_transfer(
            dev->usb, 0x02, slot, NS6_ISO_PACKETS_PER_TRANSFER);
        if (slot->transfer) {
            audio_out_cb(slot->transfer);
        } else {
            fprintf(stderr, "ns6: unable to allocate ISO output transfer\n");
            goto fail;
        }
    }

    printf("ns6: all transfers running\n");

    while (dev->running) {
        struct timeval tv = { 0, 10000 };
        int r = libusb_handle_events_timeout_completed(dev->ctx, &tv, NULL);
        if (r < 0 && r != LIBUSB_ERROR_INTERRUPTED) {
            fprintf(stderr, "ns6: handle_events: %s\n", libusb_strerror(r));
            dev->running = false;
            return -1;
        }
    }
    return dev->transport_lost ? -1 : 0;

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
        for (size_t i = 0; i < NS6_CAPTURE_READ_SLOTS; ++i) {
            if (priv->capture[i].transfer)
                libusb_cancel_transfer(priv->capture[i].transfer);
        }
        for (int i = 0; i < 2; ++i) {
            if (priv->iso_out[i].transfer)
                libusb_cancel_transfer(priv->iso_out[i].transfer);
        }
        for (int i = 0; i < NS6_FEEDBACK_SLOTS; ++i) {
            if (priv->feedback_xfer[i])
                libusb_cancel_transfer(priv->feedback_xfer[i]);
        }
        free(priv);
    }

    libusb_release_interface(dev->usb, 1);
    libusb_release_interface(dev->usb, 0);
    libusb_close(dev->usb);
    libusb_exit(dev->ctx);
    free(dev);
    printf("ns6: closed\n");
}
