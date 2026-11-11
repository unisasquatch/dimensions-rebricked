# Dimensions Rebricked

An independent PC compatibility layer for the PS4 release of LEGO Dimensions (CUSA01176). It loads the game's own
executable from **your** game files and runs it natively on Windows (and Linux / Steam Deck), replacing the PS4's
system libraries: graphics (GCN shaders translated to Vulkan), audio, input, save data and the Toy Pad.

> Dimensions Rebricked is not affiliated with, sponsored by, or endorsed by the LEGO Group, Warner Bros. Interactive
> Entertainment, TT Games, or Sony Interactive Entertainment. LEGO, LEGO Dimensions, PlayStation and the other names
> mentioned here are trademarks of their respective owners. Game content remains the property of its owners.

## What you need

- Game files from your own, lawfully acquired copy of LEGO Dimensions for PS4, and the rights to any add-on content
  you use. This project contains **no game files and no keys**, and does not tell you where to get them. Ownership
  alone does not necessarily permit bypassing access controls; the law varies by country.
- Windows 10/11 x64 with a Vulkan 1.3 GPU (or Linux / SteamOS).
- A Toy Pad is optional: the port has a virtual Toy Pad (in the pause menu, and as a web page on your local
  network), supports a real Toy Pad over USB, and can take figures from harrysof's
  [LegoToypad](https://github.com/harrysof/LegoToypad).

## Features

- Higher resolutions (1080p, 1440p, 4K, 6K) and upscalers: AMD FSR 1 / FSR 2, NVIDIA DLSS / DLAA.
- FXAA / TAA, VSync modes, borderless and exclusive fullscreen, aspect ratio options.
- Up to 4-player split screen, keyboard and controller support with matching button icons.
- Settings live in the game's own Options menu (Display Settings, Gameplay).
- Save transfer to and from shadPS4, Xenia, Cemu and RPCS3.

## Building

Windows: Visual Studio 2022 Build Tools, LLVM (clang-cl), CMake, Ninja, the Vulkan SDK and SDL3, then
`port\build.cmd`. The installer: `port\installer\make_installer.cmd` (Inno Setup 6). Steam Deck:
`port/deck/build_linux.sh`, then `port/deck/package.sh`.

DLSS uses the NVIDIA DLSS SDK files in `port/third_party/dlss` under NVIDIA's licence there; without a supported GPU
the DLSS modes fall back to FSR 2.

## Setup

The installer asks for your game's `.pkg` files (the 1.24 update; the base game 1.00 is recommended), a package keys
file, and optionally a folder of add-on pack `.pkg` files. More add-ons can be added later from
Start > Dimensions Rebricked > Add DLC packs. Setup is always a fresh install: it carries no saves or settings.

## Contributing

Never upload game files, executables, keys, dumps or saves in issues or pull requests.

## Licence

The project's own code is under the MIT licence (LICENSE). Third-party components keep their own licences; see
THIRD_PARTY_NOTICES.md.
