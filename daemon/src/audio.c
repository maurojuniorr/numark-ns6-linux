#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <alsa/asoundlib.h>
#include "ns6.h"

/* ------------------------------------------------------------------ */
/* Configuração do PCM                                                  */
/* ------------------------------------------------------------------ */
#define PCM_DEVICE_OUT      "Numark NS6 OUT Daemon"
#define PCM_DEVICE_IN       "Numark NS6 IN Daemon"
#define PCM_CHANNELS_OUT    4           /* L1 R1 L2 R2                */
#define PCM_CHANNELS_IN     2           /* L R (line in)              */
#define PCM_RATE            44100
#define PCM_FORMAT          SND_PCM_FORMAT_S24_3LE  /* 24-bit LE packed */
#define PCM_PERIODS         8
#define PCM_PERIOD_FRAMES   NS6_FRAMES_PER_URB      /* 176 frames/4ms  */
#define PCM_BUFFER_FRAMES   (PCM_PERIOD_FRAMES * PCM_PERIODS)

/* Nível mínimo antes de aceitar pull — absorve jitter */
#define PCM_MIN_FILL        (PCM_PERIOD_FRAMES * 3)  /* 528 frames = 12ms */

/* ------------------------------------------------------------------ */
/* Estado do subsistema de áudio                                        */
/* ------------------------------------------------------------------ */
typedef struct {
    snd_pcm_t      *pcm_out;
    snd_pcm_t      *pcm_in;
    uint8_t        *out_buf;
    uint8_t        *in_buf;
    int             out_frames;
    int             in_frames;
    uint8_t         last_frame[NS6_FRAME_BYTES]; /* último frame válido */
    pthread_mutex_t out_mutex;
    pthread_mutex_t in_mutex;
    pthread_cond_t  out_cond;
    pthread_cond_t  in_cond;
    bool            running;
    ns6_device_t   *dev;
} ns6_audio_t;

static ns6_audio_t g_audio = {0};

/* ------------------------------------------------------------------ */
/* Abre e configura um PCM ALSA                                         */
/* ------------------------------------------------------------------ */
static snd_pcm_t *open_pcm(const char *name,
                            snd_pcm_stream_t stream,
                            int channels)
{
    snd_pcm_t *pcm = NULL;
    snd_pcm_hw_params_t *hw;
    int r;

    r = snd_pcm_open(&pcm, name, stream, SND_PCM_NONBLOCK);
    if (r < 0) {
        fprintf(stderr, "audio: snd_pcm_open(%s): %s\n",
                name, snd_strerror(r));
        return NULL;
    }

    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(pcm, hw);

    snd_pcm_hw_params_set_access(pcm, hw,
        SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm, hw, PCM_FORMAT);

    unsigned int rate = PCM_RATE;
    snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, NULL);

    snd_pcm_hw_params_set_channels(pcm, hw, channels);

    snd_pcm_uframes_t period = PCM_PERIOD_FRAMES;
    snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, NULL);

    snd_pcm_uframes_t buf = PCM_BUFFER_FRAMES;
    snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buf);

    r = snd_pcm_hw_params(pcm, hw);
    if (r < 0) {
        fprintf(stderr, "audio: hw_params(%s): %s\n",
                name, snd_strerror(r));
        snd_pcm_close(pcm);
        return NULL;
    }

    snd_pcm_get_params(pcm, &buf, &period);
    printf("audio: PCM %s | %s | %dch | %uHz | period=%lu buf=%lu frames\n",
           name,
           stream == SND_PCM_STREAM_PLAYBACK ? "playback" : "capture",
           channels, rate, period, buf);
    return pcm;
}

/* ------------------------------------------------------------------ */
/* Thread de playback — lê do ALSA, empacota pro USB ISO               */
/* ------------------------------------------------------------------ */
static void *playback_thread(void *arg)
{
    ns6_audio_t *a = (ns6_audio_t *)arg;

    /*
     * Frame de saída: 4 canais × 3 bytes = 12 bytes
     * Lemos PCM_PERIOD_FRAMES de uma vez e colocamos no out_buf
     */
    size_t frame_bytes = PCM_CHANNELS_OUT * NS6_BYTES_PER_SAMPLE;
    size_t period_bytes = PCM_PERIOD_FRAMES * frame_bytes;
    uint8_t *buf = calloc(period_bytes, 1);
    if (!buf) return NULL;

    snd_pcm_prepare(a->pcm_out);

    /* Pré-enche o buffer antes de começar — evita underrun inicial */
    int prebuf_count = 0;
    while (a->running && prebuf_count < 6) {
        snd_pcm_sframes_t n = snd_pcm_readi(a->pcm_out, buf,
                                              PCM_PERIOD_FRAMES);
        if (n == -EAGAIN) { usleep(1000); continue; }
        if (n < 0) { snd_pcm_recover(a->pcm_out, n, 1); continue; }
        pthread_mutex_lock(&a->out_mutex);
        int space = PCM_BUFFER_FRAMES - a->out_frames;
        int copy  = n < space ? (int)n : space;
        if (copy > 0) {
            memcpy(a->out_buf + a->out_frames * NS6_FRAME_BYTES,
                   buf, copy * NS6_FRAME_BYTES);
            a->out_frames += copy;
        }
        pthread_mutex_unlock(&a->out_mutex);
        prebuf_count++;
    }
    printf("audio: pre-buffer ready (%d frames)\n", prebuf_count * PCM_PERIOD_FRAMES);

    int debug_count = 0;
    while (a->running) {
        snd_pcm_sframes_t n = snd_pcm_readi(a->pcm_out, buf,
                                              PCM_PERIOD_FRAMES);
        if (n == -EAGAIN) {
            usleep(1000);
            continue;
        }
        if (n < 0) {
            fprintf(stderr, "audio: readi: %s\n", snd_strerror(n));
            snd_pcm_recover(a->pcm_out, n, 1);
            continue;
        }

        /* Debug: mostra se há sinal */
        if (++debug_count % 250 == 0) {
            int32_t *s = (int32_t *)buf;
            int has_signal = 0;
            for (int i = 0; i < n * PCM_CHANNELS_OUT; i++)
                if (s[i] != 0) { has_signal = 1; break; }
            printf("audio: playback %s (%ld frames)\n",
                   has_signal ? "SIGNAL" : "silence", n);
        }

        /* Acumula no ring buffer que o ISO OUT vai consumir */
        pthread_mutex_lock(&a->out_mutex);
        int space = PCM_BUFFER_FRAMES - a->out_frames;
        int copy  = (int)n < space ? (int)n : space;
        if (copy > 0) {
            memcpy(a->out_buf + a->out_frames * NS6_FRAME_BYTES,
                   buf, copy * NS6_FRAME_BYTES);
            a->out_frames += copy;

            /* Debug formato — mostra primeiro frame */
            static int fmt_count = 0;
            if (++fmt_count % 500 == 0) {
                uint8_t *f = buf;
                printf("audio: frame[0] L1=%02x%02x%02x R1=%02x%02x%02x L2=%02x%02x%02x R2=%02x%02x%02x\n",
                       f[0],f[1],f[2], f[3],f[4],f[5],
                       f[6],f[7],f[8], f[9],f[10],f[11]);
            }
            pthread_cond_signal(&a->out_cond);
        }
        pthread_mutex_unlock(&a->out_mutex);
    }

    free(buf);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Thread de capture — recebe do USB ISO, entrega pro ALSA             */
/* ------------------------------------------------------------------ */
static void *capture_thread(void *arg)
{
    ns6_audio_t *a = (ns6_audio_t *)arg;

    size_t frame_bytes = PCM_CHANNELS_IN * NS6_BYTES_PER_SAMPLE;
    size_t period_bytes = PCM_PERIOD_FRAMES * frame_bytes;
    uint8_t *buf = calloc(period_bytes, 1);
    if (!buf) return NULL;

    snd_pcm_prepare(a->pcm_in);
    snd_pcm_start(a->pcm_in);

    while (a->running) {
        /* Aguarda dados do callback USB */
        pthread_mutex_lock(&a->in_mutex);
        while (a->in_frames == 0 && a->running)
            pthread_cond_wait(&a->in_cond, &a->in_mutex);

        if (!a->running) {
            pthread_mutex_unlock(&a->in_mutex);
            break;
        }

        int n = a->in_frames;
        memcpy(buf, a->in_buf, n * frame_bytes);
        a->in_frames = 0;
        pthread_mutex_unlock(&a->in_mutex);

        snd_pcm_sframes_t written = snd_pcm_writei(a->pcm_in, buf, n);
        if (written < 0) {
            snd_pcm_recover(a->pcm_in, written, 1);
        }
    }

    free(buf);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Callback chamado pelo usb.c quando chega áudio do EP 0x81          */
/* ------------------------------------------------------------------ */
void ns6_audio_push_capture(const int32_t *frames, int n_frames)
{
    ns6_audio_t *a = &g_audio;
    if (!a->running || !a->pcm_in) return;

    size_t frame_bytes = PCM_CHANNELS_IN * NS6_BYTES_PER_SAMPLE;

    pthread_mutex_lock(&a->in_mutex);
    int copy = n_frames < PCM_PERIOD_FRAMES ? n_frames : PCM_PERIOD_FRAMES;
    for (int i = 0; i < copy; i++) {
        /* L channel — 24-bit LE packed */
        int32_t l = frames[i * 4 + 0];
        int32_t r = frames[i * 4 + 1];
        uint8_t *dst = a->in_buf + i * frame_bytes;
        dst[0] = (l      ) & 0xFF;
        dst[1] = (l >>  8) & 0xFF;
        dst[2] = (l >> 16) & 0xFF;
        dst[3] = (r      ) & 0xFF;
        dst[4] = (r >>  8) & 0xFF;
        dst[5] = (r >> 16) & 0xFF;
    }
    a->in_frames = copy;
    pthread_cond_signal(&a->in_cond);
    pthread_mutex_unlock(&a->in_mutex);
}

/* ------------------------------------------------------------------ */
/* Chamado pelo usb.c para preencher buffer ISO OUT (EP 0x02)          */
/* ------------------------------------------------------------------ */
int ns6_audio_pull_playback(uint8_t *iso_buf, int max_bytes)
{
    ns6_audio_t *a = &g_audio;

    int frames_wanted = max_bytes / NS6_FRAME_BYTES;
    size_t out_bytes  = frames_wanted * NS6_FRAME_BYTES;

    pthread_mutex_lock(&a->out_mutex);

    if (a->out_frames >= frames_wanted) {
        memcpy(iso_buf, a->out_buf, out_bytes);
        /* Salva último frame válido */
        memcpy(a->last_frame,
               a->out_buf + (frames_wanted - 1) * NS6_FRAME_BYTES,
               NS6_FRAME_BYTES);
        a->out_frames -= frames_wanted;
        if (a->out_frames > 0)
            memmove(a->out_buf,
                    a->out_buf + out_bytes,
                    a->out_frames * NS6_FRAME_BYTES);
        pthread_mutex_unlock(&a->out_mutex);
        return out_bytes;
    }

    /* Underrun — preenche com o último frame válido (evita pop) */
    if (a->out_frames > 0) {
        memcpy(iso_buf, a->out_buf, a->out_frames * NS6_FRAME_BYTES);
        memcpy(a->last_frame,
               a->out_buf + (a->out_frames - 1) * NS6_FRAME_BYTES,
               NS6_FRAME_BYTES);
        a->out_frames = 0;
    }
    /* Completa com último frame repetido em vez de zeros */
    for (int i = 0; i < frames_wanted; i++) {
        memcpy(iso_buf + i * NS6_FRAME_BYTES,
               a->last_frame, NS6_FRAME_BYTES);
    }
    pthread_mutex_unlock(&a->out_mutex);
    return max_bytes;
}

/* ------------------------------------------------------------------ */
/* Inicializa o subsistema de áudio                                     */
/* ------------------------------------------------------------------ */
int ns6_audio_init(ns6_device_t *dev)
{
    ns6_audio_t *a = &g_audio;
    memset(a, 0, sizeof(*a));
    a->dev     = dev;
    a->running = true;

    pthread_mutex_init(&a->out_mutex, NULL);
    pthread_mutex_init(&a->in_mutex,  NULL);
    pthread_cond_init(&a->out_cond,   NULL);
    pthread_cond_init(&a->in_cond,    NULL);

    /* Aloca ring buffers */
    size_t out_sz = PCM_BUFFER_FRAMES * NS6_FRAME_BYTES;
    size_t in_sz  = PCM_BUFFER_FRAMES * PCM_CHANNELS_IN * NS6_BYTES_PER_SAMPLE;
    a->out_buf = calloc(out_sz, 1);
    a->in_buf  = calloc(in_sz,  1);
    if (!a->out_buf || !a->in_buf) {
        fprintf(stderr, "audio: out of memory\n");
        return -1;
    }

    /*
     * Tenta abrir dispositivos ALSA.
     * O usuário precisa criar o arquivo ~/.asoundrc com:
     *
     *   pcm.ns6_out {
     *       type plug
     *       slave { pcm "hw:Loopback,0" rate 44100 channels 4 }
     *   }
     *   pcm.ns6_in {
     *       type plug
     *       slave { pcm "hw:Loopback,1" rate 44100 channels 2 }
     *   }
     *
     * Ou usar snd-aloop (ALSA loopback).
     */
    a->pcm_out = open_pcm(PCM_DEVICE_OUT, SND_PCM_STREAM_CAPTURE,
                          PCM_CHANNELS_OUT);
    a->pcm_in  = open_pcm(PCM_DEVICE_IN,  SND_PCM_STREAM_PLAYBACK,
                          PCM_CHANNELS_IN);

    if (!a->pcm_out || !a->pcm_in) {
        fprintf(stderr,
            "audio: PCM devices not found.\n"
            "       Configure ~/.asoundrc com ns6_out e ns6_in\n"
            "       ou carregue o modulo: sudo modprobe snd-aloop\n");
        return -1;
    }

    /* Inicia threads */
    pthread_t pb_thr, cap_thr;
    pthread_create(&pb_thr,  NULL, playback_thread, a);  /* lê do Mixxx → USB */
    pthread_create(&cap_thr, NULL, capture_thread,  a);  /* USB → escreve pro Mixxx */
    pthread_detach(pb_thr);
    pthread_detach(cap_thr);

    printf("audio: subsystem started\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* Para o subsistema de áudio                                           */
/* ------------------------------------------------------------------ */
void ns6_audio_stop(void)
{
    ns6_audio_t *a = &g_audio;
    a->running = false;

    pthread_cond_broadcast(&a->out_cond);
    pthread_cond_broadcast(&a->in_cond);

    if (a->pcm_out) { snd_pcm_close(a->pcm_out); a->pcm_out = NULL; }
    if (a->pcm_in)  { snd_pcm_close(a->pcm_in);  a->pcm_in  = NULL; }

    free(a->out_buf); a->out_buf = NULL;
    free(a->in_buf);  a->in_buf  = NULL;

    printf("audio: subsystem stopped\n");
}
