// System dialogs (libSceSaveDataDialog, libSceMsgDialog). The PS4 draws these itself over the game;
// the port has no system UI, so each dialog "opens", finishes at once and answers OK / Yes. That is
// what a player would choose for the save prompts the game shows (create save data, overwrite),
// and the game draws its own menus for everything else.
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "hle/hle.h"

namespace {

constexpr int kStatusInitialized = 1, kStatusFinished = 3;
constexpr int32_t kButtonYes = 1;  // SCE_*_DIALOG_BUTTON_ID_OK / YES

struct DirName { char data[32]; };

// --- libSceSaveDataDialog ---
int g_save_status = kStatusInitialized;
int32_t g_save_mode = 0;
DirName g_save_dir{};  // the save a list dialog "selects"

const char* SaveModeName(int32_t m) {
    switch (m) {
        case 1: return "list";
        case 2: return "user message";
        case 3: return "system message";
        case 4: return "error code";
        case 5: return "progress bar";
        default: return "?";
    }
}

SYSV int SaveDialogInitialize() { g_save_status = kStatusInitialized; return 0; }
SYSV int SaveDialogTerminate() { g_save_status = kStatusInitialized; return 0; }
SYSV int SaveDialogOpen(const uint8_t* param) {
    g_save_mode = *reinterpret_cast<const int32_t*>(param + 52);
    const int32_t disp_type = *reinterpret_cast<const int32_t*>(param + 56);
    g_save_dir = {};
    // SceSaveDataDialogParam: base (48), size, mode, dispType, then pointers: animParam +64, items +72,
    // userMsgParam +80, sysMsgParam +88, errorCodeParam +96, progressBarParam +104.
    // Items: userId, titleId +8, dirName +16, dirNameNum +24.
    if (const uint8_t* items = *reinterpret_cast<const uint8_t* const*>(param + 72)) {
        const auto* dirs = *reinterpret_cast<const DirName* const*>(items + 16);
        const uint32_t count = *reinterpret_cast<const uint32_t*>(items + 24);
        if (dirs && count) g_save_dir = dirs[0];
        g_save_dir.data[31] = '\0';
    }
    int32_t sys_msg = -1;
    if (const uint8_t* sys = *reinterpret_cast<const uint8_t* const*>(param + 88)) sys_msg = *reinterpret_cast<const int32_t*>(sys);
    std::fprintf(stderr, "[dialog] save data dialog: %s (type %d, system message %d, save '%s') -> answered yes\n",
                 SaveModeName(g_save_mode), disp_type, sys_msg, g_save_dir.data);
    g_save_status = kStatusFinished;
    return 0;
}
SYSV int SaveDialogUpdateStatus() { return g_save_status; }
SYSV int SaveDialogGetResult(uint8_t* result) {
    *reinterpret_cast<int32_t*>(result + 0) = g_save_mode;
    *reinterpret_cast<int32_t*>(result + 4) = 0;  // SCE_COMMON_DIALOG_RESULT_OK
    *reinterpret_cast<int32_t*>(result + 8) = kButtonYes;
    if (auto* dir = *reinterpret_cast<DirName**>(result + 16); dir && g_save_dir.data[0]) *dir = g_save_dir;
    return 0;
}
SYSV int SaveDialogClose() { g_save_status = kStatusInitialized; return 0; }
SYSV int SaveDialogOk() { return 0; }

// --- libSceMsgDialog ---
int g_msg_status = kStatusInitialized;
int32_t g_msg_mode = 0;
SYSV int MsgDialogInitialize() { g_msg_status = kStatusInitialized; return 0; }
SYSV int MsgDialogTerminate() { g_msg_status = kStatusInitialized; return 0; }
SYSV int MsgDialogOpen(const uint8_t* param) {
    // SceMsgDialogParam: base (48 bytes), size, mode at +52, then the mode's parameter pointer.
    g_msg_mode = *reinterpret_cast<const int32_t*>(param + 52);
    std::fprintf(stderr, "[dialog] message dialog mode %d -> answered yes\n", g_msg_mode);
    g_msg_status = kStatusFinished;
    return 0;
}
SYSV int MsgDialogUpdateStatus() { return g_msg_status; }
SYSV int MsgDialogGetResult(uint8_t* result) {
    *reinterpret_cast<int32_t*>(result + 0) = g_msg_mode;
    *reinterpret_cast<int32_t*>(result + 4) = 0;
    *reinterpret_cast<int32_t*>(result + 8) = kButtonYes;
    return 0;
}
SYSV int MsgDialogClose() { g_msg_status = kStatusInitialized; return 0; }

}  // namespace

#define DIALOG(lib, name, fn) HLE_EXPORT(lib, name, reinterpret_cast<const void*>(&fn))
DIALOG("libSceSaveDataDialog", "sceSaveDataDialogInitialize", SaveDialogInitialize);
DIALOG("libSceSaveDataDialog", "sceSaveDataDialogTerminate", SaveDialogTerminate);
DIALOG("libSceSaveDataDialog", "sceSaveDataDialogOpen", SaveDialogOpen);
DIALOG("libSceSaveDataDialog", "sceSaveDataDialogUpdateStatus", SaveDialogUpdateStatus);
DIALOG("libSceSaveDataDialog", "sceSaveDataDialogGetResult", SaveDialogGetResult);
DIALOG("libSceSaveDataDialog", "sceSaveDataDialogClose", SaveDialogClose);
DIALOG("libSceSaveDataDialog", "sceSaveDataDialogProgressBarSetValue", SaveDialogOk);
DIALOG("libSceMsgDialog", "sceMsgDialogInitialize", MsgDialogInitialize);
DIALOG("libSceMsgDialog", "sceMsgDialogTerminate", MsgDialogTerminate);
DIALOG("libSceMsgDialog", "sceMsgDialogOpen", MsgDialogOpen);
DIALOG("libSceMsgDialog", "sceMsgDialogUpdateStatus", MsgDialogUpdateStatus);
DIALOG("libSceMsgDialog", "sceMsgDialogGetResult", MsgDialogGetResult);
DIALOG("libSceMsgDialog", "sceMsgDialogClose", MsgDialogClose);
