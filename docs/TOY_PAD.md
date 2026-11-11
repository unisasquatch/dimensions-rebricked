# Toy Pad options

LEGO Dimensions needs a Toy Pad to put characters and vehicles into the game. Dimensions Rebricked gives you three
ways to supply one. You don't need the real Toy Pad.

| Option | What places the figures |
|---|---|
| **This PC** (the default) | The on-screen Toy Pad in the pause menu, and the LAN Toy Pad page on a phone or tablet. A real Toy Pad that's plugged in works alongside them. |
| **Physical Toy Pad** | Only a real LEGO Dimensions Toy Pad, connected by USB. |
| **LegoToypad app** | harrysof's [LegoToypad](https://github.com/harrysof/LegoToypad) program. The on-screen Toy Pad still works alongside it. |

## Switching between them

In the game, open **Options > Gameplay > Toy Pad** and press left or right to choose. You can do this from the title
screen's Options or from the pause menu. The change is saved and takes effect at once. Any figures placed from this
PC or by LegoToypad are lifted off when you switch, so place them again afterwards.

## This PC: the on-screen Toy Pad

1. Pause the game. The pause menu has a **Toy Pad** entry in place of "Show Game Controls". Choose it.
2. The Toy Pad board is at the top. Choose a zone: the round centre pad holds 1 figure, and the left and right
   zones hold 3 each.
3. Choose **Characters** or **Vehicles & gadgets**, and sort the list if you like. L1 / R1 switch between the lists.
4. Click a figure, or select it with the controller, to put it on the chosen zone.
5. To lift a figure off, select it on the board.
6. Close the Toy Pad with B / Circle, then carry on.

Figures you place stay on the pad when you close it, just as they would on a real Toy Pad.

## LAN Toy Pad: place figures from a phone or tablet

This is a web page the game serves on your home network, so you can place figures from another device while you
play.

1. Make sure **Options > Gameplay > Toy Pad** is set to **This PC**.
2. Set **Options > Gameplay > LAN Toy Pad** to **On**. An **Address** row appears under it, for example
   `http://192.168.1.20:8642`.
3. On a phone, tablet or another computer **on the same network**, open that address in a web browser.
4. Place and remove figures on the page. They appear in the game straight away.

On the PC itself you can also use **Start > Dimensions Rebricked > Virtual Toy Pad (while the game runs)**, which
opens `http://127.0.0.1:8642`. The F2 key turns the page on or off while you play.

If the page doesn't load from another device:
- The first time, Windows Firewall may ask about `ldport.exe`. Allow it on private networks.
- Check that both devices are on the same Wi-Fi or network, not a guest network.
- If the Address row says "not started (port in use?)", another program is using port 8642. Close it and
  switch LAN Toy Pad off and on again.

Turn LAN Toy Pad **Off** if you don't want the page on your network. The pause menu's Toy Pad still works.

## Physical Toy Pad

Plug the real Toy Pad into a USB port before you start the game. Windows needs no extra driver.

- With **Toy Pad: This PC**, the real pad and the on-screen one work together. Figures on the real pad show on the
  board; lift those off the real pad, since the screen can't remove them.
- With **Toy Pad: Physical Toy Pad**, only the real pad is used, and the on-screen Toy Pad can't place figures.

On a Steam Deck or Linux PC, `install.sh` installs a permission rule so the game can use the pad (it asks for your
password once; `--no-udev` skips it). Unplug the pad and plug it in again after installing.

## LegoToypad app

[LegoToypad](https://github.com/harrysof/LegoToypad) by harrysof is a separate program for placing figures. The game
talks to it on this PC, on port 9191.

**The easy way:** in setup, tick **Download the LegoToypad app**, and optionally **Start LegoToypad with the game**.
Setup downloads it into the install folder's `LegoToypad` folder, checks the download and selects it as the Toy Pad.
It also adds a LegoToypad entry to the Start menu.

**By hand:**
1. Download LegoToypad from its [releases page](https://github.com/harrysof/LegoToypad/releases).
2. Put it in the install folder as `LegoToypad\LegoToypad.exe`. If you'd rather keep it somewhere else, set
   `legotoypad_exe=` in `data\settings.ini` to its full path.
3. In the game, set **Options > Gameplay > Toy Pad** to **LegoToypad app**.
4. Start LegoToypad. To have the game start it for you, set `legotoypad_start=1` in `data\settings.ini`.

The Toy Pad row shows **LegoToypad app (connected)** once LegoToypad is talking to the game, and **(waiting)** until
then. If it stays on waiting, check the port: if you changed `Port` under `[Listener]` in `LegoToypad.ini`, set
`legotoypad_port=` in `data\settings.ini` to the same number.

On a Steam Deck, `install.sh --legotoypad` downloads it, and the game runs it through Wine. Alternatively, add it to
Steam as a non-Steam game with Proton, and leave `legotoypad_start=0`.

## The settings file

All of this is saved in `data\settings.ini` in the install folder. **Start > Dimensions Rebricked > Game folder**
opens that folder. Change the file only while the game is closed.

| Setting | Meaning |
|---|---|
| `toypad=` | `0` This PC, `1` Physical Toy Pad, `2` LegoToypad app |
| `toypad_web=` | `1` LAN Toy Pad on, `0` off |
| `legotoypad_port=` | The port LegoToypad uses (9191 unless you changed it) |
| `legotoypad_start=` | `1` starts LegoToypad with the game |
| `legotoypad_exe=` | Where LegoToypad is, if not in the install folder's `LegoToypad` folder |
