# spikes — fwprobe and isoprobe

The one-day spike that decided whether a userspace FireWire→Core Audio adapter
for the StudioLive 24.4.2AI is a real project. **It is.** Run on a
MacBook Pro M4 Pro, macOS 15.1, with the console on the bus through an Apple
A1463 + A1790.

```sh
cc -o fwprobe fwprobe.c -framework CoreFoundation -framework IOKit
./fwprobe                    # first FireWire device on the bus
./fwprobe -g <guid-hex>      # by GUID
```

## What it established

| question | answer |
|---|---|
| Can user space open the device? | **Yes.** `IOFireWireLib` v9 interface, `Open()` OK, no kext, no Reduced Security, no entitlement |
| Do async reads work? | **Yes.** Config ROM reads clean; `"1394"` magic, PreSonus OUI `0x000a92`, model `0x15` |
| Is it really DICE? | **Yes, proven.** The nickname register byte-swaps to `"StudioLive"` |
| Does the isoch API exist? | **Partly proven.** Channel and remote port objects create, `SetTalker` OK; a local port with a DCL program is untested |

The nickname is the load-bearing evidence. The snd-dice register map put the
nickname at global+0x0c, and reading there and swapping the string bytes — the
quirk snd-dice documents — gives exactly `StudioLive`. A wrong register map does
not produce a word.

## The numbers that matter

```
tx[0]  40 audio channels   device -> Mac   (what we capture)
rx[0]  26 audio channels   Mac -> device   (not needed)
clock_select  source 12, 48000 Hz
status        clock LOCKED
version       1.0.12.0
```

**40 capture channels** against the 16 the requirement asks for.

## What is left

`AllocateChannel` returns `kIOReturnNoResources`, which is expected with a
talker and no listener — the IRM has nothing to size the allocation from. The
next step is a **local isoch port driven by a DCL program**, which is both the
real packet-count test and the first piece of the adapter proper.

---

## isoprobe — isochronous receive

```sh
cc -o isoprobe isoprobe.c -framework CoreFoundation -framework IOKit
./isoprobe -s 10
```

**Passed.** A local isoch port driven by a DCL program receives the console's
tx[0] stream:

```
Callbacks: 1251 (80064 packets counted by CallProc)
  isoch header   tag 1, channel 0 (IRM allocated 0)
  packets        352 data, 160 empty, 0 unrecognised
  CIP DBS        40 quadlets per data block (tx[0] reports 40 channels)
  CIP FMT        0x10 = IEC 61883-6 AM824
  CIP FDF        0x02, SFC 2 = 48 kHz
  DBC continuity 511 good steps, 0 breaks
VERDICT: transport PROVEN and callbacks work — 8006 packets/s against ~8000 expected.
```

**This writes to the console.** The remote port's `AllocatePort` puts the IRM's
channel into `tx[0].isoc` and `Start`/`Stop` drive `GLOBAL_ENABLE`. `quiesce()`
clears both from `atexit` and from SIGINT/SIGTERM. Verified afterwards by reading
the console back: streaming off, channel -1.

### Things the adapter must keep

- **Register the callback dispatchers before creating any isoch object.** A local
  port binds its callback path at creation. Registered afterwards, every CallProc
  stayed silent while DMA filled the whole ring.
- **Decide from the decoded buffer, not the callback count.** The first run
  printed "nothing arrived" above a full ring.
- **Ignore the AM824 label.** This console sends `0x00`, not the spec's `0x40`.
  Linux's capture path shifts it out (`be32_to_cpu(q) << 8`) and never checks it.
- **Read the sample rate from `GLOBAL_STATUS`**, never from CIP FDF or
  `clock_select`. On this console both of those say 48 kHz while it runs at
  **44.1 kHz** — see the correction below.

### Correction — the stream is 44.1 kHz

The isoprobe output above prints `SFC 2 = 48 kHz` because that is what the CIP
FDF field literally says. It is wrong. The console is locked to **44.1 kHz**, as
`GLOBAL_STATUS` reported all along; FDF and `clock_select` carry the rate that
was requested, not the one in use. Proven by the data-packet ratio (68.9% =
5512.5/8000, not 75%), by 30 s of wall time yielding 30.0 s of frames only at
44.1 kHz, and by a clock fit sitting at −81,278 ppm from 48 kHz. Found by the
clock fit in `src/slfw.c`, whose sanity clamp had been snapping it to nominal.
