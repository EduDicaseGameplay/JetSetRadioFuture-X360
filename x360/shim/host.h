// Host services for the Xbox 360 runtime: the C++11 thread library pieces the VS2010 XDK compiler lacks
// (mutex, condition variable, atomics), built on the XDK's critical sections, semaphores and interlocked
// operations, plus a monotonic clock and thread spawning.
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <xtl.h>
#undef Yield
#include <stdint.h>
#include <functional>

namespace host {

// monotonic nanoseconds since an arbitrary start
int64_t NowNs();
// wall clock in 100 ns units since 1601
int64_t SystemTime100ns();
void SleepNs(int64_t ns);
inline void YieldThread() { SwitchToThread(); }
// runs fn on a new hardware thread with a stack big enough for recompiled code (nests a host frame per guest call)
void Spawn(std::function<void()> fn, const char* name, int core = -1);

class Mutex {
public:
    Mutex() { InitializeCriticalSection(&cs_); }
    ~Mutex() { DeleteCriticalSection(&cs_); }
    void lock() { EnterCriticalSection(&cs_); }
    void unlock() { LeaveCriticalSection(&cs_); }
    bool try_lock() { return TryEnterCriticalSection(&cs_) != 0; }

private:
    Mutex(const Mutex&);
    Mutex& operator=(const Mutex&);
    CRITICAL_SECTION cs_;
};

}  // namespace host
