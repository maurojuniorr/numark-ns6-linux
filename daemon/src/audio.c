#include <pipewire/pipewire.h>
#include <pipewire/stream.h>
#include <pipewire/thread-loop.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw.h>
#include <spa/param/param.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ns6.h"
#include "ns6_audio_transport.h"

enum {
    AUDIO_RATE = 44100,
    NS6_OUTPUT_CHANNELS = 4,
    NS6_INPUT_CHANNELS = 2,
    NS6_OUTPUT_FRAME_BYTES = 12,
    NS6_INPUT_FRAME_BYTES = 6,
    NS6_INPUT_QUEUE_TARGET_FRAMES = 2048
};

typedef struct ns6_audio ns6_audio_t;

typedef struct {
    ns6_audio_t *audio;
    struct pw_stream *stream;
    const char *label;
} ns6_pw_stream_t;

struct ns6_audio {
    struct pw_thread_loop *loop;
    bool loop_started;
    ns6_pw_stream_t output;
    ns6_pw_stream_t input;
    atomic_uint_fast64_t output_overruns;
    atomic_uint_fast64_t output_underruns;
    atomic_uint_fast64_t input_drops;
    atomic_uint_fast64_t input_underruns;
    ns6_audio_ring_t output_ring;
    ns6_audio_ring_t input_ring;
    double output_phase;
    bool input_primed;
    ns6_capture_decoder_t capture_decoder;
};

static int32_t unpack_pcm24(const uint8_t *sample);
static void pack_pcm24(uint8_t *sample, int32_t value);

static void stream_state_changed(void *userdata, enum pw_stream_state old_state,
                                 enum pw_stream_state state, const char *error) {
    (void)old_state;
    ns6_pw_stream_t *endpoint = userdata;
    if (state == PW_STREAM_STATE_ERROR)
        fprintf(stderr, "ns6: PipeWire %s: %s\n", endpoint->label,
                error ? error : "stream error");
    else if (state == PW_STREAM_STATE_STREAMING)
        fprintf(stderr, "ns6: PipeWire %s active\n", endpoint->label);
}

static void output_process(void *userdata) {
    ns6_pw_stream_t *endpoint = userdata;
    ns6_audio_t *audio = endpoint->audio;
    struct pw_buffer *pw_buffer = pw_stream_dequeue_buffer(endpoint->stream);
    if (!pw_buffer) return;
    struct spa_buffer *buffer = pw_buffer->buffer;
    if (buffer && buffer->n_datas > 0 && buffer->datas[0].data &&
        buffer->datas[0].chunk) {
        struct spa_data *data = &buffer->datas[0];
        struct spa_chunk *chunk = data->chunk;
        if (chunk->stride == NS6_OUTPUT_FRAME_BYTES &&
            chunk->offset <= data->maxsize &&
            chunk->size <= data->maxsize - chunk->offset) {
            size_t frames = chunk->size / NS6_OUTPUT_FRAME_BYTES;
            size_t written = ns6_audio_ring_write(&audio->output_ring,
                    (const uint8_t *)data->data + chunk->offset, frames);
            if (written < frames)
                atomic_fetch_add_explicit(&audio->output_overruns,
                        frames - written, memory_order_relaxed);
        }
    }
    pw_stream_queue_buffer(endpoint->stream, pw_buffer);
}

static void input_process(void *userdata) {
    ns6_pw_stream_t *endpoint = userdata;
    ns6_audio_t *audio = endpoint->audio;
    struct pw_buffer *pw_buffer = pw_stream_dequeue_buffer(endpoint->stream);
    if (!pw_buffer) return;
    struct spa_buffer *buffer = pw_buffer->buffer;
    if (buffer && buffer->n_datas > 0 && buffer->datas[0].data &&
        buffer->datas[0].chunk) {
        struct spa_data *data = &buffer->datas[0];
        size_t frame_count = data->maxsize / NS6_INPUT_FRAME_BYTES;
        if (pw_buffer->requested > 0 && pw_buffer->requested < frame_count)
            frame_count = pw_buffer->requested;
        ns6_audio_ring_t *ring = &audio->input_ring;
        size_t read = atomic_load_explicit(&ring->read_index, memory_order_relaxed);
        size_t write = atomic_load_explicit(&ring->write_index, memory_order_acquire);
        size_t available = write - read;
        if (available >= NS6_INPUT_QUEUE_TARGET_FRAMES) audio->input_primed = true;
        size_t copied = 0;
        if (audio->input_primed) {
            copied = ns6_audio_ring_read(ring, data->data, frame_count);
        } else {
            memset(data->data, 0, frame_count * NS6_INPUT_FRAME_BYTES);
        }
        if (audio->input_primed && copied < frame_count) {
            atomic_fetch_add_explicit(&audio->input_underruns,
                    frame_count - copied, memory_order_relaxed);
            audio->input_primed = false;
        }
        data->chunk->offset = 0;
        data->chunk->stride = NS6_INPUT_FRAME_BYTES;
        data->chunk->size = frame_count * NS6_INPUT_FRAME_BYTES;
    }
    pw_stream_queue_buffer(endpoint->stream, pw_buffer);
}

static const struct pw_stream_events output_events = {
    .version = PW_VERSION_STREAM_EVENTS,
    .state_changed = stream_state_changed,
    .process = output_process,
};

static const struct pw_stream_events input_events = {
    .version = PW_VERSION_STREAM_EVENTS,
    .state_changed = stream_state_changed,
    .process = input_process,
};

static struct pw_stream *create_stream(ns6_audio_t *audio,
        ns6_pw_stream_t *endpoint, const char *name, const char *description,
        const char *media_class, const char *category, bool always_process,
        enum pw_direction direction, unsigned channels,
        const enum spa_audio_channel *positions,
        const struct pw_stream_events *events) {
    char channel_count[4];
    snprintf(channel_count, sizeof(channel_count), "%u", channels);
    struct pw_properties *props = pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio",
            PW_KEY_MEDIA_CATEGORY, category,
            PW_KEY_MEDIA_ROLE, "Music",
            PW_KEY_MEDIA_CLASS, media_class,
            PW_KEY_NODE_NAME, name,
            PW_KEY_NODE_DESCRIPTION, description,
            PW_KEY_AUDIO_CHANNELS, channel_count,
            PW_KEY_AUDIO_RATE, "44100",
            PW_KEY_NODE_ALWAYS_PROCESS, always_process ? "true" : "false",
            "audio.position", channels == 4 ? "[ FL FR RL RR ]" : "[ FL FR ]",
            NULL);
    if (!props) return NULL;
    endpoint->audio = audio;
    endpoint->label = description;
    endpoint->stream = pw_stream_new_simple(
            pw_thread_loop_get_loop(audio->loop), name, props, events, endpoint);
    if (!endpoint->stream) return NULL;

    uint8_t pod_buffer[1024];
    struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(pod_buffer,
                                                          sizeof(pod_buffer));
    const struct spa_pod *params[1];
    struct spa_audio_info_raw format = SPA_AUDIO_INFO_RAW_INIT(
            .format = SPA_AUDIO_FORMAT_S24_LE,
            .rate = AUDIO_RATE,
            .channels = channels);
    for (unsigned i = 0; i < channels; ++i)
        format.position[i] = positions[i];
    params[0] = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat,
                                           &format);
    if (!params[0] || pw_stream_connect(endpoint->stream, direction, PW_ID_ANY,
            PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS,
            params, 1) < 0) {
        fprintf(stderr, "ns6: could not create PipeWire %s\n", description);
        pw_stream_destroy(endpoint->stream);
        endpoint->stream = NULL;
        return NULL;
    }
    return endpoint->stream;
}

static void destroy_stream(ns6_pw_stream_t *endpoint) {
    if (!endpoint->stream) return;
    pw_stream_disconnect(endpoint->stream);
    pw_stream_destroy(endpoint->stream);
    endpoint->stream = NULL;
}

static void audio_destroy(ns6_audio_t *audio) {
    if (!audio) return;
    if (audio->loop) {
        if (audio->loop_started) pw_thread_loop_lock(audio->loop);
        destroy_stream(&audio->input);
        destroy_stream(&audio->output);
        if (audio->loop_started) {
            pw_thread_loop_unlock(audio->loop);
            pw_thread_loop_stop(audio->loop);
        }
        pw_thread_loop_destroy(audio->loop);
    }
    pw_deinit();
    free(audio);
}

int ns6_audio_start(ns6_device_t *dev) {
    ns6_audio_t *audio = calloc(1, sizeof(*audio));
    if (!audio) return -1;
    if (!ns6_audio_ring_init(&audio->output_ring, 12) ||
        !ns6_audio_ring_init(&audio->input_ring, 6)) {
        free(audio);
        return -1;
    }

    atomic_init(&audio->output_overruns, 0);
    atomic_init(&audio->output_underruns, 0);
    atomic_init(&audio->input_drops, 0);
    atomic_init(&audio->input_underruns, 0);

    int argc = 1;
    char arg0[] = "ns6d";
    char *argv_storage[] = { arg0, NULL };
    char **argv = argv_storage;
    pw_init(&argc, &argv);
    audio->loop = pw_thread_loop_new("ns6-audio", NULL);
    if (!audio->loop || pw_thread_loop_start(audio->loop) < 0) {
        fprintf(stderr, "ns6: could not start PipeWire client loop\n");
        audio_destroy(audio);
        return -1;
    }
    audio->loop_started = true;
    static const enum spa_audio_channel output_positions[] = {
        SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR,
        SPA_AUDIO_CHANNEL_RL, SPA_AUDIO_CHANNEL_RR
    };
    static const enum spa_audio_channel input_positions[] = {
        SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR
    };
    pw_thread_loop_lock(audio->loop);
    struct pw_stream *output = create_stream(audio, &audio->output,
            "numark_ns6_output", "Numark USB Audio Device", "Audio/Sink",
            "Playback", false, PW_DIRECTION_INPUT, NS6_OUTPUT_CHANNELS,
            output_positions, &output_events);
    struct pw_stream *input = create_stream(audio, &audio->input,
            "numark_ns6_input", "Numark USB Audio Device Input", "Audio/Source",
            "Capture", true, PW_DIRECTION_OUTPUT, NS6_INPUT_CHANNELS,
            input_positions, &input_events);
    pw_thread_loop_unlock(audio->loop);
    if (!output || !input) {
        fprintf(stderr, "ns6: PipeWire audio endpoints unavailable; MIDI remains active\n");
        audio_destroy(audio);
        return -1;
    }
    dev->audio_priv = audio;
    printf("ns6: PipeWire endpoints active (Numark USB Audio Device: 4 channels, "
           "Numark USB Audio Device Input: stereo, 44.1 kHz S24_3LE)\n");
    return dev->transport_lost ? -1 : 0;
}

size_t ns6_audio_output_queued(ns6_device_t *dev) {
    ns6_audio_t *audio = dev ? dev->audio_priv : NULL;
    if (!audio) return 0;
    size_t read = atomic_load_explicit(&audio->output_ring.read_index,
                                       memory_order_relaxed);
    size_t write = atomic_load_explicit(&audio->output_ring.write_index,
                                        memory_order_acquire);
    return write - read;
}

static int32_t unpack_pcm24(const uint8_t *sample) {
    int32_t value = (int32_t)sample[0] | ((int32_t)sample[1] << 8) |
                    ((int32_t)sample[2] << 16);
    return (value & 0x800000) ? value | (int32_t)0xff000000 : value;
}

static void pack_pcm24(uint8_t *sample, int32_t value) {
    if (value > 8388607) value = 8388607;
    if (value < -8388608) value = -8388608;
    sample[0] = (uint8_t)value;
    sample[1] = (uint8_t)(value >> 8);
    sample[2] = (uint8_t)(value >> 16);
}

void ns6_audio_output_read(ns6_device_t *dev, uint8_t *frames, size_t count,
                           double source_frames_per_output_frame) {
    ns6_audio_t *audio = dev ? dev->audio_priv : NULL;
    if (!audio) {
        memset(frames, 0, count * 12);
        return;
    }
    if (source_frames_per_output_frame < 0.995 ||
        source_frames_per_output_frame > 1.005)
        source_frames_per_output_frame = 1.0;

    ns6_audio_ring_t *ring = &audio->output_ring;
    size_t read = atomic_load_explicit(&ring->read_index, memory_order_relaxed);
    size_t write = atomic_load_explicit(&ring->write_index, memory_order_acquire);
    size_t available = write - read;
    double phase = audio->output_phase;
    size_t produced = 0;
    for (; produced < count; ++produced) {
        size_t offset = (size_t)phase;
        if (offset >= available) break;
        double fraction = phase - (double)offset;
        size_t first_pos = (read + offset) % NS6_AUDIO_RING_CAPACITY;
        size_t second_offset = offset + (fraction > 0.0 ? 1u : 0u);
        if (second_offset >= available) {
            if (fraction > 0.0) break;
            second_offset = offset;
        }
        size_t second_pos = (read + second_offset) % NS6_AUDIO_RING_CAPACITY;
        const uint8_t *first = ring->frames[first_pos];
        const uint8_t *second = ring->frames[second_pos];
        for (unsigned channel = 0; channel < 4; ++channel) {
            int32_t a = unpack_pcm24(first + channel * 3);
            int32_t b = unpack_pcm24(second + channel * 3);
            int32_t sample = (int32_t)((double)a + ((double)b - a) * fraction);
            pack_pcm24(frames + produced * 12 + channel * 3, sample);
        }
        phase += source_frames_per_output_frame;
    }

    size_t consumed = (size_t)phase;
    if (consumed > available) {
        consumed = available;
        phase = 0.0;
    } else {
        phase -= (double)consumed;
    }
    audio->output_phase = phase;
    atomic_store_explicit(&ring->read_index, read + consumed, memory_order_release);
    if (produced < count) {
        memset(frames + produced * 12, 0, (count - produced) * 12);
        atomic_fetch_add_explicit(&audio->output_underruns, count - produced,
                                  memory_order_relaxed);
    }
}

void ns6_audio_capture_usb_block(ns6_device_t *dev, const uint8_t *source,
                                 size_t source_bytes) {
    ns6_audio_t *audio = dev ? dev->audio_priv : NULL;
    if (!audio || source_bytes == 0) return;
    uint8_t frames[(NS6_CAPTURE_READ_SIZE / 64 + 1) * 6];
    size_t decoded = ns6_capture_decode_stream(&audio->capture_decoder, source,
            source_bytes, frames, sizeof(frames) / 6);
    size_t written = ns6_audio_ring_write(&audio->input_ring, frames, decoded);
    if (written < decoded)
        atomic_fetch_add_explicit(&audio->input_drops, decoded - written,
                                  memory_order_relaxed);
}

void ns6_audio_stop(ns6_device_t *dev) {
    ns6_audio_t *audio = dev ? dev->audio_priv : NULL;
    if (!audio) return;
    fprintf(stderr, "ns6: PipeWire totals: output overrun frames=%llu, USB-output underrun frames=%llu, input-ring dropped frames=%llu, input underrun frames=%llu\n",
            (unsigned long long)atomic_load(&audio->output_overruns),
            (unsigned long long)atomic_load(&audio->output_underruns),
            (unsigned long long)atomic_load(&audio->input_drops),
            (unsigned long long)atomic_load(&audio->input_underruns));
    dev->audio_priv = NULL;
    audio_destroy(audio);
}
