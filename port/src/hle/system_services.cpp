// System services the game depends on for real data: clock, user, system parameters,
// app content, PlayGo, save data and trophies (accepted but not stored).
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <vector>
#include <filesystem>
#include <string>

#include "hle/clock.h"
#include "hle/hle.h"
#include "host/compat.h"

namespace hle {
bool PadSlotPresent(int slot);  // libscepad.cpp: does player slot+1 have a controller?
}

namespace {

using namespace std::chrono;
namespace fs = std::filesystem;

// --- libSceRtc: ticks are microseconds since 0001-01-01 ---
constexpr uint64_t kUnixEpochTicks = 62135596800ull * 1000000ull;

struct RtcDateTime {
    uint16_t year, month, day, hour, minute, second;
    uint32_t microsecond;
};

uint64_t NowTicks() {
    return kUnixEpochTicks + (duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count() +
                              hle::GuestWallOffsetNs()) / 1000;
}

void TicksToDateTime(uint64_t ticks, RtcDateTime* out, bool local) {
    const int64_t unix_us = int64_t(ticks - kUnixEpochTicks);
    const __time64_t secs = unix_us / 1000000;
    std::tm tm{};
    if (local) _localtime64_s(&tm, &secs);
    else _gmtime64_s(&tm, &secs);
    *out = {uint16_t(tm.tm_year + 1900), uint16_t(tm.tm_mon + 1), uint16_t(tm.tm_mday), uint16_t(tm.tm_hour),
            uint16_t(tm.tm_min), uint16_t(tm.tm_sec), uint32_t(unix_us % 1000000)};
}

SYSV int RtcGetCurrentTick(uint64_t* tick) { *tick = NowTicks(); return 0; }
SYSV int RtcGetCurrentClock(RtcDateTime* dt, int tz_minutes) {
    TicksToDateTime(NowTicks() + int64_t(tz_minutes) * 60000000, dt, false);
    return 0;
}
SYSV int RtcGetCurrentClockLocalTime(RtcDateTime* dt) { TicksToDateTime(NowTicks(), dt, true); return 0; }
SYSV int RtcSetTick(RtcDateTime* dt, const uint64_t* tick) { TicksToDateTime(*tick, dt, false); return 0; }
SYSV int RtcSetTimeT(RtcDateTime* dt, int64_t t) {
    TicksToDateTime(kUnixEpochTicks + uint64_t(t) * 1000000, dt, false);
    return 0;
}

// --- libScePosix ---
struct GuestTimespec { int64_t sec, nsec; };
constexpr int kClockRealtime = 0;
SYSV int ClockGettime(int clock, GuestTimespec* ts) {
    int64_t ns = clock == kClockRealtime
                     ? duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count() + hle::GuestWallOffsetNs()
                     : int64_t(hle::GuestNowNs());
    ts->sec = ns / 1000000000;
    ts->nsec = ns % 1000000000;
    return 0;
}

// --- libSceSystemService ---
constexpr int kSystemServiceNoEvent = int(0x80A10003);
constexpr int kParamLang = 1, kParamEnterButtonAssign = 1000;
constexpr int kLangEnglishUs = 1, kEnterButtonCross = 1;

SYSV int GetDisplaySafeAreaInfo(float* ratio) { *ratio = 1.0f; return 0; }
SYSV int GetStatus(uint8_t* status) { std::memset(status, 0, 0x88); return 0; }
SYSV int HideSplashScreen() { return 0; }
SYSV int ParamGetInt(int id, int* value) {
    *value = id == kParamLang ? kLangEnglishUs : id == kParamEnterButtonAssign ? kEnterButtonCross : 0;
    return 0;
}
SYSV int ReceiveEvent(void*) { return kSystemServiceNoEvent; }

// --- libSceUserService: user 1 is always logged in; users 2-4 (local co-op) while they have a controller ---
// User N is player N and pad handle N (libscepad.cpp).
constexpr int kUserId = 1;
constexpr int kMaxUsers = 4;
constexpr int kUserServiceNoEvent = int(0x80960007);
// Users the game has been told are logged in (bit N = user N+1); changed only by GetUserEvent, so the login
// list never shows a user before the game has seen its login event.
std::atomic<uint32_t> g_logged_users{0};
SYSV int UserInitialize(void*) { return 0; }
SYSV int UserTerminate() { return 0; }
SYSV int GetInitialUser(int* user) { *user = kUserId; return 0; }
SYSV int GetLoginUserIdList(int* list) {
    const uint32_t logged = g_logged_users.load() | 1u;
    int n = 0;
    for (int i = 0; i < kMaxUsers; ++i)
        if (logged & (1u << i)) list[n++] = kUserId + i;
    while (n < kMaxUsers) list[n++] = -1;
    return 0;
}
SYSV int GetUserName(int user, char* name, uint64_t size) {
    if (user > kUserId && user < kUserId + kMaxUsers) std::snprintf(name, size, "Player %d", user - kUserId + 1);
    else std::snprintf(name, size, "Player");
    return 0;
}
// The PS4's player colours: blue, red, green, pink.
SYSV int GetUserColor(int user, int* color) {
    *color = user >= kUserId && user < kUserId + kMaxUsers ? user - kUserId : 0;
    return 0;
}
// A real PS4 reports each signed-in user with a login event at startup; games bind players to
// controllers when they see it. Players 2-4 log in when their controller connects and out when it goes
// away (one event per call; the game polls every frame and opens the new user's pad on login).
struct UserServiceEvent { int32_t type; int32_t user; };
constexpr int32_t kUserEventLogin = 0, kUserEventLogout = 1;
SYSV int GetUserEvent(UserServiceEvent* event) {
    if (!(g_logged_users.fetch_or(1u) & 1u)) {
        event->type = kUserEventLogin;
        event->user = kUserId;
        return 0;
    }
    for (int i = 1; i < kMaxUsers; ++i) {
        const bool present = hle::PadSlotPresent(i);
        const bool logged = (g_logged_users.load() >> i) & 1u;
        if (present == logged) continue;
        if (present) g_logged_users.fetch_or(1u << i);
        else g_logged_users.fetch_and(~(1u << i));
        event->type = present ? kUserEventLogin : kUserEventLogout;
        event->user = kUserId + i;
        std::fprintf(stderr, "[user] user %d %s\n", kUserId + i, present ? "logged in" : "logged out");
        return 0;
    }
    return kUserServiceNoEvent;
}

// --- libSceSysmodule: everything is built in ---
SYSV int SysmoduleOk(uint32_t) { return 0; }

// --- libSceAppContent ---
constexpr int kAppContentNotFound = int(0x80D90005);
SYSV int AppContentInitialize(const void*, uint32_t* boot_param) {
    boot_param[0] = boot_param[1] = 0;
    return 0;
}
SYSV int AppParamGetInt(int, int* value) { *value = 0; return 0; }
// Add-on content: each subfolder of the --addcont root is one installed add-on package.
struct EntitlementLabel { char data[17]; char padding[3]; };
struct AddcontInfo { EntitlementLabel label; uint32_t status; };
static_assert(sizeof(AddcontInfo) == 24);
constexpr uint32_t kAddcontInstalled = 4;
struct Addcont { std::string label, host_dir; };
std::string g_addcont_root;
const std::vector<Addcont>& Addconts() {
    static const std::vector<Addcont> list = [] {
        std::vector<Addcont> out;
        std::error_code ec;
        if (g_addcont_root.empty() || !fs::is_directory(g_addcont_root, ec)) return out;
        for (const auto& e : fs::directory_iterator(g_addcont_root, ec)) {
            if (!e.is_directory()) continue;
            const std::string label = e.path().filename().string();
            if (label.empty() || label.size() > 16) continue;
            const fs::path uroot = e.path() / "uroot";
            out.push_back({label, (fs::is_directory(uroot, ec) ? uroot : e.path()).string()});
        }
        std::sort(out.begin(), out.end(), [](const Addcont& a, const Addcont& b) { return a.label < b.label; });
        std::fprintf(stderr, "[appcontent] %zu add-on package(s) in %s\n", out.size(), g_addcont_root.c_str());
        return out;
    }();
    return list;
}
int FindAddcont(const EntitlementLabel* label) {
    if (!label) return -1;
    const auto& list = Addconts();
    for (size_t i = 0; i < list.size(); ++i)
        if (std::strncmp(list[i].label.c_str(), label->data, 16) == 0) return int(i);
    return -1;
}
SYSV int GetAddcontInfoList(uint32_t, AddcontInfo* infos, uint32_t count, uint32_t* hits) {
    const auto& list = Addconts();
    uint32_t n = uint32_t(list.size());
    if (infos && count) {
        n = std::min(n, count);
        for (uint32_t i = 0; i < n; ++i) {
            infos[i] = {};
            std::strncpy(infos[i].label.data, list[i].label.c_str(), 16);
            infos[i].status = kAddcontInstalled;
        }
    }
    if (hits) *hits = n;
    return 0;
}
SYSV int GetAddcontInfo(uint32_t, const EntitlementLabel* label, AddcontInfo* info) {
    const int i = FindAddcont(label);
    if (i < 0) return kAppContentNotFound;
    *info = {};
    std::strncpy(info->label.data, Addconts()[i].label.c_str(), 16);
    info->status = kAddcontInstalled;
    return 0;
}
SYSV int AddcontMount(uint32_t, const EntitlementLabel* label, char* mount_point) {
    const int i = FindAddcont(label);
    if (i < 0) return kAppContentNotFound;
    const std::string guest = "/addcont" + std::to_string(i);
    hle::Mount(guest, Addconts()[i].host_dir);
    std::strcpy(mount_point, guest.c_str());  // SceAppContentMountPoint: 16 chars
    std::fprintf(stderr, "[appcontent] mounted %s at %s\n", Addconts()[i].label.c_str(), guest.c_str());
    return 0;
}
// The extracted packages are plain files: no key is needed, any value will do.
SYSV int GetEntitlementKey(uint32_t, const EntitlementLabel* label, uint8_t* key) {
    if (FindAddcont(label) < 0) return kAppContentNotFound;
    std::memset(key, 0, 16);
    return 0;
}
SYSV int TemporaryDataMount2(int, char* mount_point) {
    hle::Mount("/temp0", hle::DataRoot() + "/temp0");
    std::strcpy(mount_point, "/temp0");
    return 0;
}

// --- libScePlayGo: the whole game is installed ---
constexpr uint32_t kLocusLocalFast = 3;
struct PlayGoProgress { uint64_t progress, total; };
SYSV int PlayGoInitialize(const void*) { return 0; }
SYSV int PlayGoTerminate() { return 0; }
SYSV int PlayGoOpen(int* handle, const void*) { *handle = 1; return 0; }
SYSV int PlayGoClose(int) { return 0; }
// ScePlayGoLocus is one byte: writing wider values corrupts the caller's neighbouring locals (the
// game's install-progress loop keeps its chunk counter right after it and spun forever).
SYSV int PlayGoGetLocus(int, const uint16_t*, uint32_t n, uint8_t* loci) {
    for (uint32_t i = 0; i < n; ++i) loci[i] = uint8_t(kLocusLocalFast);
    return 0;
}
SYSV int PlayGoGetProgress(int, const uint16_t*, uint32_t, PlayGoProgress* p) {
    p->progress = p->total = 1;
    return 0;
}
SYSV int PlayGoPrefetch(int, const uint16_t*, uint32_t, uint32_t) { return 0; }

// --- libSceSaveData: each save directory is a folder under <data>/savedata ---
constexpr int kSaveDataNotFound = int(0x809F0008);
constexpr uint32_t kMountCreate = 0x4, kMountCreate2 = 0x20;
struct SaveDirName { char data[32]; };
struct SaveMount {
    int32_t user;
    int32_t pad;
    const char* title_id;
    const SaveDirName* dir_name;
    const void* fingerprint;
    uint64_t blocks;
    uint32_t mount_mode;
};
struct SaveMountResult {
    char mount_point[16];
    uint64_t required_blocks;
    uint32_t unused;
    uint32_t mount_status;
};
struct SaveMountInfo { uint64_t blocks, free_blocks; };
struct SaveSearchCond {
    int32_t user;
    int32_t pad;
    const char* title_id;
    const SaveDirName* dir_name;
    uint32_t key, order;
};
struct SaveSearchResult {
    uint32_t hit_num;
    int32_t pad;
    SaveDirName* dir_names;
    uint32_t dir_names_num;
    uint32_t set_num;
};

std::string SaveRoot() { return hle::DataRoot() + "/savedata"; }

SYSV int SaveDataInitialize(const void*) { std::fprintf(stderr, "[save] initialize\n"); return 0; }
SYSV int SaveDataMount(const SaveMount* m, SaveMountResult* r) {
    const std::string dir = SaveRoot() + "/" + m->dir_name->data;
    const bool exists = fs::is_directory(dir);
    std::fprintf(stderr, "[save] mount '%s' mode %#x user %d (exists %d)\n", m->dir_name->data, m->mount_mode, m->user, exists);
    if (!exists && !(m->mount_mode & (kMountCreate | kMountCreate2))) return kSaveDataNotFound;
    hle::Mount("/savedata0", dir);
    std::memset(r, 0, sizeof(*r));
    std::strcpy(r->mount_point, "/savedata0");
    r->mount_status = exists ? 0 : 1;  // 1 = created
    return 0;
}
SYSV int SaveDataUmount(const char* mount_point) { hle::Unmount(mount_point); return 0; }
SYSV int SaveDataGetMountInfo(const char*, SaveMountInfo* info) {
    info->blocks = 0x10000;
    info->free_blocks = 0x8000;
    return 0;
}
SYSV int SaveDataGetParam(const char*, uint32_t, void* buf, uint64_t size, uint64_t* got) {
    std::memset(buf, 0, size);
    if (got) *got = size;
    return 0;
}
SYSV int SaveDataOk() { return 0; }
SYSV int SaveDataDirNameSearch(const SaveSearchCond* cond, SaveSearchResult* res) {
    std::fprintf(stderr, "[save] dir search '%s'\n", cond->dir_name ? cond->dir_name->data : "*");
    res->hit_num = res->set_num = 0;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(SaveRoot(), ec)) {
        if (!e.is_directory()) continue;
        const std::string name = e.path().filename().string();
        if (cond->dir_name && name != cond->dir_name->data) continue;  // TODO: '%' wildcards
        if (res->dir_names && res->set_num < res->dir_names_num) {
            std::snprintf(res->dir_names[res->set_num].data, 32, "%s", name.c_str());
            ++res->set_num;
        }
        ++res->hit_num;
    }
    return 0;
}

// --- libSceNpTrophy: accepted and discarded ---
SYSV int TrophyCreate(int* out) { *out = 1; return 0; }
SYSV int TrophyOk() { return 0; }
SYSV int TrophyUnlock(int, int, int, int* platinum) {
    if (platinum) *platinum = -1;
    return 0;
}
SYSV int TrophyGetUnlockState(int, int, uint8_t* flags, uint32_t* count) {
    std::memset(flags, 0, 16);
    if (count) *count = 0;
    return 0;
}

// --- libSceNetCtl: always disconnected ---
SYSV int NetCtlRegisterCallback(void*, void*, int* cid) { *cid = 1; return 0; }
SYSV int NetCtlGetState(int* state) { *state = 0; return 0; }

}  // namespace

namespace hle {
void SetAddcontRoot(const std::string& dir) { g_addcont_root = dir; }
bool AddcontInstalled(const std::string& label) {
    for (const Addcont& a : Addconts())
        if (a.label == label) return true;
    return false;
}
}  // namespace hle

#define FN(lib, name, fn) HLE_EXPORT(lib, name, reinterpret_cast<const void*>(&fn))
FN("libSceRtc", "sceRtcGetCurrentTick", RtcGetCurrentTick);
FN("libSceRtc", "sceRtcGetCurrentNetworkTick", RtcGetCurrentTick);
FN("libSceRtc", "sceRtcGetCurrentClock", RtcGetCurrentClock);
FN("libSceRtc", "sceRtcGetCurrentClockLocalTime", RtcGetCurrentClockLocalTime);
FN("libSceRtc", "sceRtcSetTick", RtcSetTick);
FN("libSceRtc", "sceRtcSetTime_t", RtcSetTimeT);
FN("libScePosix", "clock_gettime", ClockGettime);
FN("libSceSystemService", "sceSystemServiceGetDisplaySafeAreaInfo", GetDisplaySafeAreaInfo);
FN("libSceSystemService", "sceSystemServiceGetStatus", GetStatus);
FN("libSceSystemService", "sceSystemServiceHideSplashScreen", HideSplashScreen);
FN("libSceSystemService", "sceSystemServiceParamGetInt", ParamGetInt);
FN("libSceSystemService", "sceSystemServiceReceiveEvent", ReceiveEvent);
FN("libSceUserService", "sceUserServiceInitialize", UserInitialize);
FN("libSceUserService", "sceUserServiceTerminate", UserTerminate);
FN("libSceUserService", "sceUserServiceGetInitialUser", GetInitialUser);
FN("libSceUserService", "sceUserServiceGetLoginUserIdList", GetLoginUserIdList);
FN("libSceUserService", "sceUserServiceGetUserName", GetUserName);
FN("libSceUserService", "sceUserServiceGetUserColor", GetUserColor);
FN("libSceUserService", "sceUserServiceGetEvent", GetUserEvent);
FN("libSceSysmodule", "sceSysmoduleLoadModule", SysmoduleOk);
FN("libSceSysmodule", "sceSysmoduleUnloadModule", SysmoduleOk);
FN("libSceSysmodule", "sceSysmoduleIsLoaded", SysmoduleOk);
FN("libSceAppContent", "sceAppContentInitialize", AppContentInitialize);
FN("libSceAppContent", "sceAppContentAppParamGetInt", AppParamGetInt);
FN("libSceAppContent", "sceAppContentGetAddcontInfoList", GetAddcontInfoList);
FN("libSceAppContent", "sceAppContentTemporaryDataMount2", TemporaryDataMount2);
FN("libSceAppContent", "sceAppContentGetAddcontInfo", GetAddcontInfo);
FN("libSceAppContent", "sceAppContentAddcontMount", AddcontMount);
HLE_CONSTANT("libSceAppContent", "sceAppContentAddcontUnmount", 0);
HLE_CONSTANT("libSceAppContent", "sceAppContentAddcontDelete", kAppContentNotFound);
HLE_CONSTANT("libSceAppContent", "sceAppContentAddcontEnqueueDownload", kAppContentNotFound);
FN("libSceAppContent", "sceAppContentGetEntitlementKey", GetEntitlementKey);
FN("libScePlayGo", "scePlayGoInitialize", PlayGoInitialize);
FN("libScePlayGo", "scePlayGoTerminate", PlayGoTerminate);
FN("libScePlayGo", "scePlayGoOpen", PlayGoOpen);
FN("libScePlayGo", "scePlayGoClose", PlayGoClose);
FN("libScePlayGo", "scePlayGoGetLocus", PlayGoGetLocus);
FN("libScePlayGo", "scePlayGoGetProgress", PlayGoGetProgress);
FN("libScePlayGo", "scePlayGoPrefetch", PlayGoPrefetch);
FN("libSceSaveData", "sceSaveDataInitialize", SaveDataInitialize);
FN("libSceSaveData", "sceSaveDataMount", SaveDataMount);
FN("libSceSaveData", "sceSaveDataUmount", SaveDataUmount);
FN("libSceSaveData", "sceSaveDataGetMountInfo", SaveDataGetMountInfo);
FN("libSceSaveData", "sceSaveDataGetParam", SaveDataGetParam);
FN("libSceSaveData", "sceSaveDataSetParam", SaveDataOk);
FN("libSceSaveData", "sceSaveDataSaveIcon", SaveDataOk);
FN("libSceSaveData", "sceSaveDataDelete", SaveDataOk);
FN("libSceSaveData", "sceSaveDataDirNameSearch", SaveDataDirNameSearch);
FN("libSceNpTrophy", "sceNpTrophyCreateContext", TrophyCreate);
FN("libSceNpTrophy", "sceNpTrophyCreateHandle", TrophyCreate);
FN("libSceNpTrophy", "sceNpTrophyRegisterContext", TrophyOk);
FN("libSceNpTrophy", "sceNpTrophyDestroyContext", TrophyOk);
FN("libSceNpTrophy", "sceNpTrophyDestroyHandle", TrophyOk);
FN("libSceNpTrophy", "sceNpTrophyUnlockTrophy", TrophyUnlock);
FN("libSceNpTrophy", "sceNpTrophyGetTrophyUnlockState", TrophyGetUnlockState);
FN("libSceNetCtl", "sceNetCtlRegisterCallback", NetCtlRegisterCallback);
FN("libSceNetCtl", "sceNetCtlGetState", NetCtlGetState);
