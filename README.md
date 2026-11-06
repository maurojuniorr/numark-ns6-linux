# Numark NS6 Linux Driver

This repository contains a Linux userspace driver for the Numark NS6. The
current implementation is a C daemon (`ns6d`) using libusb for the controller,
ALSA Sequencer for MIDI, and PipeWire for audio. It is not a kernel module and
does not use `snd-aloop`.

The project aims to provide the NS6's audio interface and MIDI controls on
Linux, including reliable audio capture from the controller's internal record
mix and recovery after USB disconnects.

## Current progress

- **MIDI input and output:** the daemon parses the NS6's packed 42-byte USB
  reports as a continuous MIDI stream, including 14-bit jog data, and exposes
  an ALSA Sequencer port named **Numark USB Audio Device MIDI**. Mixxx controls
  and LEDs have been exercised on the hardware.
- **Audio output:** PipeWire publishes **Numark USB Audio Device** as a
  four-channel, 44.1 kHz S24_3LE sink. Channels 1–2 feed Main; channels 3–4
  feed the headphones.
- **Audio input:** PipeWire publishes **Numark USB Audio Device Input** as a
  stereo, 44.1 kHz S24_3LE source. The daemon decodes the NS6's packed Bulk IN
  endpoint `0x86`; USB capture reads run only while a PipeWire input client is
  active. The input path was corrected and a 440 Hz capture test was reported
  clear.
- **Clock and buffering:** the daemon reads the NS6 feedback endpoint `0x81`
  and adjusts output packet cadence against the USB clock and output queue.
  Bounded SPSC rings separate PipeWire processing from USB transfers.
- **USB recovery:** after a disconnect, the daemon reopens the USB transport
  and repeats the NS6 handshake while preserving its ALSA MIDI port and
  PipeWire streams. In two hardware reconnects, Mixxx stayed open and the user
  confirmed audio and MIDI returned normally without restarting it. See the
  [reconnection test notes](docs/usb-reconnection.md).

This is an actively tested prototype. The latest hardware checks were performed
on one Ubuntu notebook; broader stability and distribution testing remain.

## Work remaining

- Investigate one ALSA Sequencer input FIFO overrun warning observed during
  reconnection. It caused no user-observed MIDI or audio failure, but its source
  and effect on LED updates still need confirmation.
- Run longer combined playback and capture sessions and record underrun/drop
  counters under realistic load.
- Verify LED initialization and state recovery after repeated USB disconnects,
  including when multiple PipeWire clients are active.
- Test clean installation, startup, suspend/resume, and reconnection on more
  Linux distributions and hardware configurations.
- Audit the macOS driver's USB/CoreAudio/CoreMIDI lifecycle, then evaluate
  whether the same endpoint-preserving recovery approach can be used there. It
  has not been implemented or validated on macOS.

## Build and installation

Dependencies and setup steps are in the [daemon guide](daemon/README.md).
For the Linux daemon, install the dependencies, build with CMake, install the
systemd user service and udev rules, then select the NS6 sink/source in the
audio application.

## USB recovery design

The key recovery principle is to reopen the physical USB transport without
destroying host-visible MIDI and audio endpoints. The daemon cancels and drains
outstanding libusb transfers, resets queued audio and controller state, runs the
NS6 initialization sequence again, then restarts its transport workers. This
keeps Mixxx subscribed to the same ALSA port across an NS6 reconnect.
