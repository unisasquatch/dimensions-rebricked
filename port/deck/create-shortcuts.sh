#!/bin/bash
# Makes LEGO Dimensions' shortcuts for the folder this script is in (install.sh runs it; run it again any time they
# are gone or the install folder has moved):
#   - the application-menu entries (~/.local/share/applications/lego-dimensions*.desktop)
#   - a desktop icon (~/Desktop), for Desktop Mode
#   - a Steam shortcut (non-Steam game), so it shows up in Game Mode
#
#   ./create-shortcuts.sh [--no-desktop] [--no-steam]      (also: ./lego-dimensions.sh --create-shortcuts)
#
# Steam: on SteamOS through steamos-add-to-steam; elsewhere (best effort, not tested off SteamOS) by handing the entry
# to a running Steam through the same steam://addnonsteamgame link that steamos-add-to-steam uses. Restart Steam to
# see a new shortcut; if one is already there, remove the old one in Steam first or you get two.
set -euo pipefail
root="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
desktop=1 steam=1
for a in "$@"; do
  case "$a" in
    --no-desktop) desktop=0 ;;
    --no-steam) steam=0 ;;
    *) echo "unknown option $a" >&2; exit 2 ;;
  esac
done
say() { echo "== $*"; }
[ -x "$root/lego-dimensions.sh" ] || { echo "lego-dimensions.sh is not in $root" >&2; exit 1; }

apps="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
mkdir -p "$apps"
entry="$apps/lego-dimensions.desktop"
cat > "$entry" <<EOF
[Desktop Entry]
Type=Application
Name=Dimensions Rebricked
Comment=LEGO Dimensions for PC (unofficial)
Exec="$root/lego-dimensions.sh"
Path=$root
Icon=$root/icon.png
Terminal=false
Categories=Game;
EOF
say "menu entry: $entry"
if [ -x "$root/save-transfer.sh" ]; then
  cat > "$apps/lego-dimensions-save-transfer.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Dimensions Rebricked save transfer
Comment=Bring saves in from shadPS4, Xenia, Cemu or RPCS3, or send them out
Exec="$root/save-transfer.sh"
Path=$root
Icon=$root/icon.png
Terminal=true
Categories=Game;
EOF
  say "menu entry: $apps/lego-dimensions-save-transfer.desktop"
fi
command -v update-desktop-database >/dev/null && update-desktop-database "$apps" >/dev/null 2>&1 || true

if [ $desktop = 1 ]; then
  dir="$(command -v xdg-user-dir >/dev/null && xdg-user-dir DESKTOP 2>/dev/null || echo "$HOME/Desktop")"
  if [ -d "$dir" ]; then
    cp -f "$entry" "$dir/lego-dimensions.desktop"
    chmod +x "$dir/lego-dimensions.desktop"
    command -v gio >/dev/null && gio set "$dir/lego-dimensions.desktop" metadata::trusted true 2>/dev/null || true
    say "desktop icon: $dir/lego-dimensions.desktop"
  fi
fi

if [ $steam = 1 ]; then
  if command -v steamos-add-to-steam >/dev/null; then
    steamos-add-to-steam "$entry" >/dev/null 2>&1 && say "added to Steam (restart Steam to see it)" ||
      say "Steam shortcut not added (is Steam running?). Run this again with Steam open."
  elif command -v steam >/dev/null && pgrep -x steam >/dev/null; then
    # What steamos-add-to-steam does: hand Steam the .desktop file through its addnonsteamgame link.
    steam "steam://addnonsteamgame/$(python3 -c 'import sys, urllib.parse; print(urllib.parse.quote(sys.argv[1], safe=""))' "$entry" 2>/dev/null || echo "$entry")" >/dev/null 2>&1 &
    say "asked Steam to add it as a non-Steam game (restart Steam to see it; if it isn't there: Games > Add a Non-Steam Game)"
  else
    say "no Steam shortcut: Steam isn't running here. In Steam: Games > Add a Non-Steam Game > Dimensions Rebricked"
  fi
fi
