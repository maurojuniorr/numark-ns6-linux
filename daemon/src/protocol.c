#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "ns6.h"

/* ------------------------------------------------------------------ */
/* Sequência completa de init — Clone Exato do macOS                    */
/* ------------------------------------------------------------------ */

int ns6_init(ns6_device_t *dev)
{
    libusb_device_handle *usb = dev->usb;
    int r;
    uint8_t rate_pkt[] = { 0x44, 0xAC, 0x00 }; // 44100 Hz LE
    uint8_t dummy_buf[4];

    printf("ns6: Iniciando Boot Sequence (Clone macOS)...\n");
    
    /* Fase 1: Reset Limpo (Sem o reset físico que derruba o Linux!) */
    // libusb_reset_device(usb);  <--- COMENTADO PARA EVITAR O CRASH SILENCIOSO
    usleep(300000); 
    libusb_detach_kernel_driver(usb, 0);
    libusb_detach_kernel_driver(usb, 1);

    /* Fase 2: Configuração e Interfaces */
    r = libusb_claim_interface(usb, 0);
    if (r < 0) {
        fprintf(stderr, "ns6: Erro ao pedir Interface 0: %s\n", libusb_strerror(r));
        return r;
    }
    libusb_set_interface_alt_setting(usb, 0, 1);
    
    r = libusb_claim_interface(usb, 1);
    if (r == 0) {
        libusb_set_interface_alt_setting(usb, 1, 1);
    } else {
        fprintf(stderr, "ns6: Aviso ao pedir Interface 1: %s\n", libusb_strerror(r));
    }

    /* Limpa HALT (Como o macOS faz) */
    libusb_clear_halt(usb, 0x02);
    libusb_clear_halt(usb, 0x83);
    libusb_clear_halt(usb, 0x86);

    /* Fase 3: Consultas Iniciais */
    // Vendor bReq=73 GET
    libusb_control_transfer(usb, 0xC0, 73, 0x0000, 0, dummy_buf, 4, 1000);
    // GET_CUR sample rate (wIdx=0)
    libusb_control_transfer(usb, 0xA2, 0x81, 0x0100, 0, dummy_buf, 3, 1000);

    /* Fase 4: O SEGREDO DO macOS - Set Sample Rate DUPLO */
    printf("ns6: Configurando Clock (Waveform + Audio)...\n");
    // 1. Configura Waveform (EP 0x86 / 134)
    r = libusb_control_transfer(usb, 0x22, 0x01, 0x0100, 134, rate_pkt, 3, 1000);
    // 2. Configura Audio OUT (EP 0x02 / 2)
    r = libusb_control_transfer(usb, 0x22, 0x01, 0x0100, 2, rate_pkt, 3, 1000);

    /* Fase 5: Ativação do Modo DJ */
    libusb_control_transfer(usb, 0xC0, 73, 0x0000, 0, dummy_buf, 4, 1000); // Re-check
    r = libusb_control_transfer(usb, 0x40, 73, 0x0032, 0, NULL, 0, 1000);  // SET Mode 50
    usleep(1000); // Pequena pausa (0.5ms no macOS)

    if (r >= 0) {
        printf("ns6: Hardware Boot OK!\n");
    } else {
        fprintf(stderr, "ns6: Erro no boot sequence.\n");
    }

    /* SysEx de Inicialização das Luzes */
    uint8_t sysex_init[] = {
        0xF0, 0x00, 0x01, 0x3F, 0x00, 0x79, 0x51, 0x00,
        0x10, 0x49, 0x01, 0x08, 0x01, 0x01, 0x08, 0x04,
        0x0C, 0x0D, 0x01, 0x0A, 0x0A, 0x05, 0x06, 0x05,
        0x0D, 0x07, 0x0E, 0x08, 0x07, 0x0D, 0xF7
    };
    uint8_t pkt[NS6_CTRL_PKT_SIZE];
    memset(pkt, NS6_IDLE_BYTE, NS6_CTRL_PKT_SIZE);
    memcpy(pkt, sysex_init, sizeof(sysex_init));
    pkt[NS6_CTRL_PKT_SIZE - 1] = NS6_PKT_TERMINATOR;

    int transferred = 0;
    libusb_bulk_transfer(usb, NS6_EP_CTRL_OUT, pkt, NS6_CTRL_PKT_SIZE, &transferred, 2000);

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