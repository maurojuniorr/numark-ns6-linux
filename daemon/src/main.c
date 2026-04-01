#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <alsa/asoundlib.h>
#include "ns6.h"

typedef struct {
    snd_seq_t  *seq;
    int         port;
    int         client;
} ns6_midi_t;

static ns6_midi_t    g_midi = {0};
static ns6_device_t *g_dev  = NULL;
static volatile bool g_quit = false;

static int midi_open(ns6_midi_t *m)
{
    int r = snd_seq_open(&m->seq, "default", SND_SEQ_OPEN_DUPLEX, 0);
    if (r < 0) return r;

    snd_seq_set_client_name(m->seq, "Numark NS6");
    m->client = snd_seq_client_id(m->seq);
    snd_seq_set_input_buffer_size(m->seq, 65536);
    snd_seq_set_output_buffer_size(m->seq, 65536);

    snd_seq_client_info_t *cinfo;
    snd_seq_client_info_alloca(&cinfo);
    snd_seq_get_client_info(m->seq, cinfo);
    snd_seq_client_info_set_midi_version(cinfo, SND_SEQ_CLIENT_LEGACY_MIDI);
    snd_seq_set_client_info(m->seq, cinfo);

    m->port = snd_seq_create_simple_port(m->seq, "NS6 MIDI",
        SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ |
        SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    
    return 0;
}

static void midi_close(ns6_midi_t *m)
{
    if (m->seq) snd_seq_close(m->seq);
}

static void on_midi_in(ns6_device_t *dev, uint8_t status, uint8_t note, uint8_t value)
{
    (void)dev;
    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);
    snd_seq_ev_set_source(&ev, g_midi.port);
    snd_seq_ev_set_subs(&ev);
    snd_seq_ev_set_direct(&ev);

    uint8_t cmd = status & 0xF0;
    if (cmd == 0x90 || cmd == 0x80) {
        if (cmd == 0x90 && value > 0) snd_seq_ev_set_noteon(&ev, status & 0x0F, note, value);
        else snd_seq_ev_set_noteoff(&ev, status & 0x0F, note, value);
    } else if (cmd == 0xB0) {
        snd_seq_ev_set_controller(&ev, status & 0x0F, note, value);
    } else if (cmd == 0xE0) {
        int pitch = (note & 0x7F) | ((value & 0x7F) << 7);
        snd_seq_ev_set_pitchbend(&ev, status & 0x0F, pitch - 8192);
    }
    snd_seq_event_output_direct(g_midi.seq, &ev);
}

static void *midi_in_thread(void *arg)
{
    ns6_device_t *dev = (ns6_device_t *)arg;
    snd_seq_event_t *ev;
    while (!g_quit && snd_seq_event_input(g_midi.seq, &ev) >= 0) {
        if (ev->type == SND_SEQ_EVENT_NOTEON)
            ns6_send_midi(dev, 0x90 | ev->data.note.channel, ev->data.note.note, ev->data.note.velocity);
        else if (ev->type == SND_SEQ_EVENT_NOTEOFF)
            ns6_send_midi(dev, 0x80 | ev->data.note.channel, ev->data.note.note, ev->data.note.velocity);
        else if (ev->type == SND_SEQ_EVENT_CONTROLLER)
            ns6_send_midi(dev, 0xB0 | ev->data.control.channel, ev->data.control.param, ev->data.control.value);
    }
    return NULL;
}

static void sig_handler(int sig)
{
    (void)sig;
    g_quit = true;
    if (g_dev) g_dev->running = false;
    ns6_midi_out_wake();
}

int main(void)
{
    printf("NS6 Daemon — ALSA Ultimate Edition (Anti-Crash, Anti-Drift & Smart MIDI)\n\n");
    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    if (midi_open(&g_midi) < 0) {
        fprintf(stderr, "ns6d: failed to open ALSA MIDI\n");
        return 1;
    }

    g_dev = ns6_open();
    if (!g_dev) {
        fprintf(stderr, "ns6d: failed to open NS6 (Execute com sudo?)\n");
        midi_close(&g_midi);
        return 1;
    }

    g_dev->on_midi_in = on_midi_in;

    if (ns6_init(g_dev) < 0) {
        ns6_close(g_dev);
        midi_close(&g_midi);
        return 1;
    }

    /* Inicia o motor de Áudio ALSA *antes* de ligar as chaves do USB */
    if (ns6_audio_init(g_dev) < 0) {
        ns6_close(g_dev);
        midi_close(&g_midi);
        return 1;
    }

    g_dev->running = true;

    pthread_t thr_midi_in, thr_midi_out;
    pthread_create(&thr_midi_in, NULL, midi_in_thread, g_dev);
    pthread_create(&thr_midi_out, NULL, ns6_midi_out_worker, g_dev);
    
    /* Esta função segura o terminal até o programa fechar */
    ns6_run(g_dev);

    g_quit = true;
    ns6_audio_stop();
    pthread_cancel(thr_midi_in);
    pthread_join(thr_midi_in, NULL);
    pthread_join(thr_midi_out, NULL);
    
    ns6_close(g_dev);
    midi_close(&g_midi);
    printf("ns6d: daemon fechado com sucesso.\n");
    return 0;
}