
---

# 🐧 Numark NS6 Kernel Driver for Linux (kmod-snd-ns6)

Este projeto apresenta um **driver de kernel nativo** para a controladora Numark NS6, escrito em C para o ecossistema ALSA. Diferente de mapeamentos simples, este módulo habilita a placa de som e a interface MIDI diretamente no núcleo do Linux, garantindo latência ultrabaixa e performance de nível profissional (Club Standard).

> [!CAUTION]
> **ESTADO ATUAL: ALFA (v0.98)**
> O driver é funcional e estável para performance, mas ainda está em fase de testes intensivos. O uso em ambientes de produção real é por sua conta e risco. **Requer compilação de módulos de kernel.**

---

## 🔥 Diferenciais desta Implementação (Kernel Level)

Por rodar diretamente no Kernel, este driver soluciona problemas históricos da NS6 no Linux:

* **Anti-Drift Clock Sync:** Implementação de um *Fractional Frame Accumulator* que estabiliza o fluxo em 44100Hz, eliminando estalos (clicks) de sincronia USB.
* **Dynamic Endpoint Discovery:** O driver escaneia e ativa automaticamente as sub-interfaces (*altsettings*) de áudio e MIDI, ignorando as limitações do driver genérico.
* **Professional MIDI Queue:** Um "robô de fila" dedicado processa rajadas de comandos MIDI, garantindo que nenhum LED (como VU meters ou efeitos) seja perdido durante a performance.
* **Zero-Freeze Protection:** Blindagem contra *Interrupt Storms* e travamentos de sistema via inicialização assíncrona (*Deferred Workqueue*).

---

## ✅ O que já está funcional (Alpha 0.98)

### 🔊 Motor de Áudio (ALSA PCM)
* **Playback & Capture:** 4 canais de saída e entrada em 24-bit (S24_3LE).
* **Estabilidade:** Fluxo contínuo de pacotes isócronos que mantém o relógio da placa "quente", evitando distorções ao carregar faixas.
* **Latência:** Resposta imediata ideal para Scratch e Beatmatch de precisão.

### 🎹 Interface MIDI e LEDs
* **Full Duplex MIDI:** Comunicação bidirecional estável entre Mixxx e NS6.
* **Sincronização de Estado:** LEDs de botões, faders e seletores de FX sincronizam automaticamente ao abrir o software.
* **Platters & Jogs:** Suporte total para a alta resolução (14-bit) dos discos.

### 🛠️ Inicialização Inteligente
* **Handshake Nativo:** O driver realiza o "aperto de mão" (Activate Vendor Mode) e o envio do SysEx de boot automaticamente ao plugar a controladora.

---

## 🛠️ Requisitos de Compilação

Você precisará dos headers do seu kernel atual. No Fedora:
```bash
sudo dnf install kernel-devel kernel-headers development-tools
```

---

## 📥 Instalação e Teste

1. **Clone o repositório e acesse o branch de desenvolvimento:**
   ```bash
   git clone https://github.com/seu-usuario/numark-ns6-linux.git
   cd numark-ns6-linux
   git checkout kmod-total
   ```

2. **Compile o módulo:**
   ```bash
   make clean
   make
   ```

3. **Instale e carregue o driver:**
   ```bash
   sudo make install
   sudo depmod -a
   sudo modprobe snd-ns6
   ```

4. **Verifique se a placa foi reconhecida:**
   ```bash
   aplay -l | grep NS6
   dmesg | grep NS6
   ```

---

## 🧪 Como ajudar nos testes (Modo Developer)

Se você encontrar comportamentos estranhos nos LEDs ou no áudio, inicie o Mixxx via terminal para capturar os logs:
```bash
mixxx --debugLevel 2 --developer
```
Relate qualquer erro de `URB submitted while active` ou falhas de sincronia nas **Issues** deste repositório.

---

## 🚧 Próximos Passos (Rumo ao Beta 1.0)
* [ ] **Audio Warm-up:** Adicionar pré-carga de silêncio para eliminar o ruído residual nos primeiros 200ms da primeira música.
* [ ] **Mapeamento Complementar:** Finalizar as funções de Shift no arquivo JS do Mixxx para aproveitar o novo motor MIDI.
* [ ] **DKMS Support:** Implementar suporte a DKMS para que o driver não precise ser recompilado manualmente a cada atualização de kernel.

---

## 🤝 Contribuições
Este é um projeto comunitário. Se você entende de C, protocolos USB ou ALSA, sinta-se à vontade para enviar um Pull Request.

**Desenvolvido com ❤️ por:**
@maurojuniorr | @omaurodj e **Gemini (IA)**

*Esposo, Pai, Programador, Professor, Pesquisador e DJ.*

---
