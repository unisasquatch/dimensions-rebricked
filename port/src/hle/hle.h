#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Guest (PS4) code uses the System V x86-64 calling convention; every function the
// game calls directly must be declared with SYSV.
#define SYSV __attribute__((sysv_abi))

#ifdef _WIN32
typedef void* HANDLE;
#else
#include <pthread.h>
#endif

namespace hle {

// A guest thread as the host knows it (diagnostics: thread dumps, stall sampling).
#ifdef _WIN32
using ThreadHandle = HANDLE;
#else
using ThreadHandle = pthread_t;
#endif

// Native replacements for Sony library functions and objects, keyed by library and NID.
void Register(const char* library, const char* name, const void* address);
const void* Lookup(const std::string& library, const std::string& nid);

struct AutoRegister {
    AutoRegister(const char* library, const char* name, const void* address) {
        Register(library, name, address);
    }
};

// Registers a stub that returns `ret` and logs its first call; for services the port runs
// without (PSN, networking, system dialogs).
void RegisterConstant(const char* library, const char* name, int64_t ret);

// Guest paths ("/app0/...", "/data/...") to host paths, by longest mounted prefix.
void SetPathRoots(const std::vector<std::string>& app0_layers, const std::string& data);
void Mount(const std::string& guest_prefix, const std::string& host_dir);
void Unmount(const std::string& guest_prefix);
const std::string& DataRoot();
// Folder of extracted add-on packages (--addcont): one subfolder per add-on, named by its entitlement
// label (e.g. OPUSBEETLEJUICE1), holding the package's files (directly or in uroot/).
void SetAddcontRoot(const std::string& dir);
// True when the add-on package with this entitlement label is installed (a folder in the --addcont root).
bool AddcontInstalled(const std::string& label);
std::string HostPath(const char* guest_path);
// Every layer's host path for a guest path, top first (for merged directory listings).
std::vector<std::string> HostPathLayers(const char* guest_path);

[[noreturn]] void Fatal(const char* fmt, ...);

}  // namespace hle

#define HLE_CONCAT_(a, b) a##b
#define HLE_CONCAT(a, b) HLE_CONCAT_(a, b)

// Registers `address` as library::name.
#define HLE_EXPORT(library, name, address) \
    static ::hle::AutoRegister HLE_CONCAT(hle_reg_, __COUNTER__)(library, name, address)

#define HLE_CONSTANT(library, name, ret) \
    static ::hle::AutoConstant HLE_CONCAT(hle_const_, __COUNTER__)(library, name, ret)

namespace hle {
struct AutoConstant {
    AutoConstant(const char* library, const char* name, int64_t ret) { RegisterConstant(library, name, ret); }
};
}  // namespace hle

// Defines a SysV wrapper `g_<name>` that forwards to the host function of the same name.
#define HLE_FORWARD(library, ret, name, params, args) \
    SYSV static ret g_##name params { return ::name args; } \
    HLE_EXPORT(library, #name, reinterpret_cast<const void*>(&g_##name))
