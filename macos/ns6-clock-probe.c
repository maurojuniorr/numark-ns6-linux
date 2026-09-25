/* NS6 transport baseline: activate device, stream silence, and log USB health. */
#include <libusb.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <mach/mach_time.h>

#define VID 0x15e4
#define PID 0x0079
#define RATE 44100
#define FRAME_BYTES 12
#define ISO_PKTS 32
/* User-space libusb callbacks may be delayed by macOS scheduling. Keep 32 ms
 * queued so a brief scheduling gap cannot starve the NS6 playback FIFO. */
#define ISO_URBS 8
#define ISO_MAX 156
#define WF_SIZE 10240
static atomic_bool running = true;
static unsigned long iso_ok, iso_err, wf_ok, wf_err, feedback_changed;
static unsigned long iso_packet_err, iso_packet_short;
static unsigned long long wf_bytes;
static uint64_t wf_first_time, wf_last_time;
static unsigned char feedback_first[3];
static unsigned char feedback_packet[64];
static int feedback_length;
static unsigned long feedback_values, feedback_5, feedback_6, feedback_other;
static bool tone_enabled;
static bool side_enabled;
static bool big_endian_samples;
static bool matrix_enabled;
static bool sample_slots_32;
static int stream_frame_bytes = FRAME_BYTES;
static uint64_t next_audio_frame;

struct iso_slot { uint64_t first_frame; };
static uint64_t last_iso_time, max_iso_gap;
static unsigned long iso_resubmit_err;

static void fill_audio(struct libusb_transfer *t)
{
    struct iso_slot *slot = t->user_data;
    if (!tone_enabled) {
        memset(t->buffer, 0, t->length);
        return;
    }
    for (int p = 0; p < t->num_iso_packets; ++p) {
        unsigned char *out = libusb_get_iso_packet_buffer(t, p);
        int frames = t->iso_packet_desc[p].length / stream_frame_bytes;
        for (int f = 0; f < frames; ++f) {
            uint64_t frame_index = slot->first_frame + (uint64_t)f;
            for (int ch = 0; ch < 4; ++ch) {
                double frequency = matrix_enabled ? 440.0 + 220.0 * ch : 440.0;
                double phase = 2.0 * M_PI * frequency * frame_index / RATE;
                int32_t sample = (int32_t)(sin(phase) * 0x180000);
                uint32_t packed = (uint32_t)sample;
                if (big_endian_samples) {
                    *out++ = (packed >> 16) & 0xff;
                    *out++ = (packed >> 8) & 0xff;
                    *out++ = packed & 0xff;
                } else {
                    *out++ = packed & 0xff;
                    *out++ = (packed >> 8) & 0xff;
                    *out++ = (packed >> 16) & 0xff;
                }
                if (sample_slots_32)
                    *out++ = (sample < 0) ? 0xff : 0x00;
            }
        }
    }
}

static void stop(int unused) { (void)unused; atomic_store(&running, false); }
static void iso_cb(struct libusb_transfer *t) {
    uint64_t now = mach_continuous_time();
    if (last_iso_time && now - last_iso_time > max_iso_gap)
        max_iso_gap = now - last_iso_time;
    last_iso_time = now;
    for (int p = 0; p < t->num_iso_packets; ++p) {
        if (t->iso_packet_desc[p].status != LIBUSB_TRANSFER_COMPLETED)
            ++iso_packet_err;
        if (t->iso_packet_desc[p].actual_length != t->iso_packet_desc[p].length)
            ++iso_packet_short;
    }
    if (t->status == LIBUSB_TRANSFER_COMPLETED) ++iso_ok; else ++iso_err;
    if (atomic_load(&running)) {
        struct iso_slot *slot = t->user_data;
        slot->first_frame = next_audio_frame;
        for (int p = 0; p < t->num_iso_packets; ++p)
            next_audio_frame += t->iso_packet_desc[p].length / stream_frame_bytes;
        fill_audio(t);
        if (libusb_submit_transfer(t) < 0) ++iso_resubmit_err;
    }
}
static void wf_cb(struct libusb_transfer *t) {
    if (t->status == LIBUSB_TRANSFER_COMPLETED) {
        uint64_t now = mach_continuous_time();
        if (!wf_first_time) wf_first_time = now;
        wf_last_time = now;
        ++wf_ok;
        wf_bytes += t->actual_length;
    } else ++wf_err;
    if (atomic_load(&running)) libusb_submit_transfer(t);
}
static void fb_cb(struct libusb_transfer *t) {
    if (t->status == LIBUSB_TRANSFER_COMPLETED && t->num_iso_packets &&
        t->iso_packet_desc[0].actual_length >= 3) {
        unsigned char *p = libusb_get_iso_packet_buffer(t, 0);
        if (!feedback_length) {
            feedback_length = t->iso_packet_desc[0].actual_length;
            if (feedback_length > (int)sizeof(feedback_packet))
                feedback_length = sizeof(feedback_packet);
            memcpy(feedback_packet, p, feedback_length);
        }
        if (!feedback_first[0]) memcpy(feedback_first, p, 3);
        else if (memcmp(feedback_first, p, 3)) ++feedback_changed;
        for (int i = 0; i < t->num_iso_packets; ++i) {
            if (t->iso_packet_desc[i].actual_length < 3) continue;
            p = libusb_get_iso_packet_buffer(t, i);
            ++feedback_values;
            if (p[0] == 5 && p[1] == 0 && p[2] == 0) ++feedback_5;
            else if (p[0] == 6 && p[1] == 0 && p[2] == 0) ++feedback_6;
            else ++feedback_other;
        }
    }
    if (atomic_load(&running)) libusb_submit_transfer(t);
}
static int control(libusb_device_handle *h, uint8_t req, uint8_t type,
                   uint16_t value, uint16_t index, unsigned char *data, uint16_t len) {
    int rc = libusb_control_transfer(h, type, req, value, index, data, len, 1000);
    if (rc < 0) fprintf(stderr, "control %02x: %s\\n", req, libusb_error_name(rc));
    return rc;
}
int main(int argc, char **argv) {
    int seconds = argc > 1 ? atoi(argv[1]) : 30, rc, i;
    side_enabled = true;
    for (int arg = 2; arg < argc; ++arg) {
        if (strcmp(argv[arg], "--tone") == 0) tone_enabled = true;
        if (strcmp(argv[arg], "--no-side") == 0) side_enabled = false;
        if (strcmp(argv[arg], "--be") == 0) big_endian_samples = true;
        if (strcmp(argv[arg], "--matrix") == 0) matrix_enabled = tone_enabled = true;
        if (strcmp(argv[arg], "--s24in32") == 0) sample_slots_32 = true;
    }
    if (sample_slots_32) stream_frame_bytes = 16;
    libusb_context *ctx; libusb_device_handle *h;
    if ((rc = libusb_init(&ctx)) < 0) return 1;
    if (!(h = libusb_open_device_with_vid_pid(ctx, VID, PID))) { fprintf(stderr, "NS6 not found\\n"); return 1; }
    if ((rc = libusb_claim_interface(h, 0)) < 0) {
        fprintf(stderr, "claim interface: %s\\n", libusb_error_name(rc)); return 1;
    }
    unsigned char buf[8] = {0}, rate[] = {0x44, 0xac, 0};
    rc = control(h, 86, 0xc0, 0, 0, buf, sizeof(buf));
    if (rc > 0) control(h, 86, 0xc0, 0, 0, buf, (uint16_t)rc);
    if ((rc = libusb_set_interface_alt_setting(h, 0, 1)) < 0 ||
        (rc = libusb_claim_interface(h, 1)) < 0 ||
        (rc = libusb_set_interface_alt_setting(h, 1, 1)) < 0) {
        fprintf(stderr, "set alt setting: %s\\n", libusb_error_name(rc)); return 1;
    }
    libusb_clear_halt(h, 0x86); libusb_clear_halt(h, 0x04); libusb_clear_halt(h, 0x83);
    control(h, 73, 0xc0, 0, 0, buf, 1);
    control(h, 1, 0x22, 0x0100, 134, rate, 3); control(h, 1, 0x22, 0x0100, 2, rate, 3);
    control(h, 73, 0x40, 0x0032, 0, NULL, 0);
    static const unsigned char sysex[] = {0xf0,0x00,0x01,0x3f,0x00,0x79,0x51,0x00,0x10,0x49,0x01,0x08,0x01,0x01,0x08,0x04,0x0c,0x0d,0x01,0x0a,0x0a,0x05,0x06,0x05,0x0d,0x07,0x0e,0x08,0x07,0x0d,0xf7};
    unsigned char midi[42]; int written = 0;
    memset(midi, 0xfd, sizeof(midi)); memcpy(midi, sysex, sizeof(sysex)); midi[41] = 0;
    rc = libusb_bulk_transfer(h, 0x04, midi, sizeof(midi), &written, 1000);
    if (rc < 0) fprintf(stderr, "SysEx: %s\\n", libusb_error_name(rc));
    struct libusb_transfer *all[ISO_URBS + 4] = {0};
    struct iso_slot slots[ISO_URBS] = {0};
    unsigned char *mem[ISO_URBS + 4] = {0}; unsigned acc = 0;
    for (int n = 0; n < ISO_URBS; ++n) {
        all[n] = libusb_alloc_transfer(ISO_PKTS); mem[n] = calloc(ISO_PKTS, ISO_MAX);
        libusb_fill_iso_transfer(all[n], h, 0x02, mem[n], ISO_PKTS * ISO_MAX, ISO_PKTS, iso_cb, &slots[n], 1000);
        int bytes = 0;
        for (int p = 0; p < ISO_PKTS; ++p) { acc += RATE; int frames = acc / 8000; acc %= 8000; all[n]->iso_packet_desc[p].length = frames * stream_frame_bytes; bytes += all[n]->iso_packet_desc[p].length; }
        all[n]->length = bytes;
        slots[n].first_frame = next_audio_frame;
        next_audio_frame += bytes / stream_frame_bytes;
        fill_audio(all[n]);
        if ((rc = libusb_submit_transfer(all[n])) < 0) fprintf(stderr, "ISO submit: %s\\n", libusb_error_name(rc));
    }
    int total = ISO_URBS;
    if (side_enabled) {
        for (int n = 0; n < 3; ++n) { int i = ISO_URBS + n; all[i] = libusb_alloc_transfer(0); mem[i] = calloc(1, WF_SIZE); libusb_fill_bulk_transfer(all[i], h, 0x86, mem[i], WF_SIZE, wf_cb, NULL, 1000); libusb_submit_transfer(all[i]); }
        int i = ISO_URBS + 3; all[i] = libusb_alloc_transfer(16); mem[i] = calloc(16, 64); libusb_fill_iso_transfer(all[i], h, 0x81, mem[i], 1024, 16, fb_cb, NULL, 1000); for (int p=0;p<16;++p) all[i]->iso_packet_desc[p].length=64; libusb_submit_transfer(all[i]);
        total += 4;
    }
    signal(SIGINT, stop); time_t end = time(NULL) + seconds;
    while (atomic_load(&running) && time(NULL) < end) { struct timeval tv={1,0}; libusb_handle_events_timeout_completed(ctx,&tv,NULL); }
    atomic_store(&running, false);
    for (i=0;i<total;++i) libusb_cancel_transfer(all[i]);
    /* Cancellation completes asynchronously. Drain completion callbacks before
     * releasing their buffers; callbacks stop resubmitting once running is false. */
    for (int pass = 0; pass < 8; ++pass) {
        struct timeval tv = {1, 0};
        libusb_handle_events_timeout_completed(ctx, &tv, NULL);
    }
    for (i=0;i<total;++i) { libusb_free_transfer(all[i]); free(mem[i]); }
    mach_timebase_info_data_t timebase; mach_timebase_info(&timebase);
    double max_gap_ms = (double)max_iso_gap * timebase.numer / timebase.denom / 1e6;
    double wf_seconds = (double)(wf_last_time - wf_first_time) * timebase.numer / timebase.denom / 1e9;
    printf("iso ok=%lu err=%lu packet_err=%lu short=%lu resubmit_err=%lu max_gap=%.3fms waveform ok=%lu err=%lu feedback=%02x%02x%02x changes=%lu\\n",iso_ok,iso_err,iso_packet_err,iso_packet_short,iso_resubmit_err,max_gap_ms,wf_ok,wf_err,feedback_first[0],feedback_first[1],feedback_first[2],feedback_changed);
    printf("first feedback packet (%d bytes):", feedback_length);
    for (i = 0; i < feedback_length; ++i) printf(" %02x", feedback_packet[i]);
    printf("\\n");
    printf("feedback values=%lu: 5-frame=%lu 6-frame=%lu other=%lu\\n",
           feedback_values, feedback_5, feedback_6, feedback_other);
    if (wf_seconds > 0)
        printf("waveform bytes=%llu rate=%.2f bytes/s packets/s=%.4f\\n", wf_bytes,
               wf_bytes / wf_seconds, wf_ok / wf_seconds);
    libusb_release_interface(h,1); libusb_release_interface(h,0); libusb_close(h); libusb_exit(ctx); return 0;
}
