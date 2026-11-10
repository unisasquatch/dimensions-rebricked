#pragma once
// Imports LEGO Dimensions saves made by the shadPS4 emulator into the port's data folder.
//
//   ldport --import-shadps4 <path | auto> [--data <dir>] [--user <id>] [--dry-run]
//
// shadPS4 keeps each save directory at <user dir>/savedata/<user id>/<title id>/<save dir>/, holding the files the
// game wrote plus sce_sys/ (param.sfo, icon0.png). The port keeps the same save directories at
// <data>/savedata/<save dir>/ (one user, no title folder), so an import copies each save directory across without
// its sce_* folders. <path> may be the shadPS4 folder, its user/ or savedata/ folder, a user id or title folder, or a
// single save directory; "auto" tries the default shadPS4 locations. Port saves are copied to
// <data>/savedata-backups/<date-time>/ before anything is overwritten.
#include <string>
#include <vector>

namespace host {
// args: everything after --import-shadps4 (the path first). Returns the process exit code:
// 0 imported (or would, with --dry-run), 1 error, 2 no LEGO Dimensions saves found.
int ImportShadps4(const std::vector<std::string>& args);
}  // namespace host
