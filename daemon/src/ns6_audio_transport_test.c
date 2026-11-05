#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "ns6_audio_transport.h"

static void test_ring_preserves_frame_order_and_zero_fills(void) {
    ns6_audio_ring_t ring;
    uint8_t input[4 * 12], output[6 * 12];
    for (unsigned i = 0; i < sizeof(input); ++i) input[i] = (uint8_t)i;
    assert(ns6_audio_ring_init(&ring, 12));
    assert(ns6_audio_ring_write(&ring, input, 4) == 4);
    assert(ns6_audio_ring_read(&ring, output, 6) == 4);
    assert(memcmp(input, output, sizeof(input)) == 0);
    for (unsigned i = sizeof(input); i < sizeof(output); ++i) assert(output[i] == 0);
}

static void test_ring_wraps_without_reordering(void) {
    ns6_audio_ring_t ring;
    uint8_t input[NS6_AUDIO_RING_CAPACITY + 6][12];
    uint8_t discard[NS6_AUDIO_RING_CAPACITY - 6][12];
    uint8_t output[12][12];
    for (unsigned i = 0; i < NS6_AUDIO_RING_CAPACITY + 6; ++i)
        memset(input[i], (int)(i & 0xff), 12);
    assert(ns6_audio_ring_init(&ring, 12));
    assert(ns6_audio_ring_write(&ring, &input[0][0], NS6_AUDIO_RING_CAPACITY) == NS6_AUDIO_RING_CAPACITY);
    assert(ns6_audio_ring_read(&ring, &discard[0][0], NS6_AUDIO_RING_CAPACITY - 6) == NS6_AUDIO_RING_CAPACITY - 6);
    assert(ns6_audio_ring_write(&ring, &input[NS6_AUDIO_RING_CAPACITY][0], 6) == 6);
    assert(ns6_audio_ring_read(&ring, &output[0][0], 12) == 12);
    for (unsigned i = 0; i < 12; ++i)
        for (unsigned byte = 0; byte < 12; ++byte)
            assert(output[i][byte] == ((NS6_AUDIO_RING_CAPACITY - 6 + i) & 0xff));
}

static void test_ring_preserves_packed_6byte_frames_across_writes(void) {
    ns6_audio_ring_t ring;
    uint8_t input[6 * 6], output[6 * 6];
    for (unsigned i = 0; i < sizeof(input); ++i) input[i] = (uint8_t)(i + 1);
    assert(ns6_audio_ring_init(&ring, 6));
    assert(ns6_audio_ring_write(&ring, input, 3) == 3);
    assert(ns6_audio_ring_write(&ring, input + 18, 3) == 3);
    assert(ns6_audio_ring_read(&ring, output, 6) == 6);
    assert(memcmp(input, output, sizeof(input)) == 0);
}

static void test_capture_decoder_emits_interleaved_s24_3le(void) {
    uint8_t packed[64] = {0};
    uint8_t output[6] = {0};
    const uint32_t left = 0x7fffff, right = 0x800000;
    for (unsigned bit = 0; bit < 24; ++bit) {
        packed[bit] = (uint8_t)((left >> (23 - bit)) & 1u);
        packed[32 + bit] = (uint8_t)((right >> (23 - bit)) & 1u);
    }
    assert(ns6_capture_decode_s24_3le(packed, sizeof(packed), output, 1) == 1);
    assert(output[0] == 0xff && output[1] == 0xff && output[2] == 0x7f);
    assert(output[3] == 0x00 && output[4] == 0x00 && output[5] == 0x80);
}

static void test_capture_stream_keeps_partial_usb_frame(void) {
    ns6_capture_decoder_t decoder = {0};
    uint8_t packed[128] = {0};
    uint8_t output[12] = {0};
    packed[0] = 1;
    packed[64] = 1;
    assert(ns6_capture_decode_stream(&decoder, packed, 32, output, 2) == 0);
    assert(ns6_capture_decode_stream(&decoder, packed + 32, 96, output, 2) == 2);
    assert(decoder.pending_bytes == 0);
    assert(output[2] == 0x80 && output[8] == 0x80);
}

int main(void) {
    test_ring_preserves_frame_order_and_zero_fills();
    test_ring_wraps_without_reordering();
    test_ring_preserves_packed_6byte_frames_across_writes();
    test_capture_decoder_emits_interleaved_s24_3le();
    test_capture_stream_keeps_partial_usb_frame();
    puts("NS6 audio transport tests passed");
    return 0;
}
