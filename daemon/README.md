# ns6d — Numark NS6 Userspace Driver

Driver userspace para a Numark NS6 no Linux.
Expõe a controladora como porta ALSA MIDI virtual para uso com Mixxx.

## Protocolo

- **Controle:** MIDI packed em Bulk USB (EP 0x83/0x04), 42 bytes/pacote
- **Áudio:** PCM 24-bit 4 canais 44100Hz via ISO USB (EP 0x02/0x81)
- **Waveform:** stream 1-bit 2.5MB/s via EP 0x86
- **4 decks** via canais MIDI 1-4, layer gerenciado pela controladora

## Dependências

### Fedora
```bash
sudo dnf install libusb1-devel alsa-lib-devel cmake gcc pkg-config
```

### Debian/Ubuntu
```bash
sudo apt install libusb-1.0-0-dev libasound2-dev cmake gcc pkg-config
```

## Build

```bash
git clone <repo>
cd ns6d
cmake -B build
cmake --build build
```

## Instalação

```bash
# udev rules (acesso sem root)
sudo cp udev/99-ns6.rules /etc/udev/rules.d/
sudo udevadm control --reload
sudo udevadm trigger

# Binário
sudo cmake --install build
```

## Uso

```bash
# Conecta a NS6 via USB e roda o daemon
ns6d
```

O daemon cria uma porta ALSA MIDI chamada **"Numark NS6"**.
No Mixxx: `Preferences → Controllers → Numark NS6 MIDI`

Para conectar manualmente via aconnect:
```bash
aconnect -l                    # lista portas
aconnect "Numark NS6" mixxx    # conecta ao Mixxx
```

## Estrutura

```
src/
  ns6.h        → constantes do protocolo e structs
  protocol.c   → init sequence + parse de pacotes MIDI
  usb.c        → libusb: transfers bulk e ISO
  main.c       → ALSA virtual MIDI port + loop principal
udev/
  99-ns6.rules → permissões USB
```

## Status

- [x] Identificação do protocolo (engenharia reversa)
- [x] Sequência de init (14 passos)
- [x] Parse MIDI packed → MIDI padrão
- [x] 4 decks + layer system
- [x] CC 14-bit (faders, EQs, jog position)
- [x] Jog wheel (touch + no-touch modes)
- [x] ALSA virtual MIDI port
- [ ] ALSA PCM (áudio 24-bit 4ch) — próxima fase
- [ ] Waveform display pipe — próxima fase
- [ ] Kernel module (Fase 2)

## Fase 2 — Kernel Module

Depois de validar o daemon userspace, o mesmo protocolo
será portado para um kernel module (`snd-usb-ns6.ko`) com:
- Latência de áudio profissional
- Aparece como dispositivo nativo no ALSA
- DKMS para sobreviver updates do kernel
