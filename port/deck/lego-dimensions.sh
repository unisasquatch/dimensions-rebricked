#!/bin/bash
# LEGO Dimensions (Steam Deck / Linux): starts ldport from the installed folder.
#   <install>/ldport, libSDL3.so.0, libshaderc_shared.so.1, imports.tsv, fsr1/
#   <install>/data          settings, saves, Toy Pad figures, shader cache, ldport.log
#   game data, either copied:  <install>/game/eboot.elf, game/update, game/base, game/addcont
#   or left where it is, named by <install>/game.txt (one line: an "extracted" folder holding
#   elf/eboot.elf, CUSA01176/uroot, CUSA01176-base/uroot, addcont)
# Extra arguments go to ldport. The virtual Toy Pad page is on unless LDPORT_TOYPAD_WEB is set already.
# lego-dimensions.sh --create-shortcuts [--no-desktop] [--no-steam]: makes the menu entry, desktop icon and Steam
# shortcut again (create-shortcuts.sh).
root="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
if [ "${1:-}" = --create-shortcuts ]; then shift; exec "$root/create-shortcuts.sh" "$@"; fi
data="$root/data"
eboot="$root/game/eboot.elf" update="$root/game/update" base="$root/game/base" addcont="$root/game/addcont"
if [ -s "$root/game.txt" ]; then
  dir="$(head -n1 "$root/game.txt" | tr -d '\r')"
  dir="${dir%/}"
  eboot="$dir/elf/eboot.elf" update="$dir/CUSA01176/uroot" base="$dir/CUSA01176-base/uroot" addcont="$dir/addcont"
fi
if [ ! -f "$eboot" ] || [ ! -d "$update" ]; then
  msg="The game data is missing ($eboot). Run install.sh again and point it at the game data folder."
  command -v kdialog >/dev/null && kdialog --error "$msg" || echo "$msg" >&2
  exit 1
fi
mkdir -p "$data"
args=("$eboot" --run --names "$root/imports.tsv" --app0 "$update")
[ -d "$base" ] && args+=(--app0 "$base")
args+=(--data "$data")
[ -d "$addcont" ] && args+=(--addcont "$addcont")
export LDPORT_TOYPAD_WEB="${LDPORT_TOYPAD_WEB:-1}"
export LD_LIBRARY_PATH="$root${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
[ -f "$data/ldport.log" ] && mv -f "$data/ldport.log" "$data/ldport.prev.log"
cd "$root"
exec "$root/ldport" "${args[@]}" "$@" </dev/null >"$data/ldport.log" 2>&1
