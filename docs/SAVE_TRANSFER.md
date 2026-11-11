# Transferring saves

Dimensions Rebricked can bring a LEGO Dimensions save in from an emulator, or send one of its own saves out to
one. It works with:

| Emulator | Console | Status |
|---|---|---|
| shadPS4 | PS4 | Works: it uses the same save format as this port |
| Xenia | Xbox 360 | Experimental |
| Cemu | Wii U | Experimental |
| RPCS3 | PS3 | Experimental |

"Experimental" means the conversion follows the save format closely, but hasn't been checked against real saves
from that console. Keep your own copy of anything you care about. The tool backs things up too (see
[Backups](#backups)).

Close the game, and the emulator, before you transfer.

## Where the port keeps its saves

The port has four save slots, in `data\savedata\Slot00` to `Slot03` inside the install folder. By default that
folder is `%LOCALAPPDATA%\Programs\Dimensions Rebricked\data\savedata`. To open it, use
**Start > Dimensions Rebricked > Game folder (saves and log)**.

## Using the transfer tool (Windows)

1. Open **Start > Dimensions Rebricked > Transfer saves (shadPS4, Xenia, Cemu, RPCS3)**. A console window opens.
2. Choose a direction:
   - `1` brings a save **into** this port.
   - `2` sends a save from this port **out** to an emulator.
3. Choose the emulator.
4. Pick the save to copy from:
   - **Into the port:** the tool lists the LEGO Dimensions saves it finds in that emulator's usual folder, with
     the date of each. If yours isn't listed (for example, the emulator is a portable copy), choose
     **type a folder** and enter the save's folder, or any folder above it that holds just one save.
   - **Out of the port:** pick one of the port's slots.
5. Pick where it goes:
   - **Into the port:** pick a slot, `Slot00` to `Slot03`. Each slot shows whether it's empty, or the date of
     the save it holds.
   - **Out to an emulator:** pick the emulator's save folder from the list, or type one. The emulator has to have
     a save folder for the game already: start the game in the emulator and save once, then come back.
6. The tool shows what it's about to do. Type `y` to go ahead.

Then start the game and load the slot you chose.

### Where the tool looks for emulator saves

| Emulator | Windows | Linux / Steam Deck |
|---|---|---|
| shadPS4 | `%APPDATA%\shadPS4\savedata` | `~/.local/share/shadPS4/savedata` (and the Flatpak's folder) |
| Xenia | `Documents\Xenia\content` | `~/.local/share/Xenia/content` |
| Cemu | `%APPDATA%\Cemu\mlc01\usr\save` | `~/.local/share/Cemu/mlc01/usr/save` (and the Flatpak's folder) |
| RPCS3 | `%APPDATA%\rpcs3\dev_hdd0\home` | `~/.config/rpcs3/dev_hdd0/home` (and the Flatpak's folder) |

RPCS3 is usually a portable folder. If so, choose **type a folder** and give its `dev_hdd0\home` folder.

### What carries over

- Your story progress and add-on progress files.
- **Not** the option files (controls, sound and so on). Each console stores those differently, so the game
  recreates them with default settings the first time it starts.

### Saves from a real console

Console saves are encrypted. The tool can only read a decrypted save folder, one that holds the game's
`GAME01` / `V2GAME01` files. A decrypted PS4 save uses the same format as shadPS4: choose **shadPS4**, then
**type a folder**.

## Bringing in shadPS4 saves while installing

Setup offers **Import LEGO Dimensions saves from the shadPS4 emulator**. It's ticked automatically when setup
finds shadPS4 saves. You can also point it at a portable shadPS4's `user` folder, or at a single save folder.

This copies every shadPS4 save at once, keeping the same slots. Use the transfer tool instead to move one save
into a slot of your choice.

## Steam Deck / Linux

The same tool is in the app menu as **Dimensions Rebricked save transfer**. You can also run `save-transfer.sh`
in the install folder. `install.sh --import-shadps4 auto` brings in shadPS4 saves while installing.

## Backups

The tool never changes the save it copies from. Before it writes anything, it copies whatever the destination held
to `data\save-transfer-backups\<date-time>-<platform>-<folder>\`. To undo a transfer, copy that folder's contents
back. The installer's shadPS4 import backs up to `data\savedata-backups\<date-time>\` the same way.

## From a command prompt

```
ldport --save-transfer                                          (the interactive tool)
ldport --save-transfer --list [shadps4|xenia|cemu|rpcs3|port]   (saves found in the usual folders)
ldport --save-transfer --detect <folder>                        (which console a save folder is from)
ldport --save-transfer --from <folder> --to-port Slot00 --data <data folder> [--dry-run]
ldport --save-transfer --from <folder> --to <folder> --format <shadps4|xenia|cemu|rpcs3> --data <data folder> [--dry-run]
```

Run these from the install folder, and add `--data "<install folder>\data"`. `--dry-run` shows what would happen
without writing anything.
