#ifndef NS6_ISO_SCHEDULE_H
#define NS6_ISO_SCHEDULE_H

#include <stdint.h>

enum {
    NS6_AUDIO_FRAME_BYTES = 12,
    NS6_ISO_PACKETS_PER_MS = 8,
    NS6_ISO_PACKETS_PER_TRANSFER = 32,
    NS6_ISO_MAX_FRAMES_PER_PACKET = 6
};

typedef struct {
    unsigned fractional_ms;
} ns6_iso_schedule_t;

static inline void ns6_iso_schedule_init(ns6_iso_schedule_t *schedule,
                                         unsigned first_ms) {
    schedule->fractional_ms = (first_ms % 10u) * 100u;
}

static inline unsigned ns6_iso_schedule_next_ms(ns6_iso_schedule_t *schedule) {
    schedule->fractional_ms += 100u;
    if (schedule->fractional_ms >= 1000u) {
        schedule->fractional_ms -= 1000u;
        return 45u;
    }
    return 44u;
}

static inline unsigned ns6_iso_packet_frames(unsigned frames_per_ms,
                                             unsigned packet) {
    static const uint8_t patterns[2][NS6_ISO_PACKETS_PER_MS] = {
        {6, 5, 6, 5, 6, 5, 6, 5},
        {6, 6, 5, 6, 5, 6, 5, 6}
    };
    if (packet >= NS6_ISO_PACKETS_PER_MS ||
        (frames_per_ms != 44u && frames_per_ms != 45u))
        return 0;
    return patterns[frames_per_ms - 44u][packet];
}

#endif
