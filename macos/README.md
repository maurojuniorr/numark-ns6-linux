# Numark NS6 on macOS

Created and maintained by **Mauro Junior** ([maurojuniorr](https://github.com/maurojuniorr)).

This directory is the arm64 macOS implementation. The original Numark HAL
bundles installed on some Macs are Intel-only and cannot load on Apple Silicon.

The implementation is split into two processes:

- `ns6d`: a privileged USB transport process using libusb. It owns the NS6
  interfaces, runs the vendor initialization sequence, streams ISO audio, and
  exposes clock telemetry.
- a future Core Audio Audio Server Plug-in: it publishes the four-channel,
  44.1 kHz S24_3LE playback device to Core Audio and exchanges audio buffers
  with `ns6d` through a local IPC channel.

The split keeps USB I/O outside `coreaudiod`, which is sandboxed and must not
perform blocking work on the real-time audio thread.

## Hardware probe

Install the build dependencies once:

```sh
brew install libusb pkgconf
```

Then build and run the read-only descriptor probe:

```sh
cd macos/tools
make ns6-libusb-probe
./ns6-libusb-probe
```

On managed development environments the command needs to run outside their
sandbox. On a normal Terminal session it only enumerates the device and reads
its descriptors; it neither claims an interface nor sends USB traffic.

The expected NS6 descriptor layout is:

| Interface | Alternate setting | Endpoint | Purpose |
| --- | --- | --- | --- |
| 0 | 1 | `0x02` | ISO OUT, four-channel S24_3LE playback |
| 0 | 1 | `0x83` / `0x04` | Bulk MIDI input / output |
| 1 | 1 | `0x81` | ISO IN feedback, currently known to be unreliable |
| 1 | 1 | `0x86` | Bulk waveform input, drained continuously |

## Clock work

The next transport milestone starts silence URBs, records actual completion
cadence and buffer occupancy, and writes a timestamped CSV trace. That gives
us a measured device rate before implementing the adaptive resampler.
