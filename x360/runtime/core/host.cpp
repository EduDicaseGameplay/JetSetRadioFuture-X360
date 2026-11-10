// Clock, sleep and threads on the Xbox 360 (see shim/host.h)
#include "host.h"

namespace host {

static int64_t Frequency() {
    static int64_t f = 0;
    if (!f) {
        LARGE_INTEGER li;
        QueryPerformanceFrequency(&li);
        f = li.QuadPart;
    }
    return f;
}

int64_t NowNs() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    int64_t f = Frequency();
    return c.QuadPart / f * 1000000000ll + c.QuadPart % f * 1000000000ll / f;
}

int64_t SystemTime100ns() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return int64_t((uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime);
}

void SleepNs(int64_t ns) {
    if (ns <= 0) {
        SwitchToThread();
        return;
    }
    // Sleep has millisecond resolution: the last part spins on the clock
    int64_t end = NowNs() + ns;
    if (ns > 2000000) Sleep(DWORD((ns - 1000000) / 1000000));
    while (NowNs() < end) SwitchToThread();
}

namespace {

// guest and host threads without a hardware thread of their own alternate between hardware threads 0 and 1
volatile LONG g_next_core = 0;

struct SpawnArgs {
    std::function<void()> fn;
};

DWORD WINAPI Entry(LPVOID p) {
    SpawnArgs* a = static_cast<SpawnArgs*>(p);
    a->fn();
    delete a;
    return 0;
}

}  // namespace

void Spawn(std::function<void()> fn, const char* name, int core) {
    (void)name;
    SpawnArgs* a = new SpawnArgs;
    a->fn = fn;
    // recompiled code nests a host frame per guest call: give it room
    HANDLE h = CreateThread(NULL, 4u << 20, Entry, a, CREATE_SUSPENDED, NULL);
    if (!h) {
        delete a;
        return;
    }
    // core 0 (hardware threads 0 and 1) runs everything light and audio: core 1 is the title's thread's and core 2
    // the renderer's (gpu/render_thread.cpp)
    if (core < 0) core = int(InterlockedIncrement(&g_next_core) & 1);
    XSetThreadProcessor(h, DWORD(core));
    ResumeThread(h);
    CloseHandle(h);
}

}  // namespace host
