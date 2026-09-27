# Install

[← Back to the README](../README.md)

## Requirements

- An **Apple Silicon Mac** with a Thunderbolt 3 or 4 (USB-C) port.
- **Both Apple adapters**, A1463 and A1790, plugged **straight into the Mac**.
  - **No hub or dock** in between: a USB-C hub does not carry Thunderbolt.
  - Thunderbolt 2 uses the **Mini DisplayPort** connector shape, so Mini
    DisplayPort *video* adapters and cables fit, but they carry no PCIe at all.
    If the console does not appear, check that nothing in the chain is a video
    part.
- **Xcode Command Line Tools**: `xcode-select --install`.
- An administrator account, to install the driver.

## Check the adapter chain first

To check the hardware before building anything, run
`scripts/probe-firewire.sh baseline` with the adapters unplugged, then
`scripts/probe-firewire.sh check` with them plugged in. It records what it finds
under `var/`, which is gitignored because it includes serial numbers.

## Build and install

```sh
make                                          # the driver, the recorder, the checks and the menu bar app
make test                                     # unit tests; no console or sudo needed
build/halcheck build/StudioLiveFW.driver 20   # drives the plug-in in a plain process; console must be on the bus
sudo scripts/install-driver.sh                # copies it to /Library/Audio/Plug-Ins/HAL and restarts Core Audio
build/cacheck 10                              # records 10 s through Core Audio; must say PASSED
```

There are no prebuilt downloads: build it yourself. The driver is ad-hoc
signed. That is fine for a plug-in built and installed on the same Mac, but
macOS would block a downloaded ad-hoc plug-in, and there is no Developer ID to
sign one with.

Restarting Core Audio cuts all audio on the Mac for a second or two. The
install script waits for the previous driver to let go of the console first.

`sudo` needs an interactive terminal. From a tool without one, run the script
through
`osascript -e 'do shell script "/full/path/to/scripts/install-driver.sh" with administrator privileges'`.

Core Audio then lists **StudioLive FireWire**:
- One input per console stream channel, named as the console names them: on
  the 24.4.2AI, CH1–CH24 and AUX1–AUX16.
- A stereo output named **Not connected 1** and **Not connected 2**, which is
  exactly what it says. See [Recording in Studio One](studio-one.md) for why
  it exists.

## The menu bar icon (optional)

```sh
scripts/install-menu.sh        # copies build/StudioLiveMenu.app to ~/Applications and starts it
```

It shows at a glance whether the console is there and streaming. See
[Tools](tools.md#the-menu-bar-icon).

## Remove it

```sh
sudo scripts/uninstall-driver.sh
```
