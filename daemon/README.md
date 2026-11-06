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
sudo dnf install libusb1-devel alsa-lib-devel pipewire-devel python3-gobject gtk3 cmake gcc pkg-config
```

### Debian/Ubuntu

```bash
sudo apt install libusb-1.0-0-dev libasound2-dev libpipewire-0.3-dev python3-gi gir1.2-gtk-3.0 cmake gcc pkg-config dpkg-dev
```

## Build e instalação

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
cd build && cpack -G DEB
sudo apt install ./numark-ns6-driver_*.deb
sudo udevadm trigger
systemctl --user enable --now ns6d.service
```

O pacote instala `ns6d`, o serviço systemd do usuário e a regra udev. Ele não
descarrega módulos nem remove serviços ou configuração ALSA de instalações
anteriores. Se houver restos de uma versão antiga, remova-os manualmente depois
de verificar que não estão em uso. O serviço é **do usuário** e deve ser ativado
na sessão gráfica que executa PipeWire:

```bash
systemctl --user enable --now ns6d
```

Para remover, primeiro pare/desative o serviço na sessão do usuário e depois
remova o pacote:

```bash
systemctl --user disable --now ns6d.service
sudo apt remove numark-ns6-driver
```

O `.deb` é produzido para a arquitetura da máquina que executa CPack; gere-o
separadamente em cada arquitetura alvo (por exemplo, `amd64` ou `arm64`).
Em atualizações, o pacote recarrega a definição systemd na sessão de usuário
ativa e reinicia `ns6d` se já estiver rodando.

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
ns6d --status
journalctl --user -u ns6d -f
```

O aplicativo **Numark NS6 Status** abre uma janela simples com conexão USB,
entradas, saídas, clock, versões do driver e do firmware. A versão do firmware
é lida pela consulta USB vendor `0x56` e decodificada como no driver macOS; se a
controladora responder em um formato não reconhecido, o app mostra “Unavailable”.
O app aparece no menu após instalar o pacote e atualiza o estado uma vez por segundo.

`ns6d --status` mostra conexão/reconexão USB, cliente MIDI, streams PipeWire,
estado do feedback do clock, ocupação da fila de saída e contadores acumulados
de underrun, overrun e perda de frames. O snapshot é atualizado a cada segundo
em `$XDG_RUNTIME_DIR/ns6d.status` e removido quando o daemon encerra normalmente.

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
