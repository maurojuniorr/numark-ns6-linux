# CoreAudio HAL device

Created and maintained by **Mauro Junior** ([maurojuniorr](https://github.com/maurojuniorr)).

This directory is the macOS output-device layer for the original Numark NS6.

The USB transport has already been validated on Apple Silicon through
`macos/tools/ns6-native-tone.c`: it claims interface 0, selects alternate
setting 1, uses `LowLatencyWriteIsochPipeAsync`, and produces a clean 44.1 kHz
test tone through the NS6.

The HAL plug-in publishes one 4-channel, 44.1 kHz Float32 output device. Its
worker converts the CoreAudio mix to the packed 24-bit format used by the NS6.
Channels 1/2 feed Master and channels 3/4 feed Headphones independently, so
DJ software can route its main mix and cue mix separately.

At stream start, the worker waits briefly for 2048 PCM frames before submitting
the first eight USB transfers. This prevents the device from alternating
between empty packets and live audio while CoreAudio is filling its first
buffers.

During playback, a slow queue controller keeps about 4096 frames buffered and
adjusts the 5/6-frame USB packet cadence by at most 1,417 ppm. This absorbs the
small clock difference between the Mac and the NS6 before the PCM queue can
underflow or overflow during a long session.

The former Numark package included an Intel-only Ploytec kext. This project
does not load or depend on it.

## Data path

`DoIOOperation(WriteMix)` copies PCM to `NS6Transport` without allocation or
locking. The USB worker removes packed 24-bit frames from that queue and emits
the 5/6-frame high-speed ISO packet pattern used by the validated native
transport. This separation is required because CoreAudio invokes `DoIOOperation`
on a real-time deadline while USB completion callbacks run on a normal run loop.

## Build and install

Connect and power on the NS6, then run:

```sh
cd macos/coreaudio
make
make usb-test
make install
```

`make usb-test` sends a 440 Hz tone through the same queue, conversion, and USB
worker used by the HAL plug-in. After installation, select **Numark NS6** in
System Settings > Sound > Output. The installed driver is arm64 and is kept
separate from Numark's old Intel-only bundle.

## Installer package

Build a local macOS installer package with:

```sh
make package
```

The package is written to `dist/NumarkNS6-0.1.0.pkg`. Open it in Finder or
install it from Terminal with:

```sh
sudo installer -pkg dist/NumarkNS6-0.1.0.pkg -target /
```

It installs the arm64 HAL bundle in `/Library/Audio/Plug-Ins/HAL` and restarts
only `coreaudiod`. It also adds **Numark NS6 Status** to Applications. The app
shows live USB connection state, output format, channel count, and the driver
version. Firmware is deliberately shown as not queried until the driver can
read it from the hardware. The package is locally built and unsigned for
distribution; it does not require or include Numark's legacy Intel kext.

To remove this build:

```sh
make uninstall
```
