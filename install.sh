#!/bin/bash
# Install clip2output WITHOUT crashing a running KWin.
#
# Overwriting a .so in place with `cp` corrupts the mapping of any process that already has it
# dlopen'd - which crashed KWin once during development. Write a new file and rename over the
# old one instead: rename replaces the directory entry and leaves the old inode mapped until
# the process lets go of it.
set -euo pipefail
DEST=/usr/lib/x86_64-linux-gnu/qt6/plugins/kwin/effects/plugins
SRC="$(dirname "$0")/build/clip2output.so"
[ -f "$SRC" ] || { echo "build it first: cmake -S . -B build && cmake --build build -j4" >&2; exit 1; }
sudo mkdir -p "$DEST"
sudo cp "$SRC" "$DEST/.clip2output.so.new"
sudo mv -f "$DEST/.clip2output.so.new" "$DEST/clip2output.so"   # atomic; never corrupts a mapping
echo "installed $DEST/clip2output.so"
echo
echo "The running KWin keeps the version it already dlopen'd - a new build only takes effect"
echo "after a re-login. Load/unload the CURRENT one with:"
echo "  qdbus6 org.kde.KWin /Effects org.kde.kwin.Effects.loadEffect   clip2output"
echo "  qdbus6 org.kde.KWin /Effects org.kde.kwin.Effects.unloadEffect clip2output"
