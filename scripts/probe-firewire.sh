#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Does the FireWire OHCI controller in the Apple TB→FW800 adapter reach this Mac?
#
# The chain that can work is:
#   console ─FW800─► Apple A1463 (Thunderbolt→FireWire 800)
#                    └─► Apple A1790 (Thunderbolt 3 USB-C → Thunderbolt 2)
#                        └─► a Thunderbolt port on the Mac, with NO hub in between
#
# Success here is an UNCLAIMED PCIe device with class code 0x0C0010 (serial bus
# controller / IEEE 1394 / OHCI). It is not a working audio interface: Apple
# Silicon ships no FireWire driver code, so nothing will bind to it. Proving the
# device is reachable is the whole point — it separates "the hardware path is
# intact and only software is missing" from "no amount of software helps".
#
# Usage:
#   scripts/probe-firewire.sh baseline    # with the chain UNPLUGGED
#   scripts/probe-firewire.sh check       # with the chain plugged in
#
# Records to var/, which is gitignored — these are machine facts, not source.
set -u

here=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
out=$here/var
mkdir -p "$out"
mode=${1:-check}

case $mode in
  baseline|check) ;;
  *) echo "usage: $(basename "$0") [baseline|check]" >&2; exit 2 ;;
esac

snap=$out/$mode.txt
{
  echo "# $mode — $(date '+%Y-%m-%d %H:%M:%S %Z') — $(sw_vers -productVersion) $(uname -m)"
  echo
  echo "## thunderbolt"
  system_profiler SPThunderboltDataType 2>/dev/null
  echo
  echo "## pci"
  system_profiler SPPCIDataType 2>/dev/null
  echo
  echo "## firewire"
  system_profiler SPFireWireDataType 2>/dev/null
  echo
  echo "## pci nubs in the io registry"
  ioreg -r -c IOPCIDevice -l 2>/dev/null
} > "$snap"

echo "→ $snap"
echo

# --- the three things worth saying out loud -------------------------------

link=$(system_profiler SPThunderboltDataType 2>/dev/null | grep -c "^[[:space:]]*Status: No device connected")
ports=$(system_profiler SPThunderboltDataType 2>/dev/null | grep -c "^[[:space:]]*Status:")
if [[ $ports -gt 0 && $link -eq $ports ]]; then
  echo "thunderbolt: NO device on any of the $ports ports."
  [[ $mode == check ]] && cat <<'NOTE'

  The controller cannot see a link partner, which is upstream of every driver
  question. Check, in this order:
    - is a *video* adapter in the path? Thunderbolt 2 uses the Mini DisplayPort
      connector shape, so mDP→DP→USB-C cables fit and carry no PCIe at all;
    - is it going through a USB-C hub? A hub does not tunnel Thunderbolt;
    - is the A1790 the genuine Apple part, plugged straight into the Mac?
NOTE
else
  echo "thunderbolt: something is linked — $((ports - link)) of $ports ports."
  system_profiler SPThunderboltDataType 2>/dev/null |
    grep -A3 -B1 "^[[:space:]]*Status: Device connected" | sed 's/^/  /'
fi
echo

# class-code lives in the registry as little-endian data: 0x000C0010 → <10000c00>
if ioreg -r -c IOPCIDevice -l 2>/dev/null | grep -qi -e '10000c00' -e '"class-code".*0c0010'; then
  echo "PCIe: FOUND a 1394 OHCI controller (class 0x0C0010). This is the result we want."
  ioreg -r -c IOPCIDevice -l 2>/dev/null |
    grep -i -B12 -e '10000c00' | grep -i -e 'IOName' -e 'vendor-id' -e 'device-id' -e 'IOClass' |
    sed 's/^/  /'
else
  echo "PCIe: no 1394 OHCI controller on the bus."
fi
echo

if system_profiler SPFireWireDataType 2>/dev/null | grep -q .; then
  echo "FireWire: a bus is being reported — unexpected on Apple Silicon, worth a close look."
else
  echo "FireWire: no bus, as expected — there is no FireWire driver code on this Mac."
fi

if [[ -f $out/baseline.txt && $mode == check ]]; then
  echo
  echo "diff against baseline (new lines only):"
  diff <(grep -v '^# ' "$out/baseline.txt") <(grep -v '^# ' "$snap") |
    grep '^>' | grep -v -i -e 'timestamp' -e 'uptime' | head -40 | sed 's/^/  /'
fi
