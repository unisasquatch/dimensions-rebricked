// libkernel: pthreads, semaphores and event flags on host primitives.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#endif

#include "hle/hle.h"
#include "hle/sce_error.h"
#include "host/thread_tls.h"

namespace hle {
// Set when a thread is stuck; the main thread then logs where every guest thread is.
extern std::atomic<bool> g_thread_dump_requested;
}  // namespace hle

namespace {

using namespace std::chrono;

// A recursive mutex that hands itself to waiters in arrival order. Host mutexes are not fair: a
// thread that unlocks and immediately re-locks every frame (the game's render thread) can starve
// another thread forever, which froze the game logic. The PS4 kernel gives a released mutex to a waiter.
class FairMutex {
public:
    void lock() { Acquire<nanoseconds>(nullptr); }
    template <typename Rep, typename Period>
    bool try_lock_for(const duration<Rep, Period>& d) { return Acquire(&d); }
    bool try_lock() {
        std::lock_guard g(m_);
        if (owner_ == std::this_thread::get_id()) { ++depth_; return true; }
        if (depth_ != 0 || serving_ != next_) return false;  // held, or others are queued
        ++next_;
        ++serving_;
        Take();
        return true;
    }
    void unlock() {
        std::lock_guard g(m_);
        if (--depth_ == 0) {
            owner_ = std::thread::id();
            SkipAbandoned();  // also notifies; tickets given up by timed-out waiters must not block the queue
        }
    }

private:
    template <typename D = nanoseconds>
    bool Acquire(const D* timeout) {
        std::unique_lock g(m_);
        if (owner_ == std::this_thread::get_id()) { ++depth_; return true; }
        const uint64_t ticket = next_++;
        auto ready = [&] { return depth_ == 0 && serving_ == ticket; };
        if (timeout) {
            if (!cv_.wait_for(g, *timeout, ready)) {
                // Give the ticket up: wait for our turn to pass it on untouched.
                abandoned_.push_back(ticket);
                SkipAbandoned();
                return false;
            }
        } else {
            cv_.wait(g, ready);
        }
        ++serving_;
        SkipAbandoned();
        Take();
        return true;
    }
    void Take() {
        owner_ = std::this_thread::get_id();
        depth_ = 1;
    }
    // Tickets whose holders timed out are skipped so the queue keeps moving.
    void SkipAbandoned() {
        for (bool moved = true; moved;) {
            moved = false;
            for (auto it = abandoned_.begin(); it != abandoned_.end(); ++it)
                if (*it == serving_ && depth_ == 0) {
                    ++serving_;
                    abandoned_.erase(it);
                    moved = true;
                    break;
                }
        }
        cv_.notify_all();
    }

    std::mutex m_;
    std::condition_variable cv_;
    std::thread::id owner_;
    int depth_ = 0;
    uint64_t next_ = 0, serving_ = 0;
    std::vector<uint64_t> abandoned_;
};

// Sony handles are pointers to opaque objects; zero means "statically initialised".
struct MutexAttr { int type = 1; };
struct Mutex {
    FairMutex m;
    std::string name;
    int depth = 0;                 // recursion depth held by `owner` (diagnostics, cond waits)
    const char* owner = nullptr;   // holding thread's name
    steady_clock::time_point locked_since;  // diagnostics: long holds starve other threads
    uint64_t lock_site = 0;        // game return address of the outermost lock
};
struct CondAttr {};
struct Cond {
    std::condition_variable_any cv;
};
struct ThreadAttr {
    uint64_t stack_size = 0x10000;
    int detached = 0;
};
struct Thread {
    SYSV void* (*entry)(void*);
    void* arg;
    void* result = nullptr;
    std::string name;
#ifdef _WIN32
    HANDLE handle = nullptr;
#else
    pthread_t handle{};
#endif
};

constexpr uint64_t kMinHostStack = 4 * 1024 * 1024;

std::mutex g_lazy_init;

// Every guest thread ever created (diagnostics). Handles stay open so they can be inspected.
std::mutex g_threads_mutex;
std::vector<std::pair<std::string, hle::ThreadHandle>> g_threads;

// Diagnostics: an infinite wait that passes kStallReport is logged once with the thread's name.
constexpr auto kStallReport = seconds(3);
thread_local std::string t_thread_name = "GameMain";
void ReportStall(const char* what, const void* object) {
    std::fprintf(stderr, "[wait] thread '%s' blocked >3s on %s %p\n", t_thread_name.c_str(), what, object);
}
template <typename Lock, typename Cv, typename Pred>
void WaitWatched(Cv& cv, Lock& lock, Pred ready, const char* what, const void* object) {
    if (cv.wait_for(lock, kStallReport, ready)) return;
    ReportStall(what, object);
    cv.wait(lock, ready);
}

Mutex* GetMutex(Mutex** m) {
    if (!*m) {
        std::lock_guard lock(g_lazy_init);
        if (!*m) *m = new Mutex;
    }
    return *m;
}
Cond* GetCond(Cond** c) {
    if (!*c) {
        std::lock_guard lock(g_lazy_init);
        if (!*c) *c = new Cond;
    }
    return *c;
}

// --- attributes ---
SYSV int MutexattrInit(MutexAttr** a) { *a = new MutexAttr; return 0; }
SYSV int MutexattrDestroy(MutexAttr** a) { delete *a; *a = nullptr; return 0; }
SYSV int MutexattrSettype(MutexAttr** a, int type) { (*a)->type = type; return 0; }
SYSV int CondattrInit(CondAttr** a) { *a = new CondAttr; return 0; }
SYSV int CondattrDestroy(CondAttr** a) { delete *a; *a = nullptr; return 0; }
SYSV int AttrInit(ThreadAttr** a) { *a = new ThreadAttr; return 0; }
SYSV int AttrDestroy(ThreadAttr** a) { delete *a; *a = nullptr; return 0; }
SYSV int AttrSetstacksize(ThreadAttr** a, uint64_t size) { (*a)->stack_size = size; return 0; }
SYSV int AttrSetdetachstate(ThreadAttr** a, int detached) { (*a)->detached = detached; return 0; }
SYSV int AttrSetaffinity(ThreadAttr**, uint64_t) { return 0; }

// --- mutexes (all types are recursive here; a PS4 self-deadlock would be a game bug anyway) ---
SYSV int MutexInit(Mutex** m, MutexAttr**, const char* name) {
    *m = new Mutex;
    if (name) (*m)->name = name;
    return 0;
}
SYSV int MutexDestroy(Mutex** m) { delete *m; *m = nullptr; return 0; }
void NoteLocked(Mutex* mx, uint64_t site) {
    if (mx->depth++ == 0) {
        mx->owner = t_thread_name.c_str();
        mx->locked_since = steady_clock::now();
        mx->lock_site = site;
    }
}
// LDPORT_MUTEX_HOLDS=<ms>: log mutexes held longer than that (first few per lock site).
void CheckHold(Mutex* mx) {
    static const long long threshold = std::getenv("LDPORT_MUTEX_HOLDS") ? std::atoll(std::getenv("LDPORT_MUTEX_HOLDS")) : 0;
    if (!threshold) return;
    const long long ms = duration_cast<milliseconds>(steady_clock::now() - mx->locked_since).count();
    if (ms < threshold) return;
    static std::mutex log_mutex;
    static std::map<uint64_t, int> logged;
    std::lock_guard lock(log_mutex);
    if (logged[mx->lock_site]++ >= 5) return;
    std::fprintf(stderr, "[mutex] '%s' held mutex %p for %lld ms (locked at game call site %#llx)\n",
                 t_thread_name.c_str(), static_cast<void*>(mx), ms, (unsigned long long)mx->lock_site);
}

SYSV int MutexLock(Mutex** m) {
    const uint64_t site = reinterpret_cast<uint64_t>(__builtin_return_address(0));
    Mutex* mx = GetMutex(m);
    if (!mx->m.try_lock_for(kStallReport)) {
        const char* owner = mx->owner;
        std::fprintf(stderr, "[wait] thread '%s' blocked >3s on mutex %p '%s' held by '%s'\n",
                     t_thread_name.c_str(), static_cast<void*>(mx), mx->name.c_str(), owner ? owner : "?");
        hle::g_thread_dump_requested = true;  // the main thread prints where every thread is
        mx->m.lock();
    }
    NoteLocked(mx, site);
    return 0;
}
SYSV int MutexTrylock(Mutex** m) {
    const uint64_t site = reinterpret_cast<uint64_t>(__builtin_return_address(0));
    Mutex* mx = GetMutex(m);
    if (!mx->m.try_lock()) return hle::SceError(hle::EBUSY_BSD);
    NoteLocked(mx, site);
    return 0;
}
SYSV int MutexUnlock(Mutex** m) {
    Mutex* mx = GetMutex(m);
    if (mx->depth == 1) CheckHold(mx);
    if (--mx->depth == 0) mx->owner = nullptr;
    mx->m.unlock();
    return 0;
}

// pthread_cond_wait releases the mutex completely, even if it is held recursively, and
// restores the same depth on return.
template <typename WaitFn>
int WaitReleasingMutex(Mutex* mx, WaitFn wait) {
    const int depth = mx->depth;
    for (int i = 1; i < depth; ++i) mx->m.unlock();
    mx->depth = 0;
    mx->owner = nullptr;
    const int r = wait(mx->m);  // releases and re-acquires the last level
    for (int i = 1; i < depth; ++i) mx->m.lock();
    mx->depth = depth;
    mx->owner = t_thread_name.c_str();
    return r;
}

// --- condition variables ---
SYSV int CondInit(Cond** c, CondAttr**, const char*) { *c = new Cond; return 0; }
SYSV int CondDestroy(Cond** c) { delete *c; *c = nullptr; return 0; }
SYSV int CondSignal(Cond** c) { GetCond(c)->cv.notify_one(); return 0; }
SYSV int CondBroadcast(Cond** c) { GetCond(c)->cv.notify_all(); return 0; }
SYSV int CondWait(Cond** c, Mutex** m) {
    Cond* cond = GetCond(c);
    return WaitReleasingMutex(GetMutex(m), [&](auto& lock) {
        // A timed-out slice returns like a spurious wakeup; callers re-check their condition.
        if (cond->cv.wait_for(lock, kStallReport) == std::cv_status::timeout) {
            thread_local const void* reported = nullptr;
            if (reported != cond) ReportStall("condition variable", cond);
            reported = cond;
        }
        return 0;
    });
}
SYSV int CondTimedwait(Cond** c, Mutex** m, uint32_t usec) {
    Cond* cond = GetCond(c);
    return WaitReleasingMutex(GetMutex(m), [&](auto& lock) {
        const auto r = cond->cv.wait_for(lock, microseconds(usec));
        return r == std::cv_status::timeout ? hle::SceError(hle::ETIMEDOUT_BSD) : 0;
    });
}

// --- threads ---
#ifdef _WIN32
DWORD WINAPI ThreadMain(void* p) {
    auto* t = static_cast<Thread*>(p);
    host::EnsureGuestThreadTls();
    if (!t->name.empty()) {
        t_thread_name = t->name;
        std::wstring wide(t->name.begin(), t->name.end());
        SetThreadDescription(GetCurrentThread(), wide.c_str());
    }
    t->result = t->entry(t->arg);
    return 0;
}
#else
void* ThreadMain(void* p) {
    auto* t = static_cast<Thread*>(p);
    host::EnsureGuestThreadTls();
    if (!t->name.empty()) {
        t_thread_name = t->name;
        pthread_setname_np(pthread_self(), t->name.substr(0, 15).c_str());  // Linux names are 15 characters
    }
    t->result = t->entry(t->arg);
    return nullptr;
}
#endif

SYSV int Create(Thread** out, ThreadAttr** attr, SYSV void* (*entry)(void*), void* arg, const char* name) {
    auto* t = new Thread{entry, arg};
    if (name) t->name = name;
    // Host functions run on guest thread stacks too, so reserve generously (reservations are free).
    const uint64_t requested = (attr && *attr) ? (*attr)->stack_size : 0x10000;
    const uint64_t stack = requested < kMinHostStack ? kMinHostStack : requested;
#ifdef _WIN32
    // Commit the whole stack: PS4 stack probes can skip past Windows' guard page, and a fault on an
    // uncommitted stack page kills the process before any handler runs.
    t->handle = CreateThread(nullptr, stack, ThreadMain, t, 0, nullptr);
    if (!t->handle) return hle::SceError(hle::EAGAIN_BSD);
    {
        std::lock_guard lock(g_threads_mutex);
        g_threads.push_back({t->name, t->handle});
    }
#else
    pthread_attr_t pa;
    pthread_attr_init(&pa);
    pthread_attr_setstacksize(&pa, stack);
    const int r = pthread_create(&t->handle, &pa, ThreadMain, t);
    pthread_attr_destroy(&pa);
    if (r != 0) return hle::SceError(hle::EAGAIN_BSD);
    {
        std::lock_guard lock(g_threads_mutex);
        g_threads.push_back({t->name, t->handle});
    }
#endif
    *out = t;
    return 0;
}
SYSV int Join(Thread* t, void** result) {
#ifdef _WIN32
    WaitForSingleObject(t->handle, INFINITE);
    CloseHandle(t->handle);
#else
    pthread_join(t->handle, nullptr);
#endif
    if (result) *result = t->result;
    delete t;
    return 0;
}
SYSV void Exit(void* result) {
    (void)result;  // joiners of an exited thread see null for now
#ifdef _WIN32
    ExitThread(0);
#else
    pthread_exit(nullptr);
#endif
}
SYSV int Setprio(Thread*, int) { return 0; }
SYSV int ThreadYield() {
#ifdef _WIN32
    SwitchToThread();
#else
    sched_yield();
#endif
    return 0;
}

// --- semaphores ---
struct Sema {
    std::mutex m;
    std::condition_variable cv;
    int count, max;
};
SYSV int CreateSema(Sema** out, const char*, uint32_t, int init, int max, void*) {
    *out = new Sema{{}, {}, init, max};
    return 0;
}
SYSV int DeleteSema(Sema* s) { delete s; return 0; }
SYSV int SignalSema(Sema* s, int n) {
    {
        std::lock_guard lock(s->m);
        if (s->count + n > s->max) return hle::SceError(hle::EINVAL_BSD);
        s->count += n;
    }
    s->cv.notify_all();
    return 0;
}
SYSV int WaitSema(Sema* s, int need, uint32_t* timeout_us) {
    std::unique_lock lock(s->m);
    auto ready = [&] { return s->count >= need; };
    if (timeout_us) {
        if (!s->cv.wait_for(lock, microseconds(*timeout_us), ready)) {
            return hle::SceError(hle::ETIMEDOUT_BSD);
        }
    } else {
        WaitWatched(s->cv, lock, ready, "semaphore", s);
    }
    s->count -= need;
    return 0;
}

// --- event flags ---
constexpr uint32_t kWaitAnd = 0x01, kWaitOr = 0x02, kClearAll = 0x10, kClearPat = 0x20;
struct EventFlag {
    std::mutex m;
    std::condition_variable cv;
    uint64_t bits;
};
SYSV int CreateEventFlag(EventFlag** out, const char*, uint32_t, uint64_t init, void*) {
    *out = new EventFlag{{}, {}, init};
    return 0;
}
SYSV int DeleteEventFlag(EventFlag* e) { delete e; return 0; }
SYSV int SetEventFlag(EventFlag* e, uint64_t bits) {
    {
        std::lock_guard lock(e->m);
        e->bits |= bits;
    }
    e->cv.notify_all();
    return 0;
}
SYSV int ClearEventFlag(EventFlag* e, uint64_t keep) {
    std::lock_guard lock(e->m);
    e->bits &= keep;
    return 0;
}
SYSV int WaitEventFlag(EventFlag* e, uint64_t pattern, uint32_t mode, uint64_t* result, uint32_t* timeout_us) {
    std::unique_lock lock(e->m);
    auto ready = [&] { return (mode & kWaitAnd) ? (e->bits & pattern) == pattern : (e->bits & pattern) != 0; };
    if (timeout_us) {
        if (!e->cv.wait_for(lock, microseconds(*timeout_us), ready)) {
            if (result) *result = e->bits;
            return hle::SceError(hle::ETIMEDOUT_BSD);
        }
    } else {
        WaitWatched(e->cv, lock, ready, "event flag", e);
    }
    if (result) *result = e->bits;
    if (mode & kClearAll) e->bits = 0;
    else if (mode & kClearPat) e->bits &= ~pattern;
    return 0;
}

}  // namespace

namespace hle {
std::atomic<bool> g_thread_dump_requested{false};

std::vector<std::pair<std::string, ThreadHandle>> GuestThreads() {
    std::lock_guard lock(g_threads_mutex);
    return g_threads;
}
}  // namespace hle

#define KERNEL(name, fn) HLE_EXPORT("libkernel", name, reinterpret_cast<const void*>(&fn))
KERNEL("scePthreadMutexattrInit", MutexattrInit);
KERNEL("scePthreadMutexattrDestroy", MutexattrDestroy);
KERNEL("scePthreadMutexattrSettype", MutexattrSettype);
KERNEL("scePthreadCondattrInit", CondattrInit);
KERNEL("scePthreadCondattrDestroy", CondattrDestroy);
KERNEL("scePthreadAttrInit", AttrInit);
KERNEL("scePthreadAttrDestroy", AttrDestroy);
KERNEL("scePthreadAttrSetstacksize", AttrSetstacksize);
KERNEL("scePthreadAttrSetdetachstate", AttrSetdetachstate);
KERNEL("scePthreadAttrSetaffinity", AttrSetaffinity);
KERNEL("scePthreadMutexInit", MutexInit);
KERNEL("scePthreadMutexDestroy", MutexDestroy);
KERNEL("scePthreadMutexLock", MutexLock);
KERNEL("scePthreadMutexTrylock", MutexTrylock);
KERNEL("scePthreadMutexUnlock", MutexUnlock);
KERNEL("scePthreadCondInit", CondInit);
KERNEL("scePthreadCondDestroy", CondDestroy);
KERNEL("scePthreadCondSignal", CondSignal);
KERNEL("scePthreadCondBroadcast", CondBroadcast);
KERNEL("scePthreadCondWait", CondWait);
KERNEL("scePthreadCondTimedwait", CondTimedwait);
KERNEL("scePthreadCreate", Create);
KERNEL("scePthreadJoin", Join);
KERNEL("scePthreadExit", Exit);
KERNEL("scePthreadSetprio", Setprio);
KERNEL("scePthreadYield", ThreadYield);
KERNEL("sceKernelCreateSema", CreateSema);
KERNEL("sceKernelDeleteSema", DeleteSema);
KERNEL("sceKernelSignalSema", SignalSema);
KERNEL("sceKernelWaitSema", WaitSema);
KERNEL("sceKernelCreateEventFlag", CreateEventFlag);
KERNEL("sceKernelDeleteEventFlag", DeleteEventFlag);
KERNEL("sceKernelSetEventFlag", SetEventFlag);
KERNEL("sceKernelClearEventFlag", ClearEventFlag);
KERNEL("sceKernelWaitEventFlag", WaitEventFlag);
