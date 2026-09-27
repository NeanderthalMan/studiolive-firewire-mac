#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Install the StudioLive menu bar app into ~/Applications and start it.
# No root needed. "Open at Login" in its menu makes it start with the Mac.
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
src=$here/build/StudioLiveMenu.app
dst=$HOME/Applications/StudioLiveMenu.app
[[ -d $src ]] || { echo "not built: run 'make menu' first" >&2; exit 1; }
osascript -e 'tell application id "n12n.presonus-adapter.menu" to quit' 2>/dev/null || true
mkdir -p "$HOME/Applications"
rm -rf "$dst"
cp -R "$src" "$dst"
open "$dst"
echo "installed $dst and started it"
