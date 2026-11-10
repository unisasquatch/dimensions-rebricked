#!/bin/bash
# Assembles the Steam Deck package (after build_linux.sh): port/deck/out/LEGO-Dimensions-Deck/ and a .tar.gz of it.
# A fresh install: no saves, Toy Pad figures or keys from the PC it is built on (settings-deck.ini is the default).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
port="$(dirname "$here")"
build="$port/build-linux"
out="$here/out/LEGO-Dimensions-Deck"
rm -rf "$out"
mkdir -p "$out/data"
cp "$build/ldport/ldport" "$out/"
strip --strip-debug "$out/ldport" 2>/dev/null || true
cp -P "$build"/sdl3-install/lib/libSDL3.so* "$out/"
# shaderc (GLSL -> SPIR-V at run time) is not on SteamOS: ship the container's copy.
for lib in $(ldd "$out/ldport" | awk '/libshaderc|libglslang|libSPIRV/ {print $3}'); do cp -L "$lib" "$out/"; done
cp "$here/lego-dimensions.sh" "$here/install.sh" "$here/save-transfer.sh" "$here/99-lego-dimensions-toypad.rules" "$out/"
cp "$build/ldpkg" "$here/create-shortcuts.sh" "$out/"
strip "$out/ldpkg" 2>/dev/null || true
cp "$port/installer/icon.png" "$out/" 2>/dev/null || true
cp "$port/../docs/imports.tsv" "$out/"
cp -r "$port/third_party/fsr1" "$out/fsr1"
cp "$here/settings-deck.ini" "$out/data/settings.ini"
chmod +x "$out/ldport" "$out/lego-dimensions.sh" "$out/install.sh" "$out/save-transfer.sh" "$out/ldpkg" "$out/create-shortcuts.sh"
tar -C "$here/out" -czf "$here/out/LEGO-Dimensions-Deck.tar.gz" LEGO-Dimensions-Deck
echo "packaged $here/out/LEGO-Dimensions-Deck.tar.gz"
