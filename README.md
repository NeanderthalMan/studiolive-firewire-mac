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
| DAW | Studio One **5.5.2**, on the device directly or through an aggregate |

On that setup Core Audio lists **StudioLive FireWire** with 40 inputs and a
2-channel sink output at 44.1 kHz, a 10-second recording through Core Audio has
no gaps with all 40 channels live, and Studio One records from it.

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
make test                                     # unit tests; no console or sudo needed
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
AUX1–AUX16 — and a stereo output named **Not connected 1** and **Not connected
2**, which is exactly what it says.

To remove it: `sudo scripts/uninstall-driver.sh`.

### The menu bar icon

```sh
scripts/install-menu.sh        # copies build/StudioLiveMenu.app to ~/Applications and starts it
```

A waveform in the menu bar says whether the driver has a console and is
streaming, without opening anything:

| icon | means |
|---|---|
| waveform, struck through | no console on the FireWire bus, or the driver is not loaded |
| waveform | console present, nothing recording from it |
| filled waveform circle | streaming to an app |
| warning triangle | audio was lost in the last minute, or the aggregate's output device is not drift-compensated |

Clicking it shows the console, its rate, how long the stream has been up,
frames lost, read underruns and the worst call-back gap. It offers to turn on
drift compensation when the aggregate needs it, and to open Audio MIDI Setup
or copy all of that for a bug report. **Open at Login** starts it with the Mac.
The numbers come from the driver through Core Audio, so it needs a driver
built from this version or later; an older one shows as present with nothing
more.

## Recording in Studio One 5

Studio One uses a single device for both recording and playback, and will not
run its engine on a device with no outputs. So the driver publishes a stereo
output of its own — **Not connected 1** and **Not connected 2** — and discards
whatever is written to it. That is enough to select **StudioLive FireWire**
directly, with no aggregate and no second interface:

1. **Studio One ▸ Preferences… ▸ Audio Setup ▸ Audio Device** → **StudioLive
   FireWire**. The song rate must match the console's rate (44.1 kHz on the
   tested console).
2. **Song ▸ Song Setup ▸ Audio I/O Setup ▸ Inputs** — add a mono input per
   console channel you want; they are named as the console names them.
3. Make audio tracks, pick those inputs, arm, record.

Nothing will be audible from the Mac, including playback of the take you just
recorded. That is the trade: monitoring belongs on the console, where it has no
latency, and the console is where the audio already is.

### To hear playback too: an aggregate

Add a real output device with an **aggregate device**. Any output that can run
at the console's rate will do — built-in speakers are fine, no extra hardware
needed:

```sh
build/mkaggregate --list                     # audio devices, their channel counts and rates
build/mkaggregate "MacBook Pro Speakers"     # or any output device, by name
```

The name picks the output device whose whole name matches, ignoring case, or
else the only one whose name contains what you typed — `"AudioBox"` is enough
when there is one AudioBox. If that part matches more than one output device,
`mkaggregate` lists them and stops; give a full name. There is only ever one
StudioLive aggregate: running it again replaces the last one.

That creates **StudioLive + \<output device\>**, with the console as clock master
— recorded audio is never resampled; the output device is drift-compensated
instead. On a 24.4.2AI its inputs are numbered:

| aggregate inputs | source |
|---|---|
| 1–24 | console CH1–CH24 |
| 25–40 | console AUX1–AUX16 |
| 41 and up | the output device's own inputs, if it has any |

The console comes first in the aggregate, so its own sink comes first among the
outputs as well, and **the audible outputs are 3 and 4**:

| aggregate outputs | goes to |
|---|---|
| 1–2 | the driver's sink — *Not connected 1* and *2*, discarded |
| 3 and up | the output device |

Core Audio recomputes a sub-device's channel counts from the device itself, so
there is no way to ask it to leave the sink out. The names are the defence:
*Not connected* says what it is wherever Studio One lists it.

Then, in Studio One:

1. **Studio One ▸ Preferences… ▸ Audio Setup ▸ Audio Device** → the
   *StudioLive + …* aggregate. The song rate must match the console's rate
   (44.1 kHz on the tested console).
2. **Song ▸ Song Setup ▸ Audio I/O Setup ▸ Inputs** — add a mono input per
   console channel you want and tick its number from the table above.
3. **▸ Outputs** — the **Main** row wants **3 + 4**, not 1 + 2.
4. Make audio tracks, pick those inputs, arm, record.

**Monitor on the console.** Its headphone and monitor outputs have no latency.
Audio monitored through Studio One comes back through the aggregate's output
25–40 ms late, so turn input monitoring off on armed tracks unless you are
deliberately listening through the Mac — otherwise a *monitor follows record*
setting plays every input back a second time, late.

### Meters move, but there is no sound

On **StudioLive FireWire** on its own, that is the design: its only outputs are
the sink. Monitor on the console, or build an aggregate as above.

On the aggregate, check its output device is still attached:

```sh
build/mkaggregate --list          # "StudioLive + ..." must show more outs than the sink's 2
build/tone "StudioLive + " 3      # a 440 Hz tone through the same device Studio One uses
```

The console alone contributes 40 in and the sink's 2 out, so **out 2 with in 40**
is the aggregate saying its output device has dropped off the Mac — typically a
USB interface that was unplugged. Plugging it back in is enough: the aggregate
keeps it in its configuration and reattaches it without a rebuild.

If the tone is audible and Studio One is not, the problem is inside the song:

1. **Song ▸ Song Setup ▸ Audio I/O Setup ▸ Outputs** — the **Main** row must be
   ticked on the audible pair, which on the aggregate is **3 + 4**; 1 + 2 are the
   driver's sink and are silent by design. Check this **even if the metronome
   plays**: the click can be routed on its own, so hearing it does not prove Main
   is assigned.
2. To hear the console's inputs through the Mac, turn on the track's **monitor**
   button. Expect a 25–40 ms round trip; the console's own outputs have none.

### After the console changes sample rate

The aggregate follows the console to its new rate, but Core Audio leaves the
output device at the old one. `build/mkaggregate --list` says so when the two
disagree, and one command fixes it without rebuilding anything:

```sh
build/mkaggregate --sync          # moves the output device to the console's rate, and checks drift compensation
```

Change the song's sample rate in Studio One to match the console as well.

### Clicks every minute or two

A regular click or crackle through the aggregate — about every 2 minutes at a
256-sample block size, every minute at 128 — is the output device's clock
drifting against the console's, and a bigger block size only spaces the clicks
out. The aggregate needs drift compensation on the output device.
`build/mkaggregate --list` says when it is off, and `--sync` turns it on; it
applies the next time Studio One starts audio, so switch the audio device away
and back, or restart Studio One. Aggregates made by `mkaggregate` before this
check existed were built with it off.

## Tools

| | |
|---|---|
| `build/slrecord -s 60 -o take.wav` | every channel to one WAV, bypassing Core Audio, with a level table |
| `build/halcheck build/StudioLiveFW.driver 20` | loads the plug-in in an ordinary process and drives it the way Core Audio would — run before installing any change |
| `build/cacheck 10` | an ordinary Core Audio client recording from the installed driver — the end-to-end check |
| `build/mkaggregate --list` / `"<output>"` / `--sync` | lists audio devices and their rates / builds the Studio One aggregate / puts its output device back at the console's rate and turns its drift compensation on |
| `build/tone "<device>" [seconds] [left\|right\|both]` | a 440 Hz test tone out of a named device |
| `make test` | unit tests for the depacketiser, clock fit, I/O path and menu logic — no console or sudo needed |
| `scripts/install-menu.sh` | installs and starts the menu bar icon |

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
