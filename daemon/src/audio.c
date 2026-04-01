#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <pthread.h>
#include <alsa/asoundlib.h>
#include "ns6.h"

#define PCM_DEVICE_OUT      "hw:0,1"
#define PCM_CHANNELS_OUT    4
#define PCM_RATE            44100
#define PCM_FORMAT          SND_PCM_FORMAT_S24_3LE

/* ------------------------------------------------------------------ */
/* Ring Buffer — dimensionado para absorver drift e scheduler jitter    */
/* ------------------------------------------------------------------ */
#define RING_SIZE           22050   /* 500ms capacidade total */
#define RING_BYTES          (RING_SIZE * 12)
#define TARGET_FILL         4410    /* 100ms — nível ideal de operação */
#define LOW_WATER           882     /* 20ms — abaixo disso entra em buffering */
#define HIGH_WATER          8820    /* 200ms — acima disso descarta suavemente */
#define STARTUP_FILL        4410    /* 100ms — quanto pre-encher antes de começar */

static snd_pcm_t *pcm_out = NULL;
static bool audio_running = false;

static uint8_t ring_buf[RING_BYTES];
static int ring_rd = 0;
static int ring_wr = 0;
static int ring_fill = 0;
static bool is_buffering = true;
static pthread_mutex_t ring_lock = PTHREAD_MUTEX_INITIALIZER;

/* Contadores de debug (sem lock, informativos) */
static unsigned long dbg_underruns = 0;
static unsigned long dbg_overruns = 0;
static unsigned long dbg_pulls = 0;

int ns6_audio_init(ns6_device_t *dev) {
    (void)dev;
    int err;
    snd_pcm_hw_params_t *params;

    if ((err = snd_pcm_open(&pcm_out, PCM_DEVICE_OUT,
                             SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK)) < 0) {
        fprintf(stderr, "audio: Não abriu %s (%s)\n",
                PCM_DEVICE_OUT, snd_strerror(err));
        return -1;
    }

    snd_pcm_hw_params_alloca(&params);
    snd_pcm_hw_params_any(pcm_out, params);
    snd_pcm_hw_params_set_access(pcm_out, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm_out, params, PCM_FORMAT);
    snd_pcm_hw_params_set_channels(pcm_out, params, PCM_CHANNELS_OUT);

    unsigned int rate = PCM_RATE;
    snd_pcm_hw_params_set_rate_near(pcm_out, params, &rate, 0);

    snd_pcm_uframes_t buffer_size = 4096;
    snd_pcm_uframes_t period_size = 128;
    snd_pcm_hw_params_set_buffer_size_near(pcm_out, params, &buffer_size);
    snd_pcm_hw_params_set_period_size_near(pcm_out, params, &period_size, 0);

    if ((err = snd_pcm_hw_params(pcm_out, params)) < 0) {
        fprintf(stderr, "audio: Erro hw_params: %s\n", snd_strerror(err));
        snd_pcm_close(pcm_out);
        return -1;
    }

    snd_pcm_prepare(pcm_out);
    snd_pcm_start(pcm_out);

    /* Pre-fill: enche o ring com silêncio limpo.
     * Os primeiros URBs vão consumir zeros enquanto o Mixxx arranca. */
    pthread_mutex_lock(&ring_lock);
    memset(ring_buf, 0, RING_BYTES);
    ring_rd = 0;
    ring_wr = STARTUP_FILL;
    ring_fill = STARTUP_FILL;
    is_buffering = false;   /* Já temos 100ms, pode começar direto */
    pthread_mutex_unlock(&ring_lock);

    audio_running = true;
    printf("audio: Ring Buffer OK (pre-fill=%dms, target=%dms, high=%dms)\n",
           STARTUP_FILL * 1000 / 44100,
           TARGET_FILL * 1000 / 44100,
           HIGH_WATER * 1000 / 44100);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Pump — chamado pela thread dedicada, enche o ring a partir do ALSA   */
/* ------------------------------------------------------------------ */
void ns6_audio_pump(void) {
    if (!audio_running || !pcm_out) return;

    snd_pcm_sframes_t avail = snd_pcm_avail_update(pcm_out);
    if (avail <= 0) {
        if (avail < 0) {
            snd_pcm_recover(pcm_out, avail, 1);
            snd_pcm_start(pcm_out);
        }
        return;
    }

    pthread_mutex_lock(&ring_lock);
    int can_write = RING_SIZE - ring_fill;
    pthread_mutex_unlock(&ring_lock);

    if (can_write <= 0) return;
    if (avail > can_write) avail = can_write;

    /* Leitura em bloco do ALSA */
    uint8_t tmp[RING_BYTES];
    int frames_read = snd_pcm_readi(pcm_out, tmp, avail);

    if (frames_read <= 0) {
        if (frames_read < 0) {
            snd_pcm_recover(pcm_out, frames_read, 1);
            snd_pcm_start(pcm_out);
        }
        return;
    }

    pthread_mutex_lock(&ring_lock);
    int bytes = frames_read * 12;
    int wr_byte = ring_wr * 12;
    int space_to_end = RING_BYTES - wr_byte;

    if (bytes <= space_to_end) {
        memcpy(ring_buf + wr_byte, tmp, bytes);
    } else {
        memcpy(ring_buf + wr_byte, tmp, space_to_end);
        memcpy(ring_buf, tmp + space_to_end, bytes - space_to_end);
    }

    ring_wr = (ring_wr + frames_read) % RING_SIZE;
    ring_fill += frames_read;

    /* Overrun suave: se passou de HIGH_WATER, descarta apenas o excesso
     * acima de TARGET, mas de forma gradual — no máximo 441 frames (10ms)
     * por ciclo de pump para evitar descontinuidades audíveis. */
    if (ring_fill > HIGH_WATER) {
        int excess = ring_fill - TARGET_FILL;
        int discard = excess;
        if (discard > 441) discard = 441;  /* Max 10ms por ciclo */
        ring_rd = (ring_rd + discard) % RING_SIZE;
        ring_fill -= discard;
        dbg_overruns++;
    }
    pthread_mutex_unlock(&ring_lock);
}

/* ------------------------------------------------------------------ */
/* Pull — chamado pelo callback ISO, NUNCA toca no ALSA                 */
/* ------------------------------------------------------------------ */
int ns6_audio_pull_playback(uint8_t *iso_buf, int max_bytes) {
    int frames_needed = max_bytes / 12;

    pthread_mutex_lock(&ring_lock);

    /* Modo buffering: entrega silêncio até ter reserva suficiente */
    if (is_buffering) {
        if (ring_fill >= TARGET_FILL) {
            is_buffering = false;
            printf("audio: Buffering completo (%d frames), áudio ativo.\n",
                   ring_fill);
        } else {
            pthread_mutex_unlock(&ring_lock);
            memset(iso_buf, 0, max_bytes);
            return max_bytes;
        }
    }

    /* Underrun: ring secou. Entra em buffering ao invés de mandar
     * fragmentos que causariam chiado. */
    if (ring_fill < frames_needed) {
        is_buffering = true;
        dbg_underruns++;
        printf("audio: UNDERRUN #%lu (fill=%d), entrando em buffering.\n",
               dbg_underruns, ring_fill);
        pthread_mutex_unlock(&ring_lock);
        memset(iso_buf, 0, max_bytes);
        return max_bytes;
    }

    /* Fluxo normal */
    int rd_byte = ring_rd * 12;
    int space_to_end = RING_BYTES - rd_byte;

    if (max_bytes <= space_to_end) {
        memcpy(iso_buf, ring_buf + rd_byte, max_bytes);
    } else {
        memcpy(iso_buf, ring_buf + rd_byte, space_to_end);
        memcpy(iso_buf + space_to_end, ring_buf, max_bytes - space_to_end);
    }

    ring_rd = (ring_rd + frames_needed) % RING_SIZE;
    ring_fill -= frames_needed;

    /* Log periódico (a cada ~10s) */
    dbg_pulls++;
    if ((dbg_pulls % 2500) == 0) {
        printf("audio: ring=%d/%d (%dms) underruns=%lu overruns=%lu\n",
               ring_fill, RING_SIZE, ring_fill * 1000 / 44100,
               dbg_underruns, dbg_overruns);
    }

    pthread_mutex_unlock(&ring_lock);
    return max_bytes;
}

/* ------------------------------------------------------------------ */
/* Getter para o nível do ring (usado pelo drift compensation no USB)   */
/* ------------------------------------------------------------------ */
int ns6_audio_ring_fill(void) {
    return ring_fill;
}

void ns6_audio_stop(void) {
    audio_running = false;
    if (pcm_out) {
        snd_pcm_drop(pcm_out);
        snd_pcm_close(pcm_out);
        pcm_out = NULL;
    }
    printf("audio: Parado. underruns=%lu overruns=%lu\n",
           dbg_underruns, dbg_overruns);
}
