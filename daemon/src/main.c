#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>
#include <alsa/asoundlib.h>
#include "ns6.h"

enum { MIDI_INPUT_QUEUE_CAPACITY = 4096, MIDI_INPUT_BATCH_SIZE = 256 };

typedef struct {
    uint8_t status;
    uint8_t note;
    uint8_t value;
} ns6_midi_message_t;

/* ------------------------------------------------------------------ */
/* ALSA virtual MIDI port                                               */
/* ------------------------------------------------------------------ */
typedef struct {
    snd_seq_t  *seq;
    int         port;
    int         client;
    ns6_midi_message_t input_queue[MIDI_INPUT_QUEUE_CAPACITY];
    atomic_uint input_read;
    atomic_uint input_write;
    atomic_uint input_dropped;
} ns6_midi_t;

static ns6_midi_t    g_midi = {0};
static ns6_device_t *g_dev  = NULL;
static volatile bool g_quit = false;
static volatile sig_atomic_t g_usb_failed = 0;

static int midi_open(ns6_midi_t *m)
{
    atomic_init(&m->input_read, 0);
    atomic_init(&m->input_write, 0);
    atomic_init(&m->input_dropped, 0);

    int r = snd_seq_open(&m->seq, "default", SND_SEQ_OPEN_DUPLEX, 0);
    if (r < 0) {
        fprintf(stderr, "ns6d: snd_seq_open: %s\n", snd_strerror(r));
        return r;
    }

    snd_seq_set_client_name(m->seq, "Numark NS6 MIDI Bridge");
    m->client = snd_seq_client_id(m->seq);

    /* Buffer grande pra aguentar burst de LEDs do Mixxx */
    snd_seq_set_input_buffer_size(m->seq, 262144);
    snd_seq_set_output_buffer_size(m->seq, 65536);

    snd_seq_client_pool_t *pool;
    snd_seq_client_pool_alloca(&pool);
    snd_seq_get_client_pool(m->seq, pool);
    snd_seq_client_pool_set_input_pool(pool, 16384);
    snd_seq_client_pool_set_output_pool(pool, 2048);
    snd_seq_set_client_pool(m->seq, pool);

    m->port = snd_seq_create_simple_port(
        m->seq, "Numark USB Audio Device MIDI",
        SND_SEQ_PORT_CAP_READ  | SND_SEQ_PORT_CAP_WRITE |
        SND_SEQ_PORT_CAP_SUBS_READ | SND_SEQ_PORT_CAP_SUBS_WRITE,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_HARDWARE);

    if (m->port < 0) {
        fprintf(stderr, "ns6d: create_port: %s\n", snd_strerror(m->port));
        snd_seq_close(m->seq);
        return m->port;
    }

    snd_seq_nonblock(m->seq, 1);

    printf("ns6d: ALSA MIDI port: client=%d port=%d\n", m->client, m->port);
    printf("ns6d: connect with: aconnect %d:0 <target>\n", m->client);
    return 0;
}

static void midi_queue_input(ns6_midi_t *m,
                             uint8_t status, uint8_t note, uint8_t value)
{
    unsigned write = atomic_load_explicit(&m->input_write, memory_order_relaxed);
    unsigned read = atomic_load_explicit(&m->input_read, memory_order_acquire);
    if (write - read == MIDI_INPUT_QUEUE_CAPACITY) {
        atomic_fetch_add_explicit(&m->input_dropped, 1, memory_order_relaxed);
        return;
    }

    m->input_queue[write % MIDI_INPUT_QUEUE_CAPACITY] =
            (ns6_midi_message_t){ status, note, value };
    atomic_store_explicit(&m->input_write, write + 1, memory_order_release);
}

static int midi_emit(ns6_midi_t *m, const ns6_midi_message_t *message)
{
    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);
    snd_seq_ev_set_source(&ev, m->port);
    snd_seq_ev_set_subs(&ev);
    snd_seq_ev_set_direct(&ev);

    uint8_t msg = message->status & 0xF0;
    uint8_t ch  = message->status & 0x0F;

    switch (msg) {
    case 0x90: snd_seq_ev_set_noteon(&ev, ch, message->note, message->value);       break;
    case 0x80: snd_seq_ev_set_noteoff(&ev, ch, message->note, message->value);      break;
    case 0xB0: snd_seq_ev_set_controller(&ev, ch, message->note, message->value);   break;
    default:   return 0;
    }

    int r = snd_seq_event_output(m->seq, &ev);
    if (r < 0) {
        int drain = snd_seq_drain_output(m->seq);
        if (drain < 0)
            fprintf(stderr, "ns6d: ALSA MIDI output drain: %s\n", snd_strerror(drain));
        r = snd_seq_event_output(m->seq, &ev);
    }
    return r;
}

static void midi_flush_input(ns6_midi_t *m)
{
    unsigned read = atomic_load_explicit(&m->input_read, memory_order_relaxed);
    unsigned write = atomic_load_explicit(&m->input_write, memory_order_acquire);
    unsigned count = 0;

    while (read != write && count < MIDI_INPUT_BATCH_SIZE) {
        const ns6_midi_message_t *message =
                &m->input_queue[read % MIDI_INPUT_QUEUE_CAPACITY];
        int r = midi_emit(m, message);
        if (r < 0) {
            fprintf(stderr, "ns6d: ALSA MIDI output dropped %02x %02x %02x: %s\n",
                    message->status, message->note, message->value, snd_strerror(r));
        }
        ++read;
        ++count;
    }

    atomic_store_explicit(&m->input_read, read, memory_order_release);
    if (count > 0) {
        int r = snd_seq_drain_output(m->seq);
        if (r < 0)
            fprintf(stderr, "ns6d: ALSA MIDI output flush: %s\n", snd_strerror(r));
    }

    unsigned dropped = atomic_exchange_explicit(&m->input_dropped, 0,
                                                memory_order_relaxed);
    if (dropped > 0)
        fprintf(stderr, "ns6d: MIDI input queue overflow; dropped %u events\n", dropped);
}

static void midi_close(ns6_midi_t *m)
{
    if (m->seq) {
        snd_seq_delete_simple_port(m->seq, m->port);
        snd_seq_close(m->seq);
        m->seq = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* Callbacks da NS6                                                     */
/* ------------------------------------------------------------------ */
static void on_midi_in(ns6_device_t *dev,
                       uint8_t status, uint8_t note, uint8_t value)
{
    (void)dev;
    midi_queue_input(&g_midi, status, note, value);
}

/* ------------------------------------------------------------------ */
/* Thread LED — lê MIDI do Mixxx e envia pra controladora             */
/* ------------------------------------------------------------------ */
static void *midi_out_thread(void *arg)
{
    ns6_device_t *dev = (ns6_device_t *)arg;
    snd_seq_event_t *ev;

    printf("ns6d: LED thread started\n");

    while (dev->running) {
        midi_flush_input(&g_midi);
        int r = snd_seq_event_input(g_midi.seq, &ev);
        if (r == -EAGAIN) {
            usleep(500);
            continue;
        }
        if (r == -ENOSPC) {
            fprintf(stderr, "ns6d: ALSA MIDI input FIFO overran; events were lost\n");
            continue;
        }
        if (r < 0) {
            if (dev->running)
                fprintf(stderr, "ns6d: seq_event_input: %s\n",
                        snd_strerror(r));
            usleep(5000);
            continue;
        }

        uint8_t status = 0, note = 0, value = 0;

        switch (ev->type) {
        case SND_SEQ_EVENT_NOTEON:
            status = 0x90 | (ev->data.note.channel & 0x0F);
            note   = ev->data.note.note;
            value  = ev->data.note.velocity;
            break;
        case SND_SEQ_EVENT_NOTEOFF:
            status = 0x80 | (ev->data.note.channel & 0x0F);
            note   = ev->data.note.note;
            value  = 0;
            break;
        case SND_SEQ_EVENT_CONTROLLER:
            status = 0xB0 | (ev->data.control.channel & 0x0F);
            note   = ev->data.control.param;
            value  = ev->data.control.value & 0x7F;
            break;
        case SND_SEQ_EVENT_SYSEX: {
            /* SysEx — encaminha direto pro hardware */
            uint8_t *syx = (uint8_t *)ev->data.ext.ptr;
            int      len = (int)ev->data.ext.len;
            if (len > 0 && len <= NS6_CTRL_PKT_SIZE - 1) {
                uint8_t pkt[NS6_CTRL_PKT_SIZE];
                memset(pkt, NS6_IDLE_BYTE, NS6_CTRL_PKT_SIZE);
                memcpy(pkt, syx, len);
                pkt[NS6_CTRL_PKT_SIZE - 1] = NS6_PKT_TERMINATOR;
                if (ns6_send_ctrl_packet(dev, pkt) < 0)
                    fprintf(stderr, "ns6d: could not queue SysEx for USB output\n");
                printf("ns6d: SysEx → hardware (%d bytes)\n", len);
            }
            snd_seq_free_event(ev);
            continue;
        }
        default:
            snd_seq_free_event(ev);
            continue;
        }

        ns6_send_midi(dev, status, note, value);
        snd_seq_free_event(ev);
    }

    printf("ns6d: LED thread stopped\n");
    return NULL;
}

static void *usb_thread(void *arg)
{
    ns6_device_t *dev = (ns6_device_t *)arg;
    if (ns6_run(dev) < 0 && !g_quit)
        g_usb_failed = 1;
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Signal handler                                                       */
/* ------------------------------------------------------------------ */
static void sig_handler(int sig)
{
    (void)sig;
    printf("\nns6d: shutting down...\n");
    g_quit = true;
    if (g_dev) {
        g_dev->running = false;
        if (g_dev->ctx)
            libusb_interrupt_event_handler(g_dev->ctx);
    }
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */
int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--debug") == 0)
            ns6_debug_raw = 1;
    }

    printf("ns6d — Numark NS6 ALSA MIDI/PipeWire audio daemon\n");
    if (ns6_debug_raw)
        printf("DEBUG: raw packet mode ON\n");
    printf("\n");

    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    if (midi_open(&g_midi) < 0) {
        fprintf(stderr, "ns6d: failed to open ALSA MIDI\n");
        return 1;
    }

    g_dev = ns6_open();
    if (!g_dev) {
        fprintf(stderr, "ns6d: failed to open NS6\n");
        midi_close(&g_midi);
        return 1;
    }

    g_dev->on_midi_in = on_midi_in;

    if (ns6_init(g_dev) < 0) {
        fprintf(stderr, "ns6d: init failed\n");
        ns6_close(g_dev);
        midi_close(&g_midi);
        return 1;
    }

    g_dev->running = true;   /* antes de criar threads */
    if (ns6_audio_start(g_dev) < 0)
        fprintf(stderr, "ns6d: PipeWire audio unavailable; MIDI stays active and USB output is silent\n");

    printf("ns6d: running. Press Ctrl+C to stop.\n");
    while (!g_quit) {
        pthread_t usb_thr, led_thr, midi_out_thr;
        bool usb_started = false, led_started = false, midi_out_started = false;
        g_usb_failed = 0;
        g_dev->running = true;

        int r = pthread_create(&midi_out_thr, NULL, ns6_midi_out_worker, g_dev);
        if (r == 0) midi_out_started = true;
        if (r == 0) {
            r = pthread_create(&led_thr, NULL, midi_out_thread, g_dev);
            if (r == 0) led_started = true;
        }
        if (r == 0) {
            r = pthread_create(&usb_thr, NULL, usb_thread, g_dev);
            if (r == 0) usb_started = true;
        }
        if (r != 0) {
            fprintf(stderr, "ns6d: could not start worker thread: %s\n", strerror(r));
            g_quit = true;
        }

        while (!g_quit && !g_usb_failed)
            sleep(1);

        g_dev->running = false;
        if (g_dev->ctx) libusb_interrupt_event_handler(g_dev->ctx);
        ns6_midi_out_wake();
        if (usb_started) pthread_join(usb_thr, NULL);
        if (midi_out_started) pthread_join(midi_out_thr, NULL);
        if (led_started) pthread_join(led_thr, NULL);
        if (g_quit) break;

        fprintf(stderr, "ns6d: USB transport lost; reconnecting without replacing MIDI/PipeWire endpoints\n");
        ns6_release_controls(g_dev);
        while (atomic_load_explicit(&g_midi.input_read, memory_order_relaxed) !=
               atomic_load_explicit(&g_midi.input_write, memory_order_acquire))
            midi_flush_input(&g_midi);
        snd_seq_drop_input(g_midi.seq);
        ns6_midi_out_reset();

        while (!g_quit) {
            if (ns6_reopen(g_dev) == 0 && ns6_init(g_dev) == 0) {
                ns6_audio_reset(g_dev);
                fprintf(stderr, "ns6d: NS6 reconnected; Mixxx MIDI subscription preserved\n");
                break;
            }
            fprintf(stderr, "ns6d: NS6 not ready; retrying in 1 second\n");
            sleep(1);
        }
    }

    ns6_audio_stop(g_dev);
    ns6_close(g_dev);
    midi_close(&g_midi);
    printf("ns6d: bye\n");
    return 0;
}
