#ifndef NS6_AUDIO_TRANSPORT_H
#define NS6_AUDIO_TRANSPORT_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { NS6_AUDIO_RING_CAPACITY = 8192, NS6_AUDIO_MAX_FRAME_BYTES = 12 };

typedef struct {
    _Alignas(64) atomic_size_t read_index;
    _Alignas(64) atomic_size_t write_index;
    size_t frame_bytes;
    uint8_t frames[NS6_AUDIO_RING_CAPACITY][NS6_AUDIO_MAX_FRAME_BYTES];
} ns6_audio_ring_t;

static inline int ns6_audio_ring_init(ns6_audio_ring_t *ring, size_t frame_bytes) {
    if (!ring || frame_bytes == 0 || frame_bytes > NS6_AUDIO_MAX_FRAME_BYTES)
        return 0;
    atomic_init(&ring->read_index, 0);
    atomic_init(&ring->write_index, 0);
    ring->frame_bytes = frame_bytes;
    return 1;
}

static inline size_t ns6_audio_ring_write(ns6_audio_ring_t *ring,
                                          const uint8_t *frames,
                                          size_t count) {
    size_t write = atomic_load_explicit(&ring->write_index, memory_order_relaxed);
    size_t read = atomic_load_explicit(&ring->read_index, memory_order_acquire);
    size_t available = NS6_AUDIO_RING_CAPACITY - (write - read);
    if (count > available) count = available;
    size_t first = count;
    size_t offset = write % NS6_AUDIO_RING_CAPACITY;
    if (first > NS6_AUDIO_RING_CAPACITY - offset)
        first = NS6_AUDIO_RING_CAPACITY - offset;
    uint8_t *storage = &ring->frames[0][0];
    memcpy(storage + offset * ring->frame_bytes, frames,
           first * ring->frame_bytes);
    memcpy(storage, frames + first * ring->frame_bytes,
           (count - first) * ring->frame_bytes);
    atomic_store_explicit(&ring->write_index, write + count, memory_order_release);
    return count;
}

static inline size_t ns6_audio_ring_read(ns6_audio_ring_t *ring,
                                         uint8_t *frames, size_t count) {
    size_t read = atomic_load_explicit(&ring->read_index, memory_order_relaxed);
    size_t write = atomic_load_explicit(&ring->write_index, memory_order_acquire);
    size_t available = write - read;
    size_t copied = count < available ? count : available;
    size_t first = copied;
    size_t offset = read % NS6_AUDIO_RING_CAPACITY;
    if (first > NS6_AUDIO_RING_CAPACITY - offset)
        first = NS6_AUDIO_RING_CAPACITY - offset;
    uint8_t *storage = &ring->frames[0][0];
    memcpy(frames, storage + offset * ring->frame_bytes,
           first * ring->frame_bytes);
    memcpy(frames + first * ring->frame_bytes, storage,
           (copied - first) * ring->frame_bytes);
    memset(frames + copied * ring->frame_bytes, 0,
           (count - copied) * ring->frame_bytes);
    atomic_store_explicit(&ring->read_index, read + copied, memory_order_release);
    return copied;
}

static inline size_t ns6_capture_decode_s24_3le(const uint8_t *source,
                                               size_t source_bytes,
                                               uint8_t *output,
                                               size_t output_capacity_frames) {
    enum { SOURCE_STRIDE = 64, OUTPUT_FRAME_BYTES = 6 };
    if (!source || !output || source_bytes == 0 || source_bytes % SOURCE_STRIDE)
        return 0;
    size_t frames = source_bytes / SOURCE_STRIDE;
    if (frames > output_capacity_frames) return 0;
    for (size_t frame = 0; frame < frames; ++frame) {
        const uint8_t *sample = source + frame * SOURCE_STRIDE;
        for (unsigned channel = 0; channel < 2; ++channel) {
            uint32_t value = 0;
            const uint8_t *lane = sample + channel * 32;
            for (unsigned bit = 0; bit < 24; ++bit)
                value = (value << 1) | (lane[bit] & 1u);
            size_t at = frame * OUTPUT_FRAME_BYTES + channel * 3;
            output[at] = (uint8_t)value;
            output[at + 1] = (uint8_t)(value >> 8);
            output[at + 2] = (uint8_t)(value >> 16);
        }
    }
    return frames;
}

typedef struct {
    uint8_t pending[64];
    size_t pending_bytes;
} ns6_capture_decoder_t;

static inline size_t ns6_capture_decode_stream(ns6_capture_decoder_t *decoder,
                                               const uint8_t *source,
                                               size_t source_bytes,
                                               uint8_t *output,
                                               size_t output_capacity_frames) {
    if (!decoder || !output || !source || source_bytes == 0 ||
        decoder->pending_bytes > sizeof(decoder->pending) ||
        (decoder->pending_bytes + source_bytes) / sizeof(decoder->pending) >
                output_capacity_frames)
        return 0;
    size_t produced = 0, consumed = 0;
    if (decoder->pending_bytes) {
        size_t needed = sizeof(decoder->pending) - decoder->pending_bytes;
        size_t copied = source_bytes < needed ? source_bytes : needed;
        memcpy(decoder->pending + decoder->pending_bytes, source, copied);
        decoder->pending_bytes += copied;
        consumed += copied;
        if (decoder->pending_bytes < sizeof(decoder->pending)) return 0;
        if (output_capacity_frames == 0) return 0;
        produced += ns6_capture_decode_s24_3le(decoder->pending,
                sizeof(decoder->pending), output, 1);
        decoder->pending_bytes = 0;
    }
    size_t remaining = source_bytes - consumed;
    size_t complete_bytes = remaining - remaining % sizeof(decoder->pending);
    size_t complete_frames = complete_bytes / sizeof(decoder->pending);
    if (complete_frames > output_capacity_frames - produced)
        complete_frames = output_capacity_frames - produced;
    size_t decoded_bytes = complete_frames * sizeof(decoder->pending);
    produced += ns6_capture_decode_s24_3le(source + consumed, decoded_bytes,
            output + produced * 6, complete_frames);
    consumed += decoded_bytes;
    decoder->pending_bytes = source_bytes - consumed;
    if (decoder->pending_bytes)
        memcpy(decoder->pending, source + consumed, decoder->pending_bytes);
    return produced;
}

#endif
