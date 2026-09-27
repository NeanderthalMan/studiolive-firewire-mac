# Recording in Studio One 5

[← Back to the README](../README.md)

Studio One uses a single device for both recording and playback, and will not
run its engine on a device with no outputs. So the driver publishes a stereo
output of its own, **Not connected 1** and **Not connected 2**, and discards
whatever is written to it. That is enough to select **StudioLive FireWire**
directly, with no aggregate and no second interface.

## Record from the console

1. **Studio One ▸ Preferences… ▸ Audio Setup ▸ Audio Device** → **StudioLive
   FireWire**. The song rate must match the console's rate (44.1 kHz on the
   tested console).
2. **Song ▸ Song Setup ▸ Audio I/O Setup ▸ Inputs**: add a mono input for each
   console channel you want. They are named as the console names them.
3. Make audio tracks, pick those inputs, arm, and record.

Nothing will be audible from the Mac, including playback of the take you just
recorded. That is the trade: monitoring belongs on the console, where it has no
latency.

**Set levels with the console's gain knob, not its faders.** Each channel is
sent to the Mac before its fader, so the faders and the master change what you
hear from the console but not what is recorded. Aim for peaks around −18 to
−12 dBFS. A dynamic mic such as an SM57 or SM58 needs a lot of gain, often well
past halfway on the knob, and that is normal.

## Hear playback too: an aggregate device

To hear playback, add a real output device with an **aggregate device**. Any
output that can run at the console's rate will do. Built-in speakers are fine,
and no extra hardware is needed:

```sh
build/mkaggregate --list                     # audio devices, their channel counts and rates
build/mkaggregate "MacBook Pro Speakers"     # or any output device, by name
```

**Naming the output device:**
- A whole name matches, ignoring case.
- Otherwise it takes the only device whose name contains what you typed, so
  `"AudioBox"` is enough when there is one AudioBox.
- If that matches more than one output device, `mkaggregate` lists them and
  stops. Give a full name.
- There is only ever one StudioLive aggregate. Running it again replaces the
  last one.

That creates **StudioLive + \<output device\>** with the console as the clock
master. Recorded audio is never resampled; the output device is
drift-compensated instead. On a 24.4.2AI its inputs are numbered:

| Aggregate inputs | Source |
|---|---|
| 1–24 | console CH1–CH24 |
| 25–40 | console AUX1–AUX16 |
| 41 and up | the output device's own inputs, if it has any |

The console comes first in the aggregate, so its own sink comes first among the
outputs as well, and **the audible outputs are 3 and 4**:

| Aggregate outputs | Goes to |
|---|---|
| 1–2 | the driver's sink, *Not connected 1* and *2*: discarded |
| 3 and up | the output device |

Core Audio recomputes a sub-device's channel counts from the device itself, so
there is no way to leave the sink out. The names are the defence: *Not
connected* says what it is wherever Studio One lists it.

Then, in Studio One:

1. **Studio One ▸ Preferences… ▸ Audio Setup ▸ Audio Device** → the
   *StudioLive + …* aggregate. The song rate must match the console's rate.
2. **Song ▸ Song Setup ▸ Audio I/O Setup ▸ Inputs**: add a mono input for each
   console channel you want and tick its number from the table above.
3. **▸ Outputs**: the **Main** row wants **3 + 4**, not 1 + 2.
4. Make audio tracks, pick those inputs, arm, and record.

**Monitor on the console.** Its headphone and monitor outputs have no latency.
Audio monitored through Studio One comes back through the aggregate 25–40 ms
late. Turn input monitoring off on armed tracks unless you are deliberately
listening through the Mac; otherwise a *monitor follows record* setting plays
every input back a second time, late.

Something not working? See [Troubleshooting](troubleshooting.md).
