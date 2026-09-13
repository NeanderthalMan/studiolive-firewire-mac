#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Is there a Dante device on this network?
#
# Dante devices advertise themselves over mDNS/Bonjour, so this is a definitive,
# free answer to "is the card in the console a Dante card" — no software licence
# and no disassembly required.
#
# Before running: plug ONE of the console's Ethernet ports into the same switch
# or router as this Mac (the Dante card's two ports are primary/secondary
# redundancy, either will do), and make sure the console is powered on.
#
# A silent result with the console unplugged means nothing at all.
set -uo pipefail

echo "Mac's interfaces with a link:"
for i in $(networksetup -listallhardwareports 2>/dev/null | awk '/Device:/{print $2}'); do
  s=$(ifconfig "$i" 2>/dev/null | awk '/status:/{print $2}')
  a=$(ipconfig getifaddr "$i" 2>/dev/null)
  [[ ${s:-} == active ]] && echo "  $i  ${a:-no address}"
done
echo

echo "Browsing for Dante services (about 25s)..."
found=0
for t in _netaudio-arc._udp _netaudio-cmc._udp _netaudio-dbc._udp _netaudio-chan._udp; do
  # dns-sd has no timeout flag; alarm(2) survives exec, so this bounds it.
  out=$(perl -e 'alarm 6; exec "dns-sd", "-B", $ARGV[0], "local."' "$t" 2>/dev/null | awk 'NR>4')
  if [[ -n ${out//[[:space:]]/} ]]; then
    found=1
    echo "  $t"
    echo "$out" | sed 's/^/    /'
  fi
done

echo
if [[ $found -eq 1 ]]; then
  cat <<'YES'
DANTE DEVICE FOUND.

The card in the console is a Dante card, and this project is mostly over:
install Dante Controller (free) and Dante Virtual Soundcard ($49.99, Apple
Silicon native), route the console's channels to DVS, and Studio One sees them
as a Core Audio device. No FireWire, no bridge machine, no driver.
YES
else
  cat <<'NO'
Nothing found.

That is only meaningful if the console is powered on and its Ethernet is
plugged into the same network as this Mac. If it is not, this result says
nothing — go plug it in and run again.

If it IS plugged in and powered and still nothing appears, the card is probably
the stock FireWire card (2x FireWire 800 + ONE Ethernet, no S/PDIF) rather than
the SL-Dante-MIX (2x FireWire 800 + TWO Ethernet + S/PDIF out).
NO
fi
