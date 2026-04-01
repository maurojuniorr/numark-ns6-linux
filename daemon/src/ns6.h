#ifndef NS6_H
#define NS6_H

#include <stdint.h>
#include <stdbool.h>
#include <libusb-1.0/libusb.h>

/* ------------------------------------------------------------------ */
/* Device identification                                                */
/* ------------------------------------------------------------------ */
#define NS6_VENDOR_ID           0x15e4
#define NS6_PRODUCT_ID          0x0079

/* ------------------------------------------------------------------ */
/* USB Endpoints                                                        */
/* ------------------------------------------------------------------ */
#define NS6_EP_CTRL_IN          0x83
#define NS6_EP_CTRL_OUT         0x04
#define NS6_EP_WAVEFORM         0x86

#define NS6_EP_PLAY             0x02
#define NS6_EP_SYNC             0x81

/* ------------------------------------------------------------------ */
/* Packet sizes & USB URB definitions                                   */
/* ------------------------------------------------------------------ */
#define NS6_CTRL_PKT_SIZE       42
#define NS6_WAVEFORM_PKT_SIZE   10240
#define NS6_IDLE_BYTE           0xFD
#define NS6_PKT_TERMINATOR      0x00

#define NS6_PLAY_PKTS_PER_URB   32
#define NS6_PLAY_PKT_SIZE       156
#define NS6_FRAME_BYTES         12

#define NS6_SYNC_PKTS_PER_URB   16
#define NS6_SYNC_PKT_SIZE       64

/* ------------------------------------------------------------------ */
/* MIDI channels                                                        */
/* ------------------------------------------------------------------ */
#define NS6_CH_GLOBAL           0
#define NS6_CH_DECK1            1
#define NS6_CH_DECK2            2
#define NS6_CH_DECK3            3
#define NS6_CH_DECK4            4

/* ------------------------------------------------------------------ */
/* CC numbers                                                           */
/* ------------------------------------------------------------------ */
#define NS6_CC_LSB_OFFSET       0x20

#define NS6_CC_CROSSFADER       0x07
#define NS6_CC_MASTER_VOL       0x1B
#define NS6_CC_BOOTH_VOL        0x1C
#define NS6_CC_HP_VOL           0x19
#define NS6_CC_HP_MIX           0x1A

#define NS6_CC_VOLUME           0x08
#define NS6_CC_LOW              0x09
#define NS6_CC_MID              0x0A
#define NS6_CC_HIGH             0x0B
#define NS6_CC_GAIN             0x0C
#define NS6_CC_PITCH            0x13
#define NS6_CC_JOG              0x00

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
#define NS6_NOTE_LAYER_LEFT     0x04
#define NS6_NOTE_LAYER_RIGHT    0x05

#define NS6_CC_NAV_KNOB         0x44
#define NS6_CC_STRIP            0x02
#define NS6_CC_IS_SINGLE(cc)    ((cc) == NS6_CC_STRIP)

/* ------------------------------------------------------------------ */
/* Vendor/Class requests                                                */
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
/* Jog wheel state                                                      */
/* ------------------------------------------------------------------ */
typedef struct {
    bool     touch;
    bool     msb_ready;
    uint8_t  msb;
    uint8_t  lsb;
    uint16_t position;
} ns6_jog_t;

/* ------------------------------------------------------------------ */
/* Controller state                                                     */
/* ------------------------------------------------------------------ */
typedef struct {
    ns6_jog_t jog[4];
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
/* Device handle                                                        */
/* ------------------------------------------------------------------ */
typedef struct ns6_device ns6_device_t;

struct ns6_device {
    libusb_device_handle *usb;
    libusb_context       *ctx;
    ns6_state_t           state;

    void (*on_midi_in)(ns6_device_t *dev,
                       uint8_t status, uint8_t note, uint8_t value);

    bool  running;
    bool  no_audio;
    void *priv;
};

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */
ns6_device_t *ns6_open(void);
int           ns6_init(ns6_device_t *dev);
int           ns6_run(ns6_device_t *dev);
void          ns6_close(ns6_device_t *dev);

/* MIDI */
int  ns6_send_midi(ns6_device_t *dev, uint8_t status, uint8_t note, uint8_t value);
void ns6_midi_out_wake(void);
void *ns6_midi_out_worker(void *arg);

/* Protocol */
void ns6_parse_packet(ns6_device_t *dev, const uint8_t *pkt);
extern int ns6_debug_raw;

/* Audio */
int  ns6_audio_init(ns6_device_t *dev);
void ns6_audio_stop(void);
int  ns6_audio_pull_playback(uint8_t *iso_buf, int max_bytes);
void ns6_audio_pump(void);
int  ns6_audio_ring_fill(void);

#endif /* NS6_H */
