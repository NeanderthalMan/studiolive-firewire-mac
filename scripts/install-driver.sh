#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Install the StudioLive FireWire Core Audio driver and restart Core Audio.
# Needs root:   sudo scripts/install-driver.sh
# Restarting coreaudiod cuts all audio on the Mac for a second or two.
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
src=$here/build/StudioLiveFW.driver
dst=/Library/Audio/Plug-Ins/HAL/StudioLiveFW.driver
[[ $EUID -eq 0 ]] || { echo "needs root: sudo $0" >&2; exit 1; }
[[ -d $src ]] || { echo "not built: run 'make driver' first" >&2; exit 1; }
rm -rf "$dst"
cp -R "$src" "$dst"
chown -R root:wheel "$dst"
killall coreaudiod   # kickstart is SIP-blocked on macOS 15; launchd relaunches it
echo "installed $dst and restarted Core Audio"
