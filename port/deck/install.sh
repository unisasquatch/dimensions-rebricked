#!/bin/bash
# Installs LEGO Dimensions on a Steam Deck (or another Linux PC) from this unpacked package.
#
#   ./install.sh [--dir <install folder>] [--gamedata <extracted folder> | --pkgs <folder of .pkg files>] [--inplace]
#                [--keys <keys file>] [--no-udev] [--import-shadps4 <shadPS4 folder | auto>] [--no-shadps4]
#                [--legotoypad | --no-legotoypad]
#
# --gamedata: the unpacked game data ("extracted" folder: elf/eboot.elf, CUSA01176/uroot, CUSA01176-base/uroot,
#   addcont/). Without it a folder picker opens (Desktop Mode). It is copied into the install folder (~50 GB)
#   unless --inplace, which leaves it where it is (game.txt names it).
# --pkgs: the PS4 .pkg files themselves (fake-signed, as used with shadPS4: the 1.24 update, and the base game 1.00
#   and add-ons if you have them), a folder of them or one file. ldpkg unpacks them into the install folder (~50 GB).
#   A folder picked in the folder picker that holds .pkg files is taken the same way. Later, more add-ons (DLC):
#   <install>/ldpkg --dlc --keys <install>/data/pkgkeys.bin -o <install>/game <folder of add-on .pkg files>
# --keys: the package keys file .pkg files need (the two fake-package RSA keys; not part of this package; see
#   keys.example.inc in the source). Asked for when missing; kept as <install>/data/pkgkeys.bin for adding DLC later.
# Shortcuts (menu entry, desktop icon, Steam): made at the end; to make them again, <install>/create-shortcuts.sh.
# --import-shadps4: bring in LEGO Dimensions saves from the shadPS4 emulator (its user folder, savedata folder or one
#   save folder; "auto" looks in ~/.local/share/shadPS4 and the Flatpak's folder). The saves already installed are
#   copied to data/savedata-backups/<date-time>/ first. Without the option, install.sh offers it when it finds
#   shadPS4 saves; --no-shadps4 skips that question. Later: ldport --import-shadps4 <folder> --data <install>/data
# Saves from Xenia, Cemu or RPCS3 (and back out to any emulator): save-transfer.sh in the install folder, also in the
#   menu as "LEGO Dimensions save transfer".
# --legotoypad: download harrysof's LegoToypad app (MIT licence, https://github.com/harrysof/LegoToypad; about 33 MB)
#   into <install>/LegoToypad and take the game's figures from it (Options > Gameplay > Toy Pad switches
#   back). It is a Windows program: the game starts it through Wine when Wine is installed; otherwise add
#   LegoToypad.exe to Steam as a non-Steam game and run it with Proton. Without the option, install.sh asks;
#   --no-legotoypad skips the question.
# The Toy Pad udev rule (lets the game use a physical Toy Pad) needs sudo once; --no-udev skips it.
set -euo pipefail
pkg="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
dest="$HOME/Games/LEGO Dimensions"
gamedata="" pkgs="" keys="" inplace=0 udev=1 shadps4="" ask_shadps4=1 legotoypad=ask
# LegoToypad's release (to move to a newer one: its URL, and the asset's SHA-256 "digest" from the GitHub API).
legotoypad_url="https://github.com/harrysof/LegoToypad/releases/download/v1.7/LegoToypad_app_Version_1.7.exe"
legotoypad_sha256="ed2ff917b0ca5b632389112ef7bec75cfadddec2cc401a579ecefdcc06bc8831"
while [ $# -gt 0 ]; do
  case "$1" in
    --dir) dest="$2"; shift ;;
    --gamedata) gamedata="$2"; shift ;;
    --pkgs) pkgs="$2"; shift ;;
    --inplace) inplace=1 ;;
    --keys) keys="$2"; shift ;;
    --no-udev) udev=0 ;;
    --import-shadps4) shadps4="$2"; shift ;;
    --no-shadps4) ask_shadps4=0 ;;
    --legotoypad) legotoypad=1 ;;
    --no-legotoypad) legotoypad=0 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
  shift
done

say() { echo "== $*"; }
if [ -z "$gamedata" ] && [ -z "$pkgs" ]; then
  if command -v kdialog >/dev/null && [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]; then
    gamedata="$(kdialog --title "LEGO Dimensions: game data folder (holds elf, CUSA01176, ...) or the folder with the .pkg files" --getexistingdirectory "$HOME")" || exit 1
  else
    read -r -p "Game data folder (holds elf/, CUSA01176/ ...) or folder with the .pkg files: " gamedata
  fi
  gamedata="${gamedata%/}"
  # A folder of .pkg files (and no unpacked data) means the PS4 packages themselves.
  if [ ! -f "$gamedata/elf/eboot.elf" ] && compgen -G "$gamedata/*.[pP][kK][gG]" >/dev/null; then pkgs="$gamedata" gamedata=""; fi
fi
if [ -n "$pkgs" ]; then
  pkgs="${pkgs%/}"
  if [ -z "$keys" ]; then
    if command -v kdialog >/dev/null && [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]; then
      keys="$(kdialog --title "LEGO Dimensions: the package keys file" --getopenfilename "$HOME")" || exit 1
    else
      read -r -p "Package keys file (the two fake-package RSA keys): " keys
    fi
  fi
  "$pkg/ldpkg" --keys "$keys" || { echo "those keys don't work: $keys" >&2; exit 1; }
  say "checking the packages in $pkgs"
  "$pkg/ldpkg" --keys "$keys" --info "$pkgs" || { echo "these packages can't be used yet: the fake-signed 1.24 update package is needed" >&2; exit 1; }
else
  gamedata="${gamedata%/}"
  [ -f "$gamedata/elf/eboot.elf" ] || { echo "elf/eboot.elf not found in $gamedata" >&2; exit 1; }
  [ -d "$gamedata/CUSA01176/uroot" ] || { echo "CUSA01176/uroot (the 1.24 update) not found in $gamedata" >&2; exit 1; }
fi

say "installing to $dest"
mkdir -p "$dest/data"
cp -f "$pkg/ldport" "$pkg/lego-dimensions.sh" "$pkg/save-transfer.sh" "$pkg/imports.tsv" "$pkg/icon.png" "$dest/"
cp -f "$pkg/ldpkg" "$pkg/create-shortcuts.sh" "$dest/"
chmod +x "$dest/ldpkg" "$dest/create-shortcuts.sh"
cp -f "$pkg"/lib*.so* "$dest/"
cp -rf "$pkg/fsr1" "$dest/"
chmod +x "$dest/ldport" "$dest/lego-dimensions.sh" "$dest/save-transfer.sh"
[ -f "$dest/data/settings.ini" ] || cp "$pkg/data/settings.ini" "$dest/data/"
[ -d "$pkg/data/shadercache" ] && cp -rf "$pkg/data/shadercache" "$dest/data/"
[ -n "$keys" ] && cp -f "$keys" "$dest/data/pkgkeys.bin"

# shadPS4 saves: offered when its default folders hold LEGO Dimensions saves (GAME01 / V2GAME01 in a save folder).
if [ -z "$shadps4" ] && [ $ask_shadps4 = 1 ]; then
  for cand in "${XDG_DATA_HOME:-$HOME/.local/share}/shadPS4" "$HOME/.var/app/net.shadps4.shadPS4/data/shadPS4"; do
    [ -d "$cand" ] && find "$cand" -maxdepth 7 \( -name GAME01 -o -name V2GAME01 \) -print -quit 2>/dev/null | grep -q . || continue
    q="Import the LEGO Dimensions saves from shadPS4 ($cand)? The saves already installed are backed up first."
    if command -v kdialog >/dev/null && [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]; then
      kdialog --title "LEGO Dimensions" --yesno "$q" && shadps4="$cand"
    elif [ -t 0 ]; then
      read -r -p "$q [y/N] " yn
      case "$yn" in [yY]*) shadps4="$cand" ;; esac
    fi
    break
  done
fi
if [ -n "$shadps4" ]; then
  say "importing shadPS4 saves from $shadps4"
  LD_LIBRARY_PATH="$dest${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" "$dest/ldport" --import-shadps4 "$shadps4" --data "$dest/data" ||
    say "the shadPS4 import failed (see above); the saves from before are unchanged or in $dest/data/savedata-backups"
fi

# LegoToypad: downloaded only when asked for, checked against the release's SHA-256.
set_setting() {  # key value: replaces the key's line in data/settings.ini, or adds one
  local f="$dest/data/settings.ini"
  if grep -q "^$1=" "$f" 2>/dev/null; then sed -i "s|^$1=.*|$1=$2|" "$f"; else echo "$1=$2" >> "$f"; fi
}
if [ "$legotoypad" = ask ]; then
  legotoypad=0
  q="Download the LegoToypad app by harrysof (about 33 MB, from GitHub) and use it as the Toy Pad? The game's own virtual Toy Pad stays available in Options > Display Settings > Toy Pad."
  if command -v kdialog >/dev/null && [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]; then
    kdialog --title "LEGO Dimensions" --yesno "$q" && legotoypad=1
  elif [ -t 0 ]; then
    read -r -p "$q [y/N] " yn
    case "$yn" in [yY]*) legotoypad=1 ;; esac
  fi
fi
if [ "$legotoypad" = 1 ]; then
  say "downloading LegoToypad"
  mkdir -p "$dest/LegoToypad"
  tmp="$dest/LegoToypad/LegoToypad.exe.part"
  if { command -v curl >/dev/null && curl -fL --retry 2 -o "$tmp" "$legotoypad_url"; } ||
     { command -v wget >/dev/null && wget -O "$tmp" "$legotoypad_url"; }; then
    if [ "$(sha256sum "$tmp" | cut -d' ' -f1)" = "$legotoypad_sha256" ]; then
      mv -f "$tmp" "$dest/LegoToypad/LegoToypad.exe"
      set_setting toypad 2
      if command -v wine >/dev/null; then
        set_setting legotoypad_start 1
        say "LegoToypad installed: the game starts it (through Wine) and takes its figures from it"
      else
        set_setting legotoypad_start 0
        say "LegoToypad installed in $dest/LegoToypad. Wine isn't installed: add LegoToypad.exe to Steam as a"
        say "  non-Steam game, set it to run with Proton, and start it alongside the game"
      fi
    else
      rm -f "$tmp"
      say "LegoToypad's download didn't match its checksum: not installed (the game keeps its own Toy Pad)"
    fi
  else
    rm -f "$tmp"
    say "LegoToypad couldn't be downloaded (the game keeps its own Toy Pad)"
  fi
fi

if [ -n "$pkgs" ]; then
  # ldpkg skips files already unpacked, so running install.sh again after an interruption carries on.
  rm -f "$dest/game.txt"
  say "unpacking the PKG files into $dest/game (about 50 GB; this takes a while)"
  "$pkg/ldpkg" --keys "$dest/data/pkgkeys.bin" --layout install --log "$dest/data/ldpkg.log" -o "$dest/game" "$pkgs" ||
    { echo "unpacking failed (log: $dest/data/ldpkg.log); run install.sh again to carry on" >&2; exit 1; }
elif [ $inplace = 1 ]; then
  rm -rf "$dest/game"
  echo "$gamedata" > "$dest/game.txt"
  say "using the game data where it is: $gamedata"
else
  rm -f "$dest/game.txt"
  say "copying the game data (about 50 GB; this takes a while)"
  mkdir -p "$dest/game"
  cp -f "$gamedata/elf/eboot.elf" "$dest/game/"
  copy_dir() { [ -d "$1" ] && { mkdir -p "$2"; cp -r "$1/." "$2/"; say "  $(basename "$2") done"; } || true; }
  copy_dir "$gamedata/CUSA01176/uroot" "$dest/game/update"
  copy_dir "$gamedata/CUSA01176-base/uroot" "$dest/game/base"
  copy_dir "$gamedata/addcont" "$dest/game/addcont"
fi

# Menu entries, desktop icon and a Steam shortcut (create-shortcuts.sh in the install folder makes them again later).
"$dest/create-shortcuts.sh" || say "shortcuts not made (see above); run $dest/create-shortcuts.sh later"

if [ $udev = 1 ]; then
  rule=/etc/udev/rules.d/99-lego-dimensions-toypad.rules
  if [ ! -f "$rule" ]; then
    say "installing the Toy Pad rule (needs your sudo password; --no-udev skips it)"
    if sudo install -m 644 "$pkg/99-lego-dimensions-toypad.rules" "$rule"; then
      sudo udevadm control --reload-rules && sudo udevadm trigger || true
    else
      say "skipped: a physical Toy Pad won't be usable until this rule is installed"
    fi
  fi
fi
say "done. Start it from Steam (Game Mode) or $dest/lego-dimensions.sh"
