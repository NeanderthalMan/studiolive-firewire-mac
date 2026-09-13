#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Remove the StudioLive FireWire Core Audio driver.   sudo scripts/uninstall-driver.sh
set -euo pipefail
[[ $EUID -eq 0 ]] || { echo "needs root: sudo $0" >&2; exit 1; }
rm -rf /Library/Audio/Plug-Ins/HAL/StudioLiveFW.driver
killall coreaudiod   # kickstart is SIP-blocked on macOS 15; launchd relaunches it
echo "removed, and restarted Core Audio"
