# How it works

[← Back to the README](../README.md)

## Why this is possible at all

The StudioLive AI consoles stream their audio to a computer over **FireWire
800**, and PreSonus has said in writing that this will never work on Apple
Silicon: *"StudioLive AI-Series mixers will not work with Apple Silicon Macs, and
there are no plans to develop a driver."* Universal Control 3.6.0 was the last
FireWire release.

**But the bus itself works.** Through Apple's Thunderbolt→FireWire adapter
chain, `system_profiler SPFireWireDataType` lists the console at full speed:
- Apple's `IOFireWireFamily` and `AppleFWOHCI` load, claim the adapter's OHCI
  controller and enumerate the console.
- `IOFireWireLib` gives asynchronous transactions and isochronous channels to
  ordinary user-space processes.

So the console is reachable. The missing piece was something that turns that
FireWire device into a Core Audio device, and that is what this project is.

## The pieces

- **Transport.** Apple's `IOFireWireFamily` and `AppleFWOHCI` do the OHCI
  bring-up, DMA rings and transaction layer. `IOFireWireLib` gives this code the
  device, async register reads, and an isochronous channel with a local port
  driven by a DCL receive program.
- **Protocol.** The console is TCAT DICE. The register map comes from Linux's
  `snd-dice` driver and was checked against the hardware rather than trusted:
  the nickname register byte-swaps to exactly `StudioLive`. The stream is
  IEC 61883-6 AM824. `src/slfw.c` depacketises it into a lock-free frame ring
  and fits a clock to it.
- **Sample rate** is measured from the stream itself:
  - Blocking-mode IEC 61883-6 sends one packet per FireWire cycle, 8000 a
    second, so frames ÷ packets × 8000 is the sample rate, readable within half
    a second.
  - None of the console's own rate fields can be trusted. On the tested
    console, the CIP FDF field and `clock_select` claim 48 kHz while it streams
    44.1 kHz, and while it boots DICE `GLOBAL_STATUS` is wrong in both
    directions.
  - `GLOBAL_STATUS` is only the starting guess. When the measured rate
    disagrees with the advertised one, the driver has Core Audio reconfigure
    the device within about a second.
  - A stream that stops delivering for two seconds is reopened, at the rate
    Core Audio already has.
- **Core Audio.** `driver/plugin.c` is an AudioServerPlugIn that owns FireWire
  directly, with no helper daemon, and opens the console lazily at StartIO:
  - Its `Info.plist` declares `AudioServerPlugIn_IOKitUserClients =
    [IOFireWireUserClient]`, which is what lets Core Audio's sandboxed driver
    service open a FireWire device at all.
  - The thread that receives packets runs time-constrained, like Core Audio's
    own I/O threads.
  - Zero timestamps are latched from the clock fit and steered by no more than
    200 ppm.
  - It publishes a status property that the menu bar icon reads.

## The evidence

[`spike/README.md`](../spike/README.md) covers the two throwaway probes that
proved user-space FireWire access and isochronous receive on Apple Silicon
before any of the real code was written, and what they taught.
