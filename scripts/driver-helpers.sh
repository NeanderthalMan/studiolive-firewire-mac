#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Sourced by install-driver.sh and uninstall-driver.sh.
#
# Core Audio runs the driver in a helper process of its own, and that helper
# can outlive `killall coreaudiod`: on 2026-09-27 the old one kept streaming
# for a minute after an install, holding the console, and the new driver found
# it busy. So a restart waits for the helpers that were running before it.
#
# SIGTERM, never SIGKILL. The driver catches SIGTERM and gives the FireWire
# channel back before it dies. SIGKILL runs no code at all, and a helper killed
# mid-stream can wedge the Mac's FireWire stack until a reboot. Waiting a
# minute is cheap next to a reboot, so a helper that will not go is reported
# and left alone.

driver_helpers() { pgrep -f 'Core Audio Driver \(StudioLiveFW\.driver\)' || true; }

restart_core_audio() {
    local old
    old=$(driver_helpers)
    killall coreaudiod   # kickstart is SIP-blocked on macOS 15; launchd relaunches it
    [[ -z $old ]] && return 0
    kill -TERM $old 2>/dev/null || true
    for _ in $(seq 60); do
        local left=""
        for p in $old; do kill -0 "$p" 2>/dev/null && left+=" $p"; done
        [[ -z $left ]] && return 0
        sleep 1
    done
    echo "the old driver helper (pid$left) is still running after 60 s; not killing it, because" >&2
    echo "SIGKILL mid-stream can wedge FireWire until reboot. The new driver will open the" >&2
    echo "console by itself once that helper exits." >&2
    return 0
}
