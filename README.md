# studiolive-firewire-mac

A user-space Core Audio driver that lets an **Apple Silicon Mac record a PreSonus
StudioLive AI console over FireWire** — something PreSonus has said it will never
support.

It needs no kernel extension, no Reduced Security mode and no special
entitlement. It rides on Apple's own FireWire stack, which still ships and still
loads on Apple Silicon, and turns the console into an ordinary Core Audio input
device that any Core Audio app can see. Studio One 5 is the only one tested.

> Not affiliated with, endorsed by, or supported by PreSonus. "PreSonus",
> "StudioLive" and "Studio One" are trademarks of their owners. This talks to
> your console over FireWire and writes to its streaming registers — use it at
> your own risk.

## The problem

The StudioLive AI consoles stream their audio to a computer over **FireWire
800**, and PreSonus has said in writing that this will never work on Apple
Silicon: *"StudioLive AI-Series mixers will not work with Apple Silicon Macs, and
there are no plans to develop a driver."* Universal Control 3.6.0 was the last
FireWire release.

**But the bus itself works.** With Apple's Thunderbolt→FireWire adapter chain,
`system_profiler SPFireWireDataType` lists the console at full speed. Apple's
`IOFireWireFamily` and `AppleFWOHCI` load, claim the adapter's OHCI controller and
enumerate the console, and `IOFireWireLib` hands asynchronous transactions and
isochronous channels to ordinary user-space processes. The console is reachable
and mute; the only missing piece was something that turns that FireWire unit into
a Core Audio device. This is that piece.

## What has been tested

Exactly one setup, and only this:

| | |
|---|---|
| Console | StudioLive **24.4.2AI** |
| Mac | MacBook Pro, **M4 Pro**, macOS **15.1** |
| Adapters | Apple Thunderbolt to FireWire Adapter (**A1463**) → Apple Thunderbolt 3 (USB-C) to Thunderbolt 2 Adapter (**A1790**) → a Thunderbolt port on the Mac |
| DAW | Studio One **5.5.2**, through an aggregate device |

On that setup Core Audio lists **StudioLive FireWire** with 40 inputs at
44.1 kHz, a 10-second recording through Core Audio has no gaps with all 40
channels live, and Studio One records from it.

**Untested:** the other AI mixers (16.4.2AI, 32.4.2AI, RM16AI, RM32AI), other
Apple Silicon Macs, other macOS versions, and 48 kHz. The DICE protocol they
speak is shared, so they may work; nobody has checked. The driver
opens the first PreSonus FireWire device on the bus. Reports either way are
welcome.

## Requirements

- An **Apple Silicon Mac** with a Thunderbolt 3 or 4 (USB-C) port.
- **Both Apple adapters**, A1463 and A1790, plugged **straight into the Mac**.
  - **No hub or dock** in between: a USB-C hub does not carry Thunderbolt.
  - Thunderbolt 2 uses the **Mini DisplayPort** connector shape, so Mini
    DisplayPort *video* adapters and cables fit — and carry no PCIe at all. If the
    console does not appear, check that nothing in the chain is a video part.
- **Xcode Command Line Tools**: `xcode-select --install`.
- An administrator account, to install the driver.

To check the hardware chain before building anything, run
`scripts/probe-firewire.sh baseline` with the adapters unplugged, then
`scripts/probe-firewire.sh check` with them plugged in. It records what it finds
under `var/`, which is gitignored because it includes serial numbers.

## Build and install

```sh
make                                          # the driver, the recorder and the checks
build/halcheck build/StudioLiveFW.driver 20   # drives the plug-in in a plain process; console must be on the bus
sudo scripts/install-driver.sh                # copies it to /Library/Audio/Plug-Ins/HAL and restarts Core Audio
build/cacheck 10                              # records 10 s THROUGH Core Audio; must say PASSED
```

There are no prebuilt downloads: build it yourself. The driver is ad-hoc signed,
which is fine for a plug-in built and installed on the same Mac, but macOS would
block a downloaded ad-hoc plug-in, and there is no Developer ID to sign one with. Restarting Core Audio cuts all audio on the Mac for a second or
two. `sudo` needs an interactive terminal; from a tool without one, run the script
through `osascript -e 'do shell script "/full/path/to/scripts/install-driver.sh"
with administrator privileges'`.

Core Audio then lists **StudioLive FireWire**: one input per console stream
channel, named as the console names them — on the 24.4.2AI, CH1–CH24 and
AUX1–AUX16.

To remove it: `sudo scripts/uninstall-driver.sh`.

## Recording in Studio One 5

Studio One 5 uses a single device for both recording and playback, and this
driver is input-only. So it records through an **aggregate device** that adds an
output for playback:

```sh
build/mkaggregate --list                     # audio devices and their channel counts
build/mkaggregate "MacBook Pro Speakers"     # or any output device, by name substring
```

That creates **StudioLive + \<output device\>**, with the console as clock master
— recorded audio is never resampled; the output device is drift-compensated
instead. On a 24.4.2AI its inputs are numbered:

| aggregate inputs | source |
|---|---|
| 1–24 | console CH1–CH24 |
| 25–40 | console AUX1–AUX16 |
| 41 and up | the output device's own inputs, if it has any |

In Studio One:

1. **Studio One ▸ Preferences… ▸ Audio Setup ▸ Audio Device** → the
   *StudioLive + …* aggregate. The song rate must match the console's rate
   (44.1 kHz on the tested console).
2. **Song ▸ Song Setup ▸ Audio I/O Setup ▸ Inputs** — add a mono input per
   console channel you want and tick its number from the table above.
3. Make audio tracks, pick those inputs, arm, record.

**Monitor on the console.** Its headphone and monitor outputs have no latency.
Audio monitored through Studio One comes back through the aggregate's output
25–40 ms late, so turn input monitoring off on armed tracks unless you are
deliberately listening through the Mac — otherwise a *monitor follows record*
setting plays every input back a second time, late.

### Meters move, but there is no sound

Capture is working; playback has nowhere to go. Check the aggregate still has
outputs:

```sh
build/mkaggregate --list          # "StudioLive + ..." must show out 2 (or more)
build/tone "StudioLive + " 3      # a 440 Hz tone through the same device Studio One uses
```

`out 0` means the aggregate's output device has dropped off the Mac, typically a
USB interface that was unplugged. Plugging it back in is enough: the aggregate
keeps it in its configuration and reattaches it without a rebuild.

If the tone is audible and Studio One is not, the problem is inside the song:

1. **Song ▸ Song Setup ▸ Audio I/O Setup ▸ Outputs** — the **Main** row must have
   outputs 1 + 2 ticked. Check this **even if the metronome plays**: the click can
   be routed on its own, so hearing it does not prove Main is assigned.
2. To hear the console's inputs through the Mac, turn on the track's **monitor**
   button. Expect a 25–40 ms round trip; the console's own outputs have none.

## Tools

| | |
|---|---|
| `build/slrecord -s 60 -o take.wav` | every channel to one WAV, bypassing Core Audio, with a level table |
| `build/halcheck build/StudioLiveFW.driver 20` | loads the plug-in in an ordinary process and drives it the way Core Audio would — run before installing any change |
| `build/cacheck 10` | an ordinary Core Audio client recording from the installed driver — the end-to-end check |
| `build/mkaggregate --list` / `"<output>"` | lists audio devices / builds the Studio One aggregate |
| `build/tone "<device>" [seconds] [left\|right\|both]` | a 440 Hz test tone out of a named device |

The driver opens the console only while some app is doing I/O, so `slrecord` and
the spikes work whenever nothing is recording through Core Audio, and fail with
an exclusive-access error when something is.

## How it works

- **Transport.** Apple's `IOFireWireFamily` and `AppleFWOHCI` do the OHCI
  bring-up, DMA rings and transaction layer. `IOFireWireLib` gives this code the
  device, async register reads, and an isochronous channel with a local port
  driven by a DCL receive program.
- **Protocol.** The console is TCAT DICE. The register map comes from Linux's
  `snd-dice` driver and was checked against the hardware rather than trusted —
  the nickname register byte-swaps to exactly `StudioLive`. The stream is
  IEC 61883-6 AM824: `src/slfw.c` depacketises it into a lock-free frame ring and
  fits a clock to it.
- **Sample rate** is measured from the stream itself. Blocking-mode IEC 61883-6
  sends one packet per FireWire cycle, 8000 a second, so frames ÷ packets × 8000
  is the sample rate, readable within half a second. None of the console's own
  rate fields can be trusted: on the tested console the CIP FDF field and
  `clock_select` claim 48 kHz while it streams 44.1 kHz, and while it boots DICE
  `GLOBAL_STATUS` is wrong in both directions. `GLOBAL_STATUS` is only the
  starting guess; when the measured cadence disagrees with the advertised rate,
  the driver has Core Audio reconfigure the device, within about a second. A
  stream that stops delivering for two seconds is reopened, at the rate Core
  Audio already has.
- **Core Audio.** `driver/plugin.c` is an AudioServerPlugIn that owns FireWire
  directly, with no helper daemon, and opens the console lazily at StartIO. Its
  `Info.plist` declares `AudioServerPlugIn_IOKitUserClients =
  [IOFireWireUserClient]`, which is what lets Core Audio's sandboxed driver
  service open a FireWire device at all. Zero timestamps are latched from the
  clock fit and steered by no more than 200 ppm.

[`spike/README.md`](spike/README.md) is the evidence: the two throwaway probes
that proved user-space FireWire access and isochronous receive on Apple Silicon
before any of the real code was written, and what they taught.

## Known issues

- **Do not unplug the adapter, or power off the console, while anything is
  recording or playing through the driver.** If the FireWire link disappears
  while audio is streaming, macOS's own FireWire teardown gets stuck: the console
  does not come back, and every Thunderbolt port can report no device. The driver
  releases everything it holds — this was checked with the driver's log and its
  Mach ports while stuck — so the hang is inside the kernel. Recover by plugging
  the adapter into a *different* Thunderbolt port, or by rebooting.
  `ioreg -r -c IOFireWireUserClient -w0 | grep inactive` shows the stuck client.
  Unplugging while nothing is streaming is fine.
- **The first 15–20 seconds after powering the console on are unstable.** While
  it boots, the console reinitialises several times: its stream stalls, and it
  can switch between 44.1 and 48 kHz for a few seconds. The driver follows the
  real stream within about a second each time and reopens a stalled stream on
  its own, but audio recorded in that window glitches. Power the console on
  first, and start recording once it has settled.
- **The driver's log is only visible to root.** It is filed under the process
  `com.apple.audio.Core-Audio-Driver-Service.helper`:
  `sudo log show --last 10m --info --predicate 'subsystem == "n12n.presonus-adapter"'`.
  Plain `log show` returns nothing.
- **Under very heavy CPU load** (for example Spotlight re-indexing right after a
  reboot) the driver can hand Core Audio silence while it catches up.
- **Input only.** The console's return channels (26 on the 24.4.2AI) are not
  exposed.
- **One console at a time.**
- **It depends on Apple's FireWire kernel extensions.** A future macOS that
  removes them ends this, with no workaround.

## Alternatives

If you would rather not depend on any of this: a StudioLive **SL-Dante-MIX**
option card with Dante Virtual Soundcard is maintained by someone else, and the
console's DB-25 direct outs into a multichannel analog interface need no
software at all.

## License

GPL-2.0-or-later — see [`LICENSE`](LICENSE). No code was copied from other
projects. The DICE register map, stream layout and AM824 handling were learned
from reading [FFADO](http://www.ffado.org/) and the Linux kernel's `snd-dice`
driver, both GPL, and then checked against the hardware.
