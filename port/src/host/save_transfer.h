#pragma once
// LEGO Dimensions save transfer between the port and the emulators: shadPS4 (PS4), Xenia (Xbox 360), Cemu (Wii U)
// and RPCS3 (PS3). One save slot at a time, in either direction.
//
//   ldport --save-transfer                                     interactive (what the Start menu entry runs)
//   ldport --save-transfer --list [platform]                   saves found in the emulators' default folders
//   ldport --save-transfer --detect <folder>                   which platform a save folder is from
//   ldport --save-transfer --from <folder> --to <folder> --format <platform> [--data <dir>] [--dry-run]
//   ldport --save-transfer --from <folder> --to-port <SlotNN> --data <dir>
//   platform: port | shadps4 | xenia | cemu | rpcs3 (the format written to --to)
//
// The source is only read. Whatever the destination folder held is copied to
// <data>/save-transfer-backups/<date-time>-<platform>-<folder>/ before anything there is replaced.
//
// Save format (checked against PS4 saves; the big-endian consoles' details come from notes on another port of the
// game and are not verified here, so Xenia, Cemu and RPCS3 are marked experimental):
//   24-byte header in the console's byte order: header version (13; Wii U 12), ~FNV-1 of those 4 bytes, a float 0
//   and the hash of its 4 bytes, ~FNV-1 of the payload, payload size.
//   Payload: zeros, marker 0x4dee7e53, block version (GAME 28 / Wii U 27, DLC 21 / Wii U 20), chunks
//   [tag][size][data][tag], sentinel 0x075bcd15, zeros up to the platform's fixed size. Little-endian everywhere
//   except a few chunks stored in the console's own order.
//   GAME is the start of the V2GAME payload (with V2GAME's hash); PS3 has one GAME01 only, padded to 1 KiB.
//   Option files (OPTSC, FEOPTS, ZOPTS) are raw per-console structures: not converted, the game recreates them.
#include <string>
#include <vector>

namespace host {
// args: everything after --save-transfer. Returns 0 done, 1 error, 2 nothing found.
int SaveTransfer(const std::vector<std::string>& args);
}  // namespace host
