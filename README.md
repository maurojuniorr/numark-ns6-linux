# 🐧 Numark NS6 Native Linux Kernel Driver (`snd-ns6`)

**Resurrecting a Legend: The first 99% functional native ALSA kernel driver for the Numark NS6.**

This project is a low-level engineering effort to bring the **Numark NS6** (VID 15e4, PID 0079) to the Linux ecosystem. Unlike simple MIDI mappings, this is a **C-coded Kernel Module** that interfaces directly with the ALSA architecture, aiming for professional-grade stability and ultra-low latency.

> [!IMPORTANT]
> **CURRENT STATUS: ALPHA (v0.99) — "The Final Mile"**
> The driver is fully operational for both MIDI and Audio. Current development is focused on fine-tuning the adaptive rate controller to eliminate the final 1% of audio micro-glitches (clicks) caused by hardware crystal drift.

---

## 👨‍🏫 About the Author
**Mauro Junior**
* **Computer Science Professor** at **IFAM** (Federal Institute of Amazonas, Brazil).
* **Professional DJ** & Linux Enthusiast.
* **Programmer** dedicated to hardware reverse engineering and kernel development.

---

## 🔥 The Engineering Challenge (The "Final Boss")

The NS6 was considered "Windows/Mac only" for 15 years due to its proprietary Ploytec chipset. This driver solves the core architectural hurdles that prevented official support:

### 1. Anti-Drift Sync Engine
The NS6 lacks a functional hardware feedback endpoint (EP 0x81 is a dummy returning constant `0xAAAAAA`). This causes a clock mismatch between the host and the hardware's internal crystal (~44101.5 Hz).
* **The Solution:** We implemented a **Fractional Frame Accumulator** coupled with an **Adaptive PID Controller**.
* The driver monitors the ALSA buffer fill level and dynamically adjusts the host consumption rate to converge with the hardware crystal.

$$f_{adj} = K_p e(t) + K_i \int e(t) dt + K_d \frac{de(t)}{dt}$$

### 2. 42-Byte MIDI Protocol
Unlike standard USB-MIDI, the NS6 uses fixed 42-byte packets with `0xFD` padding. Our driver implements a native parser that ensures no LED commands or high-resolution (14-bit) Jog movements are lost during performance.

---

## ✅ Functional Features (Alpha 0.99)

* **Pro Audio (ALSA PCM):** 4-channel output (Main + Headphones) at 24-bit (S24_3LE).
* **Stable Isochronous Stream:** Packet management designed to keep the hardware clock "warm."
* **Native Handshake:** Automated boot sequence (Activate Vendor Mode) and SysEx initialization handled at the kernel level.
* **Endpoint Draining:** EP 0x86 (Waveform) is continuously drained to prevent USB bus stalls.

---

## 🏗️ Technical Heritage
This driver is built upon the foundational logic of the **Ozzy Project** by **Marcel Bierling (Ploytec)**. We are honoring that legacy by extending the logic to support the specific quirks of the NS6 hardware, such as the unique initialization sequence and `bReq=0xE0` vendor requests.

---

## 🛠️ How to Help (Call for Developers)

We are in the final stabilization phase. If you are an **ALSA developer**, a **USB timing expert**, or a **PID tuning wizard**, we need your help with:

1.  **PID Calibration:** Fine-tuning the $K_p$, $K_i$, and $K_d$ parameters for smooth convergence across different CPU governors.
2.  **The bReq=0xE0 Mystery:** Investigating xHCI limitations on Linux to replicate the proprietary Ploytec polling.
3.  **Waveform Decoding:** Reverse engineering the EP 0x86 data protocol to enable the onboard LCD displays.

---

## 📥 Fast Installation

### Prerequisites (Fedora example)
```bash
sudo dnf install kernel-devel kernel-headers development-tools

Bash
git clone [https://github.com/maurojuniorr/numark-ns6-linux.git](https://github.com/maurojuniorr/numark-ns6-linux.git)
cd numark-ns6-linux/kmod-total
make clean && make
sudo make install
sudo modprobe snd-ns6

Developed with ❤️ by:
Mauro Junior (@maurojuniorr) & Gemini (IA)
Husband, Father, Programmer, Professor, Researcher, and DJ.
