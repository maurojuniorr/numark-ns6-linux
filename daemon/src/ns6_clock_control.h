#ifndef NS6_CLOCK_CONTROL_H
#define NS6_CLOCK_CONTROL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
    NS6_CLOCK_FEEDBACK_WINDOW = 40000,
    NS6_CLOCK_FEEDBACK_QUEUE = 256,
    NS6_CLOCK_QUEUE_TARGET_FRAMES = 2048
};

typedef struct {
    uint8_t window[NS6_CLOCK_FEEDBACK_WINDOW];
    uint8_t pending[NS6_CLOCK_FEEDBACK_QUEUE];
    uint64_t sum;
    uint64_t rate_millihz;
    uint32_t count;
    uint32_t index;
    uint16_t pending_read;
    uint16_t pending_count;
    uint64_t last_feedback_ms;
    bool locked;
    uint64_t fallback_fraction;
} ns6_clock_control_t;

static inline void ns6_clock_control_init(ns6_clock_control_t *clock) {
    memset(clock, 0, sizeof(*clock));
    clock->rate_millihz = UINT64_C(44100000);
}

static inline bool ns6_clock_control_observe(ns6_clock_control_t *clock,
                                              uint8_t frames_per_ms,
                                              uint64_t now_ms) {
    if (!clock || frames_per_ms < 42 || frames_per_ms > 46) return false;
    if (clock->count == NS6_CLOCK_FEEDBACK_WINDOW)
        clock->sum -= clock->window[clock->index];
    else
        ++clock->count;
    clock->window[clock->index] = frames_per_ms;
    clock->sum += frames_per_ms;
    clock->index = (clock->index + 1) % NS6_CLOCK_FEEDBACK_WINDOW;
    if (clock->count == NS6_CLOCK_FEEDBACK_WINDOW)
        clock->rate_millihz = clock->sum * UINT64_C(1000000) /
                              NS6_CLOCK_FEEDBACK_WINDOW;

    if (clock->pending_count == NS6_CLOCK_FEEDBACK_QUEUE) {
        clock->pending_read = (clock->pending_read + 1) % NS6_CLOCK_FEEDBACK_QUEUE;
        --clock->pending_count;
    }
    uint16_t write = (uint16_t)((clock->pending_read + clock->pending_count) %
                                NS6_CLOCK_FEEDBACK_QUEUE);
    clock->pending[write] = frames_per_ms;
    ++clock->pending_count;
    clock->last_feedback_ms = now_ms;
    clock->locked = true;
    return true;
}

static inline void ns6_clock_control_reset(ns6_clock_control_t *clock) {
    ns6_clock_control_init(clock);
}

static inline void ns6_clock_control_next_millisecond(
        ns6_clock_control_t *clock, unsigned packet_frames[8]) {
    static const uint8_t patterns[5][8] = {
        {6, 5, 5, 5, 6, 5, 5, 5},
        {5, 5, 6, 5, 6, 5, 6, 5},
        {6, 5, 6, 5, 6, 5, 6, 5},
        {6, 6, 5, 6, 5, 6, 5, 6},
        {5, 6, 6, 6, 5, 6, 6, 6}
    };
    unsigned frames;
    if (clock->pending_count) {
        frames = clock->pending[clock->pending_read];
        clock->pending_read = (clock->pending_read + 1) % NS6_CLOCK_FEEDBACK_QUEUE;
        --clock->pending_count;
    } else {
        frames = (unsigned)(clock->rate_millihz / UINT64_C(1000000));
        clock->fallback_fraction += clock->rate_millihz % UINT64_C(1000000);
        if (clock->fallback_fraction >= UINT64_C(1000000)) {
            clock->fallback_fraction -= UINT64_C(1000000);
            ++frames;
        }
    }
    if (frames < 42) frames = 42;
    if (frames > 46) frames = 46;
    for (unsigned packet = 0; packet < 8; ++packet)
        packet_frames[packet] = patterns[frames - 42][packet];
}

static inline double ns6_clock_control_source_step(
        const ns6_clock_control_t *clock, size_t queued_frames) {
    uint64_t rate = clock && clock->rate_millihz ? clock->rate_millihz :
                    UINT64_C(44100000);
    if (rate < UINT64_C(44000000)) rate = UINT64_C(44000000);
    if (rate > UINT64_C(44100300)) rate = UINT64_C(44100300);
    double step = 44100000.0 / (double)rate;
    double trim = ((double)queued_frames - NS6_CLOCK_QUEUE_TARGET_FRAMES) * 0.000001;
    if (trim > 0.005) trim = 0.005;
    if (trim < -0.005) trim = -0.005;
    step *= 1.0 + trim;
    if (step > 1.005) step = 1.005;
    if (step < 0.995) step = 0.995;
    return step;
}

#endif
