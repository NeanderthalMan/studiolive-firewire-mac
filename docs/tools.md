# Tools

[← Back to the README](../README.md)

`make` builds all of these into `build/`.

| | |
|---|---|
| `scripts/install-menu.sh` | installs and starts the [menu bar icon](#the-menu-bar-icon) |
| `build/mkaggregate --list` | lists audio devices and their rates, and warns about a StudioLive aggregate that needs fixing |
| `build/mkaggregate "<output>"` | builds the Studio One aggregate; see [Recording in Studio One](studio-one.md#hear-playback-too-an-aggregate-device) |
| `build/mkaggregate --sync` | puts the aggregate's output device back at the console's rate and turns its drift compensation on |
| `build/slrecord -s 60 -o take.wav` | records every channel to one WAV, bypassing Core Audio, and prints a level table |
| `build/halcheck build/StudioLiveFW.driver 20` | loads the plug-in in an ordinary process and drives it the way Core Audio would; run it before installing any change |
| `build/cacheck 10` | an ordinary Core Audio client recording from the installed driver: the end-to-end check |
| `build/tone "<device>" [seconds] [left\|right\|both]` | a 440 Hz test tone out of a named device |
| `make test` | unit tests for the depacketiser, clock fit, I/O path and menu logic; no console or sudo needed |

The driver opens the console only while some app is doing I/O. So `slrecord`
and the spikes work whenever nothing is recording through Core Audio, and fail
with an exclusive-access error when something is.

## The menu bar icon

```sh
scripts/install-menu.sh        # copies build/StudioLiveMenu.app to ~/Applications and starts it
```

A waveform in the menu bar shows whether the driver has a console and is
streaming, without opening anything:

| Icon | Means |
|---|---|
| waveform, struck through | no console on the FireWire bus, or the driver is not loaded |
| waveform | console present, nothing recording from it |
| filled waveform circle | streaming to an app |
| warning triangle | audio was lost in the last minute, or the aggregate's output device is not drift-compensated |

Clicking it shows:
- the console and its rate
- how long the stream has been up
- frames lost and read underruns
- the worst call-back gap
- whether the aggregate is drift-compensated

It also offers to turn drift compensation on when the aggregate needs it, to
open Audio MIDI Setup, and to copy all of that for a bug report. **Open at
Login** starts it with the Mac.

The numbers come from the driver through Core Audio, so the icon needs a driver
built from the same version or later. With an older driver it shows the console
as present and nothing more.
