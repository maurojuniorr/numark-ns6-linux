# NS6 USB reconnection notes

## Linux result — 2026-11-05

The daemon now recovers the USB transport inside the same process. On USB loss,
it stops and joins the USB/MIDI workers, cancels and drains outstanding libusb
transfers, releases the old handle, and retries opening the NS6. After the
device returns, it repeats the NS6 initialization and starts the workers again.

The ALSA Sequencer client/port and PipeWire streams stay alive through that
cycle. Audio rings and decoder/clock state are reset so queued audio from before
the disconnect is not replayed. Held PLAY, CUE, and jog-touch notes are released
before the controller state is reset. Stale MIDI output queued for the device is
discarded.

### Hardware observation

After installing this build, the Mixxx instance was opened once to subscribe to
the current ALSA port. The NS6 USB connection was then interrupted and restored
twice while Mixxx stayed open. The daemon kept PID `26895`; ALSA client/port
`128:0` and the Mixxx subscription remained present. Mauro confirmed that both
MIDI and audio returned normally, as if the controller had not disconnected,
without restarting Mixxx.

The journal emitted one `ALSA MIDI input FIFO overran` warning during recovery,
but no MIDI or audio failure was observed. Keep this warning on the follow-up
list, especially if LED updates are missing after a reconnect.

## Follow-up for the macOS driver

Use the same lifecycle principle as an investigation, not as a direct code
translation: keep the published CoreAudio devices and CoreMIDI endpoints alive
while reopening only the USB transport. Re-run the NS6 handshake and reset the
transport's audio queues/clock state, without tearing down and recreating the
host-visible endpoints. First inspect how the macOS driver owns USB transfers,
CoreAudio callbacks, and CoreMIDI endpoint registration; then test whether
existing clients keep their subscriptions and recover audio/MIDI after a USB
reconnect. This approach has not yet been implemented or validated on macOS.
