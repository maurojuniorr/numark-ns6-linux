# ns6d — Numark NS6 Userspace Driver

Driver userspace para a Numark NS6 no Linux. O daemon recebe e envia o MIDI
USB pela ALSA Sequencer e publica áudio como dois nós PipeWire próprios:
**Numark USB Audio Device** (4 canais) e **Numark USB Audio Device Input** (2 canais). O áudio
passa diretamente pelas filas do daemon; `snd-aloop` não é usado.

## Fluxos de áudio e MIDI

- **Saída:** PipeWire entrega PCM intercalado S24_3LE, 44,1 kHz, em quatro
  canais. Os canais 1–2 vão ao Main e 3–4 aos fones. O PipeWire converte o
  formato/taxa dos aplicativos para o formato fixo da NS6 no grafo compartilhado.
- **Entrada:** o endpoint USB `0x86` transporta áudio estéreo empacotado. O
  daemon decodifica os blocos de 64 bytes, converte para S24_3LE e publica uma
  source estéreo a 44,1 kHz. Aplicativos PipeWire podem compartilhar essa source.
- **Relógio USB:** o endpoint isócrono `0x81` fornece feedback de 42–46
  frames/ms. O controlador ajusta a cadência de saída; sem feedback válido por
  2 s, usa o ritmo nominal de 44,1 kHz.
- **Buffers:** filas SPSC isolam as callbacks USB e PipeWire de I/O bloqueante.
  A correção fracionária limitada acompanha a ocupação da fila de saída para
  compensar a deriva entre o relógio do host e o da NS6.
- **MIDI:** relatórios de até 42 bytes no endpoint Bulk `0x83` são remontados
  como fluxo MIDI contínuo antes de expor as mensagens pelo cliente ALSA
  Sequencer **Numark NS6 MIDI Bridge**; saída pelo Bulk `0x04`.

## Dependências

### Fedora

```bash
sudo dnf install libusb1-devel alsa-lib-devel pipewire-devel cmake gcc pkg-config
```

### Debian/Ubuntu

```bash
sudo apt install libusb-1.0-0-dev libasound2-dev libpipewire-0.3-dev cmake gcc pkg-config
```

## Build e instalação

```bash
cmake -B build
cmake --build build
sudo cmake --install build
sudo udevadm control --reload-rules
sudo udevadm trigger
```

A instalação remove o serviço systemd antigo e os aliases ALSA do loopback
instalados por versões anteriores. Ela tenta descarregar `snd-aloop` se nenhum
outro processo o estiver usando. O serviço atual é um serviço **do usuário**;
ative-o na sessão gráfica que executa PipeWire:

```bash
systemctl --user daemon-reload
systemctl --user enable --now ns6d
```

As regras udev dão acesso à NS6 e ao ALSA Sequencer ao usuário da sessão ativa.
Se o dispositivo já estava conectado durante a instalação, desconecte e conecte
novamente o USB para aplicar a ACL. Em uma perda de USB, o daemon reabre o
transporte e repete a inicialização sem recriar o cliente/porta MIDI ALSA ou os
nós PipeWire. Em duas reconexões físicas de teste, áudio e MIDI voltaram sem
fechar o Mixxx. A instalação de uma nova versão do daemon ainda recria a porta
MIDI uma vez.

No Mixxx, selecione **Numark USB Audio Device** e associe os canais 1–2 ao Main e
3–4 aos fones. Para entrada, selecione **Numark USB Audio Device Input**. O áudio do sistema
também pode usar a saída PipeWire da NS6.

Confira os nós com `wpctl status`; MIDI com `aconnect -l` e logs com:

```bash
journalctl --user -u ns6d -f
```

## Estado

- [x] Identificação USB e sequência de inicialização da NS6
- [x] MIDI packed ↔ ALSA Sequencer, quatro decks e jog 14-bit
- [x] Nó PipeWire de saída com 4 canais e source de entrada estéreo
- [x] Captura USB `0x86` sob demanda: leituras iniciam com cliente PipeWire e pausam sem clientes, mantendo o MIDI ativo
- [x] Feedback do clock USB e correção fracionária da fila de saída
- [x] Reconexão USB sem recriar a porta MIDI ALSA nem os nós PipeWire
- [x] Testar duas reconexões USB com Mixxx aberto; áudio e MIDI retornaram sem reiniciar o Mixxx
- [ ] Investigar um aviso de FIFO ALSA cheio observado durante a recuperação; não houve falha observada
- [ ] Validar estabilidade prolongada, reprodução e captura simultâneas, latência e contadores de underrun/drop

O caminho Linux é um daemon userspace, não um módulo de kernel. A validação de
áudio simultâneo e da latência real depende do hardware e da sessão PipeWire
usados.

O resultado do teste físico de reconexão e a abordagem a investigar no macOS
estão registrados em [`docs/usb-reconnection.md`](../docs/usb-reconnection.md).
