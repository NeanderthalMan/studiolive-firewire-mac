# studiolive-firewire-mac

A user-space Core Audio driver that lets an **Apple Silicon Mac record a PreSonus
StudioLive AI console over FireWire**, which PreSonus has said it will never
support.

It needs no kernel extension, no Reduced Security mode and no special
entitlement. It uses Apple's own FireWire stack, which still ships on Apple
Silicon, and makes the console an ordinary Core Audio input device that any
Core Audio app can use.

> Not affiliated with, endorsed by, or supported by PreSonus. "PreSonus",
> "StudioLive" and "Studio One" are trademarks of their owners. This talks to
> your console over FireWire and writes to its streaming registers. Use it at
> your own risk.

## What has been tested

One setup, and only this:

| | |
|---|---|
| Console | StudioLive **24.4.2AI** |
| Mac | MacBook Pro, **M4 Pro**, macOS **15.1** |
| Adapters | Apple Thunderbolt to FireWire (**A1463**) → Apple Thunderbolt 3 to Thunderbolt 2 (**A1790**) → the Mac |
| DAW | Studio One **5.5.2** |

On that setup, all 40 channels record through Core Audio with no gaps, and
Studio One records from them.

The other AI mixers, other Macs, other macOS versions and 48 kHz are untested.
They speak the same protocol, so they may work. Reports either way are welcome.

## Quick start

You need Xcode Command Line Tools (`xcode-select --install`), both Apple
adapters plugged straight into the Mac, and an administrator account.

```sh
make                             # build the driver, the tools and the menu bar app
make test                        # unit tests; no console or sudo needed
sudo scripts/install-driver.sh   # install the driver and restart Core Audio
scripts/install-menu.sh          # optional: the menu bar status icon
```

Core Audio then lists **StudioLive FireWire**, with one input per console
channel, named as the console names them. In Studio One, pick it as the audio
device and record.

## Documentation

| | |
|---|---|
| [Install](docs/install.md) | Requirements, checking the adapter chain, building, installing and removing |
| [Recording in Studio One](docs/studio-one.md) | Recording from the console, and hearing playback through an aggregate device |
| [Troubleshooting](docs/troubleshooting.md) | No sound, clicks, sample-rate changes, and known issues |
| [Tools](docs/tools.md) | The menu bar icon, the recorder, the checks and `mkaggregate` |
| [How it works](docs/how-it-works.md) | Why this is possible, and how the driver is built |

## Alternatives

If you would rather not depend on any of this: a StudioLive **SL-Dante-MIX**
option card with Dante Virtual Soundcard is maintained by someone else, and the
console's DB-25 direct outs into a multichannel analog interface need no
software at all.

## License

GPL-2.0-or-later. See [`LICENSE`](LICENSE). No code was copied from other
projects. The DICE register map, stream layout and AM824 handling were learned
by reading [FFADO](http://www.ffado.org/) and the Linux kernel's `snd-dice`
driver, both GPL, and then checked against the hardware.
