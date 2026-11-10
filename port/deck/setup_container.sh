#!/bin/bash
# Run on the Steam Deck (Desktop Mode, as the deck user): makes the "ldport-build" distrobox the Linux build runs in.
# Debian 12 keeps glibc (2.36) older than SteamOS's, so what it builds runs on the Deck itself, outside the box.
set -euo pipefail
if ! distrobox list 2>/dev/null | grep -q ldport-build; then
  distrobox create --yes --name ldport-build --image docker.io/library/debian:12
fi
distrobox enter ldport-build -- bash -c '
set -e
sudo apt-get update -qq
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
  build-essential clang-16 lld-16 cmake ninja-build pkg-config git ca-certificates curl \
  libvulkan-dev glslang-dev libshaderc-dev spirv-tools \
  libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxtst-dev libxfixes-dev libxkbcommon-dev \
  libwayland-dev wayland-protocols libdecor-0-dev libegl-dev libgl-dev libgbm-dev libdrm-dev \
  libasound2-dev libpulse-dev libpipewire-0.3-dev libudev-dev libdbus-1-dev libibus-1.0-dev libusb-1.0-0-dev
echo "container ready"
'
