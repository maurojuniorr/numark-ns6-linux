#ifndef NS6_H
#define NS6_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <libusb-1.0/libusb.h>

/* ------------------------------------------------------------------ */
/* Device identification                                                */
/* ------------------------------------------------------------------ */
#define NS6_VENDOR_ID           0x15e4
#define NS6_PRODUCT_ID          0x0079

/* ------------------------------------------------------------------ */
/* USB Endpoints                                                        */
/* ------------------------------------------------------------------ */
#define NS6_EP_CTRL_IN          0x83   /* BULK IN  - MIDI control (device→host) */
#define NS6_EP_CTRL_OUT         0x04   /* BULK OUT - MIDI/LEDs   (host→device)  */
#define NS6_EP_FEEDBACK         0x81   /* ISO IN   - feedback/status              */
#define NS6_EP_CAPTURE          0x86   /* BULK IN  - stereo record mix            */

/* ------------------------------------------------------------------ */
/* Packet sizes                                                         */
/* ------------------------------------------------------------------ */
#define NS6_CTRL_PKT_SIZE       42     /* MIDI packed packet */
#define NS6_CAPTURE_READ_SIZE   10240  /* packed capture block */
#define NS6_IDLE_BYTE           0xFD   /* unused slot filler */
#define NS6_PKT_TERMINATOR      0x00   /* end of packet      */

/* ------------------------------------------------------------------ */
/* MIDI channels                                                        */
/* ------------------------------------------------------------------ */
#define NS6_CH_GLOBAL           0  /* crossfader, master, booth, layer */
#define NS6_CH_DECK1            1  /* deck 1 — layer OFF, lado esquerdo */
#define NS6_CH_DECK2            2  /* deck 2 — layer OFF, lado direito  */
#define NS6_CH_DECK3            3  /* deck 3 — layer ON,  lado esquerdo */
#define NS6_CH_DECK4            4  /* deck 4 — layer ON,  lado direito  */

/* ------------------------------------------------------------------ */
/* CC numbers — MSB; LSB = MSB + 0x20                                  */
/* ------------------------------------------------------------------ */
#define NS6_CC_LSB_OFFSET       0x20

/* Global (canal 0) */
#define NS6_CC_CROSSFADER       0x07
#define NS6_CC_MASTER_VOL       0x1B
#define NS6_CC_BOOTH_VOL        0x1C
#define NS6_CC_HP_VOL           0x19
#define NS6_CC_HP_MIX           0x1A

/* Por deck (canais 1-4) */
#define NS6_CC_VOLUME           0x08
#define NS6_CC_LOW              0x09
#define NS6_CC_MID              0x0A
#define NS6_CC_HIGH             0x0B
#define NS6_CC_GAIN             0x0C
#define NS6_CC_PITCH            0x13
#define NS6_CC_JOG              0x00   /* MSB — LSB = 0x20 */

/* Helpers 14-bit */
#define NS6_CC_IS_MSB(cc)       ((cc) < 0x20)
#define NS6_CC_IS_LSB(cc)       ((cc) >= 0x20 && (cc) < 0x40)
#define NS6_CC_MSB_OF(lsb)      ((lsb) - NS6_CC_LSB_OFFSET)
#define NS6_CC_LSB_OF(msb)      ((msb) + NS6_CC_LSB_OFFSET)
#define NS6_14BIT(msb, lsb)     (((msb) << 7) | (lsb))

/* ------------------------------------------------------------------ */
/* Note numbers                                                         */
/* ------------------------------------------------------------------ */
#define NS6_NOTE_CUE            0x10
#define NS6_NOTE_PLAY           0x11
#define NS6_NOTE_SYNC           0x12
#define NS6_NOTE_0x0E           0x0E
#define NS6_NOTE_JOG_TOUCH      0x2C
#define NS6_NOTE_LAYER_LEFT     0x04   /* canal 0 — deck 1↔3 */
#define NS6_NOTE_LAYER_RIGHT    0x05   /* canal 0 — deck 2↔4 */

/* Knob de navegação (canal 0, CC 0x44, relativo) */
#define NS6_CC_NAV_KNOB         0x44

/* Strip search (canal 1-4, CC 0x02 — simples 7-bit, não par 14-bit) */
#define NS6_CC_STRIP            0x02
#define NS6_CC_IS_SINGLE(cc)    ((cc) == NS6_CC_STRIP)

/* ------------------------------------------------------------------ */
/* Vendor/Class requests (init sequence)                                */
/* ------------------------------------------------------------------ */
#define NS6_BREQ_VENDOR_CAP      86
#define NS6_BREQ_VENDOR_MODE     73
#define NS6_VENDOR_MODE_GET      0x12
#define NS6_VENDOR_MODE_ACTIVATE 0x0032
#define NS6_BREQ_SET_SAMPLE_RATE 1
#define NS6_BREQ_GET_SAMPLE_RATE 129
#define NS6_SAMPLE_RATE_44100    { 0x44, 0xAC, 0x00 }

static const uint8_t NS6_SYSEX_INIT[] = {
    0xF0, 0x00, 0x01, 0x3F, 0x00, 0x79, 0x51, 0x00,
    0x10, 0x49, 0x01, 0x08, 0x01, 0x01, 0x08, 0x04,
    0x0C, 0x0D, 0x01, 0x0A, 0x0A, 0x05, 0x06, 0x05,
    0x0D, 0x07, 0x0E, 0x08, 0x07, 0x0D, 0xF7
};
#define NS6_SYSEX_INIT_LEN (sizeof(NS6_SYSEX_INIT))

/* ------------------------------------------------------------------ */
/* Estado do jog wheel                                                  */
/* ------------------------------------------------------------------ */
typedef struct {
    bool     touch;
    bool     msb_ready;
    uint8_t  msb;
    uint8_t  lsb;
    uint16_t position;
} ns6_jog_t;

/* ------------------------------------------------------------------ */
/* Estado completo da controladora                                      */
/* ------------------------------------------------------------------ */
typedef struct {
    ns6_jog_t jog[4];
    uint8_t   midi_message[3];
    uint8_t   midi_message_used;
    uint8_t   midi_message_expected;
    uint16_t  volume[4];
    uint16_t  eq_low[4];
    uint16_t  eq_mid[4];
    uint16_t  eq_high[4];
    uint16_t  gain[4];
    uint16_t  pitch[4];
    bool      play[4];
    bool      cue[4];
    bool      layer[2];
    uint16_t  crossfader;
    uint16_t  master_vol;
    uint16_t  booth_vol;
    uint16_t  hp_vol;
    uint16_t  hp_mix;
    uint8_t   pending_msb[64];
    bool      pending_msb_valid[64];
} ns6_state_t;

/* ------------------------------------------------------------------ */
/* Handle principal do daemon                                           */
/* ------------------------------------------------------------------ */
typedef struct ns6_device ns6_device_t;

struct ns6_device {
    libusb_device_handle *usb;
    libusb_context       *ctx;
    char                  firmware_version[32];
    ns6_state_t           state;

    void (*on_midi_in)(ns6_device_t *dev,
                       uint8_t status, uint8_t note, uint8_t value);

    bool  running;
    bool  transport_lost;
    atomic_bool clock_feedback_locked;
    void *priv;
    void *audio_priv;
};

typedef struct {
    bool output_active;
    bool input_active;
    size_t output_queued_frames;
    uint64_t output_overruns;
    uint64_t output_underruns;
    uint64_t input_drops;
    uint64_t input_underruns;
} ns6_audio_status_t;

/* ------------------------------------------------------------------ */
/* API pública                                                          */
/* ------------------------------------------------------------------ */
ns6_device_t *ns6_open(void);
int           ns6_init(ns6_device_t *dev);
int           ns6_run(ns6_device_t *dev);
int           ns6_reopen(ns6_device_t *dev);
void          ns6_close(ns6_device_t *dev);
void          ns6_release_controls(ns6_device_t *dev);

int  ns6_send_midi(ns6_device_t *dev,
                   uint8_t status, uint8_t note, uint8_t value);
int  ns6_send_ctrl_packet(ns6_device_t *dev,
                          const uint8_t packet[NS6_CTRL_PKT_SIZE]);
void ns6_midi_out_wake(void);
void ns6_midi_out_reset(void);
void *ns6_midi_out_worker(void *arg);
int ns6_audio_start(ns6_device_t *dev);
void ns6_audio_stop(ns6_device_t *dev);
void ns6_audio_reset(ns6_device_t *dev);
size_t ns6_audio_output_queued(ns6_device_t *dev);
void ns6_audio_get_status(ns6_device_t *dev, ns6_audio_status_t *status);
bool ns6_audio_capture_requested(ns6_device_t *dev);
void ns6_audio_output_read(ns6_device_t *dev, uint8_t *frames, size_t count,
                           double source_frames_per_output_frame);
void ns6_audio_capture_usb_block(ns6_device_t *dev, const uint8_t *source,
                                 size_t source_bytes);

void ns6_parse_packet(ns6_device_t *dev, const uint8_t *pkt, size_t length);

extern int ns6_debug_raw;

#endif /* NS6_H */
