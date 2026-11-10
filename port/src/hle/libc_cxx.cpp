// libc: the C++ runtime pieces the game imports (allocation, static init, error paths).
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "hle/hle.h"

namespace hle {
void AddExitHandler(SYSV void (*fn)(void*), void* arg, bool with_arg);
}

namespace {

SYSV void* New(size_t n) {
    void* p = std::malloc(n ? n : 1);
    if (!p) hle::Fatal("operator new(%zu) failed", n);
    return p;
}
SYSV void Delete(void* p) { std::free(p); }

SYSV int CxaAtexit(SYSV void (*fn)(void*), void* arg, void* /*dso*/) {
    hle::AddExitHandler(fn, arg, true);
    return 0;
}

// Function-local statics: byte 0 of the guard is set once initialisation finished.
std::recursive_mutex g_guard_mutex;
SYSV int CxaGuardAcquire(uint64_t* guard) {
    if (*reinterpret_cast<volatile uint8_t*>(guard)) return 0;
    g_guard_mutex.lock();
    if (*reinterpret_cast<volatile uint8_t*>(guard)) {
        g_guard_mutex.unlock();
        return 0;
    }
    return 1;  // caller runs the initialiser, then calls release
}
SYSV void CxaGuardRelease(uint64_t* guard) {
    *reinterpret_cast<volatile uint8_t*>(guard) = 1;
    g_guard_mutex.unlock();
}

SYSV void PureVirtual() { hle::Fatal("pure virtual function called"); }
SYSV void Terminate() { hle::Fatal("std::terminate"); }
SYSV void* SetTerminate(void* handler) {
    static void* current = nullptr;
    void* old = current;
    current = handler;
    return old;
}
SYSV void XbadAlloc() { hle::Fatal("std::bad_alloc"); }
SYSV void XlengthError(const char* what) { hle::Fatal("std::length_error: %s", what); }
SYSV void XoutOfRange(const char* what) { hle::Fatal("std::out_of_range: %s", what); }
SYSV const char* SyserrorMap(int err) { return std::strerror(err); }

// Exceptions are not supported yet; reaching any of these means the game threw.
SYSV void* CxaBeginCatch(void*) { hle::Fatal("C++ exception caught (unwinding not implemented)"); }
SYSV void UnwindResume(void*) { hle::Fatal("C++ exception unwinding (not implemented)"); }
SYSV int GxxPersonality() { hle::Fatal("C++ personality routine called (unwinding not implemented)"); }

// iostream init objects: nothing to set up, the game doesn't use iostreams for I/O.
SYSV void IosInit(void*) {}

// RTTI: typeinfo objects point 16 bytes into this vtable; only identity matters here.
const void* g_class_type_info_vtable[16] = {};

}  // namespace

#define CXX(name, fn) HLE_EXPORT("libc", name, reinterpret_cast<const void*>(&fn))
CXX("_Znwm", New);
CXX("_Znam", New);
CXX("_ZdlPv", Delete);
CXX("_ZdaPv", Delete);
CXX("__cxa_atexit", CxaAtexit);
CXX("__cxa_guard_acquire", CxaGuardAcquire);
CXX("__cxa_guard_release", CxaGuardRelease);
CXX("__cxa_pure_virtual", PureVirtual);
CXX("_ZSt9terminatev", Terminate);
CXX("_ZSt13set_terminatePFvvE", SetTerminate);
CXX("_ZSt11_Xbad_allocv", XbadAlloc);
CXX("_ZSt14_Xlength_errorPKc", XlengthError);
CXX("_ZSt14_Xout_of_rangePKc", XoutOfRange);
CXX("_ZSt13_Syserror_mapi", SyserrorMap);
CXX("__cxa_begin_catch", CxaBeginCatch);
CXX("_Unwind_Resume", UnwindResume);
CXX("__gxx_personality_v0", GxxPersonality);
CXX("_ZNSt6_WinitC1Ev", IosInit);
CXX("_ZNSt6_WinitD1Ev", IosInit);
CXX("_ZNSt8ios_base4InitC1Ev", IosInit);
CXX("_ZNSt8ios_base4InitD1Ev", IosInit);
HLE_EXPORT("libc", "_ZTVN10__cxxabiv117__class_type_infoE", g_class_type_info_vtable);
