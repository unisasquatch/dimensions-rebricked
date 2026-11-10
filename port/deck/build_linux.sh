#!/bin/bash
# Builds the Linux port inside the ldport-build distrobox:  distrobox enter ldport-build -- port/deck/build_linux.sh
# SDL3 is built from source once (into port/build-linux/sdl3-install); ldport goes to port/build-linux/ldport.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
port="$(dirname "$here")"
out="$port/build-linux"
mkdir -p "$out"
export CC=clang-16 CXX=clang++-16

SDL_VERSION="${SDL_VERSION:-3.4.18}"
sdl_prefix="$out/sdl3-install"
if [ ! -f "$sdl_prefix/lib/cmake/SDL3/SDL3Config.cmake" ] && [ ! -f "$sdl_prefix/lib/x86_64-linux-gnu/cmake/SDL3/SDL3Config.cmake" ]; then
  cd "$out"
  if [ ! -d "SDL3-$SDL_VERSION" ]; then
    curl -fL -o sdl3.tar.gz "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VERSION/SDL3-$SDL_VERSION.tar.gz"
    tar xzf sdl3.tar.gz
  fi
  cmake -S "SDL3-$SDL_VERSION" -B sdl3-build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$sdl_prefix" \
    -DCMAKE_INSTALL_LIBDIR=lib -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
  cmake --build sdl3-build
  cmake --install sdl3-build
fi

cmake -S "$port" -B "$out/ldport" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_PREFIX_PATH="$sdl_prefix" \
  -DCMAKE_EXE_LINKER_FLAGS="-fuse-ld=lld-16" -DCMAKE_SHARED_LINKER_FLAGS="-fuse-ld=lld-16"
cmake --build "$out/ldport" --target ldport
echo "built $out/ldport/ldport"

# ldpkg (unpacks the PS4 .pkg files; one file, no dependencies). Never with keys built in: install.sh asks for them.
$CXX -O2 -std=c++20 -maes -msse4.1 -pthread -fuse-ld=lld-16 -DLDPKG_NO_BUILTIN_KEYS "$port/tools/ldpkg/ldpkg.cpp" -o "$out/ldpkg"
echo "built $out/ldpkg"
