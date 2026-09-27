# Troubleshooting

[← Back to the README](../README.md)

The [menu bar icon](tools.md#the-menu-bar-icon) is the quickest first check. It
shows whether the console is there, whether it is streaming, and whether any
audio has been lost.

## Meters move, but there is no sound

On **StudioLive FireWire** on its own, that is by design: its only outputs are
the sink. Monitor on the console, or build an
[aggregate](studio-one.md#hear-playback-too-an-aggregate-device).

On the aggregate, check that its output device is still attached:

```sh
build/mkaggregate --list          # "StudioLive + ..." must show more outs than the sink's 2
build/tone "StudioLive + " 3      # a 440 Hz tone through the same device Studio One uses
```

The console alone contributes 40 in and the sink's 2 out. So **out 2 with in
40** means the aggregate's output device has dropped off the Mac, typically a
USB interface that was unplugged. Plugging it back in is enough: the aggregate
keeps it in its configuration and reattaches it without a rebuild.

If the tone is audible and Studio One is not, the problem is inside the song:

1. **Song ▸ Song Setup ▸ Audio I/O Setup ▸ Outputs**: the **Main** row must be
   ticked on the audible pair, which on the aggregate is **3 + 4**. Outputs 1 +
   2 are the driver's sink and are silent by design. Check this **even if the
   metronome plays**: the click can be routed on its own, so hearing it does not
   prove Main is assigned.
2. To hear the console's inputs through the Mac, turn on the track's
   **monitor** button. Expect a 25–40 ms round trip; the console's own outputs
   have none.

## Clicks every minute or two

A regular click or crackle through the aggregate, about every 2 minutes at a
256-sample block size or every minute at 128, is the output device's clock
drifting against the console's. A bigger block size only spaces the clicks
out.

The aggregate needs drift compensation on the output device:
1. `build/mkaggregate --list` says when it is off.
2. `build/mkaggregate --sync` turns it on. The menu bar icon offers the same
   fix.
3. It applies the next time Studio One starts audio, so switch the audio device
   away and back, or restart Studio One.

Aggregates made by older versions of `mkaggregate` were built with it off.

## Individual channels are much quieter than the main mix

That is expected. Each channel is sent to the Mac before its fader, so the
faders, the master and any processing on the main bus do not reach it. Set the
recording level with each channel's **gain knob**, aiming for peaks around −18
to −12 dBFS, then use the faders to balance what you hear from the console.

## After the console changes sample rate

The aggregate follows the console to its new rate, but Core Audio leaves the
output device at the old one. `build/mkaggregate --list` says so when the two
disagree, and one command fixes it without rebuilding anything:

```sh
build/mkaggregate --sync          # moves the output device to the console's rate, and checks drift compensation
```

Change the song's sample rate in Studio One to match the console as well.

## Known issues

- **Do not unplug the adapter, or power off the console, while anything is
  recording or playing through the driver.**
  - If the FireWire link disappears mid-stream, macOS's own FireWire teardown
    gets stuck. The console does not come back, and every Thunderbolt port can
    report no device.
  - The driver releases everything it holds, which was checked with its log and
    its Mach ports while stuck, so the hang is inside the kernel.
  - To recover, plug the adapter into a *different* Thunderbolt port, or
    reboot. `ioreg -r -c IOFireWireUserClient -w0 | grep inactive` shows the
    stuck client.
  - Unplugging while nothing is streaming is fine.
- **The first 15–20 seconds after powering the console on are unstable.** While
  it boots, the console reinitialises several times. Its stream stalls, and it
  can switch between 44.1 and 48 kHz for a few seconds. The driver follows the
  real stream within about a second each time and reopens a stalled stream on
  its own, but audio recorded in that window glitches. Power the console on
  first, and start recording once it has settled.
- **The driver's log is only visible to root.** It is filed under the process
  `com.apple.audio.Core-Audio-Driver-Service.helper`:
  `sudo log show --last 10m --info --predicate 'subsystem == "n12n.presonus-adapter"'`.
  Plain `log show` returns nothing.
- **Under very heavy CPU load**, such as Spotlight re-indexing right after a
  reboot, the driver can hand Core Audio silence while it catches up.
- **Input only.** The console's return channels (26 on the 24.4.2AI) are not
  exposed.
- **One console at a time.**
- **It depends on Apple's FireWire kernel extensions.** A future macOS that
  removes them ends this, with no workaround.
