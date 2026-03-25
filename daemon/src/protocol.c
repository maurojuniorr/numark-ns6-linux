#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "ns6.h"

/* ------------------------------------------------------------------ */
/* Helpers internos                                                     */
/* ------------------------------------------------------------------ */

static int ctrl_out(libusb_device_handle *usb,
                    uint8_t bmrt, uint8_t breq,
                    uint16_t wval, uint16_t widx,
                    uint8_t *data, uint16_t len)
{
    int r = libusb_control_transfer(usb, bmrt, breq, wval, widx,
                                    data, len, 1000);
    if (r < 0)
        fprintf(stderr, "ctrl_out bmrt=0x%02x breq=%d: %s\n",
                bmrt, breq, libusb_strerror(r));
    return r;
}

static int ctrl_in(libusb_device_handle *usb,
                   uint8_t bmrt, uint8_t breq,
                   uint16_t wval, uint16_t widx,
                   uint8_t *buf, uint16_t len)
{
    int r = libusb_control_transfer(usb, bmrt, breq, wval, widx,
                                    buf, len, 1000);
    if (r < 0)
        fprintf(stderr, "ctrl_in bmrt=0x%02x breq=%d: %s\n",
                bmrt, breq, libusb_strerror(r));
    return r;
}

/* ------------------------------------------------------------------ */
/* Passo 2 — Vendor capability query                                    */
/* ------------------------------------------------------------------ */
static int ns6_vendor_capability(libusb_device_handle *usb)
{
    uint8_t buf[8];
    const uint8_t expected[] = { 0x31, 0x01, 0x03, 0x02, 0x02 };

    /* Primeira leitura: descobre o tamanho */
    int r = ctrl_in(usb, 0xC0, NS6_BREQ_VENDOR_CAP, 0, 0, buf, 8);
    if (r < 0) return r;

    /* Segunda leitura: lê com tamanho correto */
    r = ctrl_in(usb, 0xC0, NS6_BREQ_VENDOR_CAP, 0, 0, buf, r);
    if (r < 0) return r;

    if (r < 5 || memcmp(buf, expected, 5) != 0) {
        fprintf(stderr, "ns6: capability mismatch\n");
        return -1;
    }

    printf("ns6: capability OK: %02x %02x %02x %02x %02x\n",
           buf[0], buf[1], buf[2], buf[3], buf[4]);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Passo 7/11 — Vendor mode query                                       */
/* ------------------------------------------------------------------ */
static int ns6_vendor_mode_get(libusb_device_handle *usb)
{
    uint8_t val = 0;
    int r = ctrl_in(usb, 0xC0, NS6_BREQ_VENDOR_MODE, 0, 0, &val, 1);
    if (r < 0) return r;

    /* 0x12 = idle, 0x32 = already initialized (e.g. by snd-usb-audio) */
    if (val != 0x12 && val != 0x32) {
        fprintf(stderr, "ns6: vendor mode unexpected: 0x%02x\n", val);
        return -1;
    }
    printf("ns6: vendor mode = 0x%02x OK\n", val);
    return 0;
}

/* ------------------------------------------------------------------ */
/* ------------------------------------------------------------------ */
/* Passo 12 — Ativar modo operacional                                   */
/* ------------------------------------------------------------------ */
static int ns6_activate(libusb_device_handle *usb)
{
    int r = ctrl_out(usb, 0x40, NS6_BREQ_VENDOR_MODE,
                     NS6_VENDOR_MODE_ACTIVATE, 0, NULL, 0);
    if (r < 0) return r;
    printf("ns6: device activated\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* Passo 13 — SysEx de identificação                                    */
/* ------------------------------------------------------------------ */
static int ns6_send_sysex(ns6_device_t *dev)
{
    uint8_t pkt[NS6_CTRL_PKT_SIZE];
    memset(pkt, NS6_IDLE_BYTE, NS6_CTRL_PKT_SIZE);
    pkt[NS6_CTRL_PKT_SIZE - 1] = NS6_PKT_TERMINATOR;

    /* Copia SysEx nos primeiros bytes */
    size_t len = NS6_SYSEX_INIT_LEN;
    if (len > NS6_CTRL_PKT_SIZE - 1)
        len = NS6_CTRL_PKT_SIZE - 1;
    memcpy(pkt, NS6_SYSEX_INIT, len);

    int transferred = 0;
    int r = libusb_bulk_transfer(dev->usb, NS6_EP_CTRL_OUT,
                                  pkt, NS6_CTRL_PKT_SIZE,
                                  &transferred, 1000);
    if (r < 0) {
        fprintf(stderr, "ns6: sysex send failed: %s\n", libusb_strerror(r));
        return r;
    }
    printf("ns6: SysEx sent (%d bytes)\n", transferred);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Passo 14 — Burst de sincronização de estado inicial                  */
/* ------------------------------------------------------------------ */
static void build_init_pkt(uint8_t *pkt,
                            uint8_t s0, uint8_t n0, uint8_t v0,
                            uint8_t s1, uint8_t n1, uint8_t v1,
                            uint8_t s2, uint8_t n2, uint8_t v2)
{
    memset(pkt, NS6_IDLE_BYTE, NS6_CTRL_PKT_SIZE);
    pkt[0] = s0; pkt[1] = n0; pkt[2] = v0;
    pkt[3] = s1; pkt[4] = n1; pkt[5] = v1;
    pkt[6] = s2; pkt[7] = n2; pkt[8] = v2;
    pkt[NS6_CTRL_PKT_SIZE - 1] = NS6_PKT_TERMINATOR;
}

static int send_pkt(ns6_device_t *dev, const uint8_t *pkt)
{
    int transferred = 0;
    return libusb_bulk_transfer(dev->usb, NS6_EP_CTRL_OUT,
                                 (uint8_t *)pkt, NS6_CTRL_PKT_SIZE,
                                 &transferred, 1000);
}

static int ns6_send_init_state(ns6_device_t *dev)
{
    uint8_t pkt[NS6_CTRL_PKT_SIZE];

    /*
     * Envia estado inicial de todos os controles:
     * EQs e gains no máximo (0x7F), volumes no máximo,
     * crossfader no centro, botões desligados.
     * Reproduz o burst observado na captura.
     */

    /* Deck 1: gain, high, mid */
    build_init_pkt(pkt,
        0xB0, NS6_CC_LSB_OF(NS6_CC_GAIN),  0x7F,   /* Gain Dk1 LSB  */
        0xB0, NS6_CC_GAIN,                  0x7F,   /* Gain Dk1 MSB  */
        0xB0, NS6_CC_LSB_OF(NS6_CC_HIGH),  0x7F);  /* High EQ Dk1 LSB */
    send_pkt(dev, pkt);

    /* Deck 1: high, mid, low */
    build_init_pkt(pkt,
        0xB0, NS6_CC_HIGH,                  0x7F,
        0xB0, NS6_CC_LSB_OF(NS6_CC_MID),   0x7F,
        0xB0, NS6_CC_MID,                   0x7F);
    send_pkt(dev, pkt);

    /* Deck 1: low, volume */
    build_init_pkt(pkt,
        0xB0, NS6_CC_LSB_OF(NS6_CC_LOW),   0x7F,
        0xB0, NS6_CC_LOW,                   0x7F,
        0xB0, NS6_CC_LSB_OF(NS6_CC_VOLUME),0x7F);
    send_pkt(dev, pkt);

    /* Deck 1: volume; Deck 2: low, mid, high, gain */
    build_init_pkt(pkt,
        0xB0, NS6_CC_VOLUME,                0x7F,
        0xB0, NS6_CC_LSB_OF(NS6_CC_LOW)+5, 0x7F,   /* Low Dk2 LSB = 0x2E */
        0xB0, NS6_CC_LOW+5,                 0x7F);  /* Low Dk2 MSB = 0x0E */
    send_pkt(dev, pkt);

    /* Deck 2: volume, gain, high, mid */
    build_init_pkt(pkt,
        0xB0, NS6_CC_VOLUME+5,              0x7F,   /* Vol Dk2 MSB = 0x0D */
        0xB0, NS6_CC_LSB_OF(NS6_CC_VOLUME)+5, 0x7F,
        0xB0, NS6_CC_GAIN+5,                0x7F);
    send_pkt(dev, pkt);

    /* Crossfader no centro */
    build_init_pkt(pkt,
        0xB0, NS6_CC_CROSSFADER,            0x40,   /* MSB centro */
        0xB0, NS6_CC_LSB_OF(NS6_CC_CROSSFADER), 0x00,
        NS6_IDLE_BYTE, NS6_IDLE_BYTE, NS6_IDLE_BYTE);
    send_pkt(dev, pkt);

    /* Todos os botões desligados */
    build_init_pkt(pkt,
        0x80, NS6_NOTE_PLAY,     0x00,   /* Play Dk1 OFF */
        0x80, NS6_NOTE_CUE,      0x00,   /* Cue  Dk1 OFF */
        0x82, NS6_NOTE_PLAY,     0x00);  /* Play Dk2 OFF */
    send_pkt(dev, pkt);

    build_init_pkt(pkt,
        0x82, NS6_NOTE_CUE,      0x00,
        0x83, NS6_NOTE_PLAY,     0x00,
        0x83, NS6_NOTE_CUE,      0x00);
    send_pkt(dev, pkt);

    build_init_pkt(pkt,
        0x84, NS6_NOTE_PLAY,     0x00,
        0x84, NS6_NOTE_CUE,      0x00,
        NS6_IDLE_BYTE, NS6_IDLE_BYTE, NS6_IDLE_BYTE);
    send_pkt(dev, pkt);

    printf("ns6: init state burst sent\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* Sequência completa de init — 14 passos                               */
/* ------------------------------------------------------------------ */
int ns6_init(ns6_device_t *dev)
{
    libusb_device_handle *usb = dev->usb;
    int r;

    printf("ns6: starting init sequence...\n");

    /* Passo 1 — SET_CONFIGURATION 1
     * Se o kernel module de áudio estiver ativo, pula — ele já configurou */
    /* Passo 1 — Claim interface 0 apenas (interface 1 com snd-usb-audio) */
    r = libusb_claim_interface(usb, 0);
    if (r < 0) {
        fprintf(stderr, "ns6: claim interface 0: %s\n", libusb_strerror(r));
        return r;
    }

    /* Passo 2 — Vendor capability query */
    r = ns6_vendor_capability(usb);
    if (r < 0) return r;

    /* Passo 3 — SET_INTERFACE 0 alt=1 */
    r = libusb_set_interface_alt_setting(usb, 0, 1);
    if (r < 0) {
        fprintf(stderr, "ns6: set_interface 0,1: %s\n", libusb_strerror(r));
        return r;
    }
    printf("ns6: interface 0 alt=1 OK\n");

    /* Interface 1 precisa de alt=1 para o MIDI funcionar.
     * Claimamos temporariamente só para setar, depois liberamos
     * para o snd-usb-audio/snd-ns6-audio usar. */
    r = libusb_claim_interface(usb, 1);
    if (r == 0) {
        libusb_set_interface_alt_setting(usb, 1, 1);
        libusb_release_interface(usb, 1);
        printf("ns6: interface 1 alt=1 OK (released)\n");
    } else {
        printf("ns6: interface 1 already claimed by kernel driver (OK)\n");
    }

    /* Passo 4 — CLEAR_FEATURE nos endpoints MIDI */
    libusb_clear_halt(usb, NS6_EP_WAVEFORM);
    libusb_clear_halt(usb, NS6_EP_CTRL_OUT);
    libusb_clear_halt(usb, NS6_EP_CTRL_IN);
    printf("ns6: endpoints cleared\n");

    /* Passo 5 — Vendor mode query */
    r = ns6_vendor_mode_get(usb);
    if (r < 0) return r;

    printf("ns6: sample rate config skipped (snd-usb-audio)\n");

    /* Passo 6 — Vendor mode query (confirmação) */
    r = ns6_vendor_mode_get(usb);
    if (r < 0) return r;

    /* Passo 7 — Ativar modo operacional */
    r = ns6_activate(usb);
    if (r < 0) return r;

    /* Passo 8 — SysEx de identificação */
    r = ns6_send_sysex(dev);
    if (r < 0) return r;

    /* Passo 9 — Burst de sincronização de estado */
    r = ns6_send_init_state(dev);
    if (r < 0) return r;

    printf("ns6: init complete!\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* Parse de pacote MIDI de 42 bytes                                     */
/* ------------------------------------------------------------------ */
/* Liga/desliga debug de pacotes crus — útil pra mapear novos controles */
int ns6_debug_raw = 0;

void ns6_parse_packet(ns6_device_t *dev, const uint8_t *pkt)
{
    ns6_state_t *st = &dev->state;
    int i = 0;

    /* Debug: mostra pacote cru completo quando há dados não-idle */
    if (ns6_debug_raw) {
        bool has_data = false;
        for (int j = 0; j < NS6_CTRL_PKT_SIZE - 1; j++) {
            if (pkt[j] != NS6_IDLE_BYTE && pkt[j] != NS6_PKT_TERMINATOR) {
                has_data = true;
                break;
            }
        }
        if (has_data) {
            printf("RAW [");
            for (int j = 0; j < NS6_CTRL_PKT_SIZE - 1; j += 3) {
                uint8_t s = pkt[j], n = pkt[j+1], v = pkt[j+2];
                if (s == NS6_IDLE_BYTE) break;
                printf(" %02x %02x %02x |", s, n, v);
            }
            printf(" ]\n");
        }
    }

    while (i < NS6_CTRL_PKT_SIZE - 3) {
        uint8_t status = pkt[i];
        uint8_t note   = pkt[i + 1];
        uint8_t value  = pkt[i + 2];

        /* Fim do pacote */
        if (status == NS6_PKT_TERMINATOR)
            break;

        /* Slot vazio */
        if (status == NS6_IDLE_BYTE) {
            i += 3;
            continue;
        }

        /* Byte de status inválido — slot corrompido, pula */
        if (!(status & 0x80)) {
            i += 3;
            continue;
        }

        uint8_t msg = status & 0xF0;
        uint8_t ch  = status & 0x0F;
        int     dk  = ch - 1;   /* deck index 0..3 */

        switch (msg) {

        /* ---- Note On ---- */
        case 0x90:
            if (ch == NS6_CH_GLOBAL) {
                if (note == NS6_NOTE_LAYER_LEFT) {
                    st->layer[0] = !st->layer[0];
                    printf("ns6: Layer LEFT → deck %d\n",
                           st->layer[0] ? 3 : 1);
                } else if (note == NS6_NOTE_LAYER_RIGHT) {
                    st->layer[1] = !st->layer[1];
                    printf("ns6: Layer RIGHT → deck %d\n",
                           st->layer[1] ? 4 : 2);
                }
            } else if (dk >= 0 && dk < 4) {
                if (note == NS6_NOTE_PLAY) {
                    st->play[dk] = true;
                } else if (note == NS6_NOTE_CUE) {
                    st->cue[dk] = true;
                } else if (note == NS6_NOTE_JOG_TOUCH) {
                    st->jog[dk].touch = true;
                    st->jog[dk].msb_ready = false;
                }
            }
            if (dev->on_midi_in)
                dev->on_midi_in(dev, status, note, value);
            break;

        /* ---- Note Off ---- */
        case 0x80:
            if (dk >= 0 && dk < 4) {
                if (note == NS6_NOTE_PLAY)
                    st->play[dk] = false;
                else if (note == NS6_NOTE_CUE)
                    st->cue[dk] = false;
                else if (note == NS6_NOTE_JOG_TOUCH)
                    st->jog[dk].touch = false;
            }
            if (dev->on_midi_in)
                dev->on_midi_in(dev, status, note, value);
            break;

        /* ---- Control Change ---- */
        case 0xB0: {
            /* Jog position — canal 1..4, CC 0x00/0x20 */
            if (dk >= 0 && dk < 4 && note == NS6_CC_JOG) {
                if (value != NS6_IDLE_BYTE) {
                    /* MSB válido — acumula e aguarda LSB */
                    st->jog[dk].msb       = value;
                    st->jog[dk].msb_ready = true;
                }
                /* MSB == 0xfd: dado incompleto neste pacote, ignora */
                i += 3;
                continue;
            }
            if (dk >= 0 && dk < 4 &&
                note == NS6_CC_LSB_OF(NS6_CC_JOG)) {
                if (value == NS6_IDLE_BYTE) {
                    /* LSB incompleto neste pacote, ignora */
                    i += 3;
                    continue;
                }
                if (st->jog[dk].msb_ready || st->jog[dk].touch) {
                    st->jog[dk].lsb      = value;
                    st->jog[dk].position = NS6_14BIT(st->jog[dk].msb, value);
                    st->jog[dk].msb_ready = false;

                    if (dev->on_midi_in) {
                        dev->on_midi_in(dev, status,
                                        NS6_CC_JOG, st->jog[dk].msb);
                        dev->on_midi_in(dev, status,
                                        NS6_CC_LSB_OF(NS6_CC_JOG), value);
                    }
                }
                i += 3;
                continue;
            }

            /* CC 14-bit: MSB acumula silenciosamente, LSB finaliza e emite */
            if (NS6_CC_IS_MSB(note)) {
                /* Exceções: CCs abaixo de 0x20 que são valores simples */
                if (NS6_CC_IS_SINGLE(note)) {
                    if (dev->on_midi_in)
                        dev->on_midi_in(dev, status, note, value);
                    i += 3;
                    continue;
                }
                /* Só acumula — não emite nada ainda */
                if (note < 64) {
                    st->pending_msb[note] = value;
                    st->pending_msb_valid[note] = true;
                }
                i += 3;
                continue;
            }

            /* CC fora do range 14-bit (0x40+) — emite direto */
            if (note >= 0x40) {
                if (dev->on_midi_in)
                    dev->on_midi_in(dev, status, note, value);
                i += 3;
                continue;
            }

            if (NS6_CC_IS_LSB(note)) {
                uint8_t  msb_cc = NS6_CC_MSB_OF(note);
                uint16_t val14  = 0;
                bool     paired = false;

                if (msb_cc < 64 && st->pending_msb_valid[msb_cc]) {
                    val14  = NS6_14BIT(st->pending_msb[msb_cc], value);
                    paired = true;
                    st->pending_msb_valid[msb_cc] = false;
                } else {
                    /* LSB sem MSB prévio — emite como CC simples */
                    if (dev->on_midi_in)
                        dev->on_midi_in(dev, status, note, value);
                    i += 3;
                    continue;
                }

                /* Atualiza estado interno */
                if (ch == NS6_CH_GLOBAL) {
                    switch (msb_cc) {
                    case NS6_CC_CROSSFADER: st->crossfader = val14; break;
                    case NS6_CC_MASTER_VOL: st->master_vol = val14; break;
                    case NS6_CC_BOOTH_VOL:  st->booth_vol  = val14; break;
                    case NS6_CC_HP_VOL:     st->hp_vol     = val14; break;
                    case NS6_CC_HP_MIX:     st->hp_mix     = val14; break;
                    }
                } else if (dk >= 0 && dk < 4) {
                    switch (msb_cc) {
                    case NS6_CC_VOLUME: st->volume[dk]  = val14; break;
                    case NS6_CC_LOW:    st->eq_low[dk]  = val14; break;
                    case NS6_CC_MID:    st->eq_mid[dk]  = val14; break;
                    case NS6_CC_HIGH:   st->eq_high[dk] = val14; break;
                    case NS6_CC_GAIN:   st->gain[dk]    = val14; break;
                    case NS6_CC_PITCH:  st->pitch[dk]   = val14; break;
                    }
                }

                /*
                 * Emite sempre MSB primeiro, LSB depois.
                 * Mixxx e ALSA esperam essa ordem para CC 14-bit.
                 */
                if (paired && dev->on_midi_in) {
                    uint8_t msb_status = (status & 0xF0) | ch;
                    uint8_t msb_val    = st->pending_msb[msb_cc];
                    dev->on_midi_in(dev, msb_status, msb_cc, msb_val);
                    dev->on_midi_in(dev, msb_status, note,   value);
                }
            }
            break;
        }

        default:
            break;
        }

        i += 3;
    }
}
