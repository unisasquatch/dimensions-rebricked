#!/bin/bash
# LEGO Dimensions save transfer: brings saves in from shadPS4, Xenia, Cemu or RPCS3, or sends the port's out to them.
# Interactive; opened in a terminal window when started from the menu. Extra arguments go to
# ldport --save-transfer (e.g. --list, or --from <folder> --to <folder> --format <platform>).
root="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
if [ ! -t 0 ] && [ $# -eq 0 ]; then
  for term in konsole gnome-terminal xterm; do
    command -v "$term" >/dev/null || continue
    case "$term" in
      gnome-terminal) exec gnome-terminal -- "$0" --in-terminal ;;
      *) exec "$term" -e "$0" --in-terminal ;;
    esac
  done
fi
[ "${1:-}" = --in-terminal ] && { shift; pause=1; }
export LD_LIBRARY_PATH="$root${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
mkdir -p "$root/data"
"$root/ldport" --save-transfer --data "$root/data" "$@"
rc=$?
[ -n "${pause:-}" ] && read -r -p "Press Enter to close. " _
exit $rc
