// Xbox 360 build of pc-recomp/sdk/runtime/kernel/threads.cpp: each guest thread is a hardware thread of its own
// (host::Spawn), the current guest thread sits in a TLS slot.
#include <algorithm>
#include <stdio.h>

#include "kernel/kernel.h"
#include "host.h"

namespace kernel {

namespace {

// the current guest thread; its ThreadRef is kept alive by g_threads or by the spawn closure
__declspec(thread) Thread* t_current = NULL;
std::mutex g_threads_lock;
std::vector<ThreadRef> g_threads;
std::atomic<uint32_t> g_next_id(0x10);

// guest structures per thread
const uint32_t kKpcrSize = 0x400;
const uint32_t kEthreadSize = 0x200;

void SetupGuestBlocks(Thread& t, uint32_t stack_size, uint32_t tls_size) {
    stack_size = std::max<uint32_t>(stack_size, 0x10000);
    tls_size = (tls_size + 3) & ~3u;
    t.stack_top = mem::AllocStack(stack_size + tls_size + 0x1000);
    t.stack_size = stack_size;
    t.kpcr = mem::AllocSystem(kKpcrSize, 64);
    t.ethread = mem::AllocSystem(kEthreadSize, 64);
    // TLS sits at the top of the stack: StackBase + tls_index * 4 must equal TlsData
    t.tls = t.stack_top - tls_size;
    uint32_t k = t.kpcr;
    W32(k + 0x00, 0xFFFFFFFF);                // NtTib.ExceptionList
    W32(k + 0x04, t.stack_top);               // NtTib.StackBase
    W32(k + 0x08, t.stack_top - stack_size);  // NtTib.StackLimit
    W32(k + 0x18, k);                         // NtTib.Self
    W32(k + 0x1C, k);                         // SelfPcr
    W32(k + 0x20, k + 0x28);                  // Prcb
    G8(k + 0x24) = 0;                         // Irql
    W32(k + 0x28, t.ethread);                 // PrcbData.CurrentThread
    uint32_t e = t.ethread;
    G8(e + 0) = 6;                             // dispatcher type ThreadObject
    G8(e + 2) = uint8_t(kEthreadSize / 4);     // Header.Size in dwords
    W32(e + 0x1C, t.stack_top);                // KTHREAD.StackBase
    W32(e + 0x20, t.stack_top - stack_size);   // KTHREAD.StackLimit
    W32(e + 0x28, t.tls);                      // KTHREAD.TlsData
    W32(e + 0x12C, t.id);                      // ETHREAD.UniqueThread
}

void InitContext(Thread& t, X86Context& ctx) {
    memset(&ctx, 0, sizeof(ctx));
    ctx.esp = (t.tls & ~15u) - 16;
    ctx.fs_base = t.kpcr;
    ctx.fcw = 0x27F;     // x87 53 bit precision, exceptions masked
    ctx.mxcsr = 0x1F80;  // SSE exceptions masked, round to nearest
    ctx.thread_handle = 0;
    t.ctx = &ctx;
}

void Register(const ThreadRef& t) {
    std::lock_guard<std::mutex> lock(g_threads_lock);
    g_threads.push_back(t);
}

void Unregister(Thread* t) {
    std::lock_guard<std::mutex> lock(g_threads_lock);
    for (size_t i = 0; i < g_threads.size(); i++)
        if (g_threads[i].get() == t) {
            g_threads.erase(g_threads.begin() + i);
            break;
        }
}

void MarkExited(Thread& t, uint32_t code) {
    t.exit_code = code;
    std::lock_guard<std::mutex> lock(DispatchLock());
    SetSignal(t, 1);
    SignalChanged();
}

struct StartInfo {
    uint32_t system_routine, start_routine, start_context;
};
std::unordered_map<Thread*, StartInfo> g_start;
std::mutex g_start_lock;

}  // namespace

Thread* Current() { return t_current; }
ThreadRef CurrentRef() {
    Thread* t = t_current;
    if (!t) return ThreadRef();
    std::lock_guard<std::mutex> lock(g_threads_lock);
    for (size_t i = 0; i < g_threads.size(); i++)
        if (g_threads[i].get() == t) return g_threads[i];
    return ThreadRef();
}
X86Context& CurrentContext() { return *t_current->ctx; }

std::vector<ThreadRef> Threads() {
    std::lock_guard<std::mutex> lock(g_threads_lock);
    return g_threads;
}

ThreadRef CreateThread(uint32_t stack_size, uint32_t tls_size, uint32_t system_routine, uint32_t start_routine,
                       uint32_t start_context, bool suspended) {
    ThreadRef t = std::make_shared<Thread>();
    t->id = g_next_id.fetch_add(4);
    SetupGuestBlocks(*t, stack_size, tls_size);
    Bind(t, t->ethread);
    if (suspended) t->suspend = 1;
    std::lock_guard<std::mutex> lock(g_start_lock);
    StartInfo si = {system_routine, start_routine, start_context};
    g_start[t.get()] = si;
    return t;
}

static void RunThread(ThreadRef t) {
    t_current = t.get();
    X86Context* ctx = static_cast<X86Context*>(_aligned_malloc(sizeof(X86Context), 64));
    InitContext(*t, *ctx);
    {
        std::unique_lock<std::mutex> lock(t->apc_lock);
        while (t->suspend.load() > 0) t->resume_cv.wait(lock);
    }
    StartInfo si;
    {
        std::lock_guard<std::mutex> lock(g_start_lock);
        si = g_start[t.get()];
        g_start.erase(t.get());
    }
    LOGI("thread %X starts at %08X (%08X, %08X)", t->id, si.system_routine ? si.system_routine : si.start_routine,
         si.start_routine, si.start_context);
    uint32_t code;
    if (si.system_routine)
        code = CallGuest(*ctx, si.system_routine, si.start_routine, si.start_context);
    else
        code = CallGuest(*ctx, si.start_routine, si.start_context);
    LOGI("thread %X returned %08X", t->id, code);
    MarkExited(*t, code);
    Unregister(t.get());
    t_current = NULL;
}

void StartThread(ThreadRef t) {
    Register(t);
    char name[32];
    _snprintf(name, sizeof(name), "guest %X", t->id);
    host::Spawn([t] { RunThread(t); }, name);
}

void StartHostThread(const char* name, std::function<void(X86Context&)> fn) {
    ThreadRef t = std::make_shared<Thread>();
    t->id = g_next_id.fetch_add(4);
    SetupGuestBlocks(*t, 0x20000, 0x100);
    Bind(t, t->ethread);
    Register(t);
    host::Spawn(
        [t, fn] {
            t_current = t.get();
            X86Context* ctx = static_cast<X86Context*>(_aligned_malloc(sizeof(X86Context), 64));
            InitContext(*t, *ctx);
            fn(*ctx);
            t_current = NULL;
        },
        name);
}

void ExitThread(uint32_t code) {
    Thread* t = Current();
    if (t) {
        LOGI("thread %X exits with %08X", t->id, code);
        MarkExited(*t, code);
        Unregister(t);
    }
    GuestRelease();
    t_current = NULL;
    ::ExitThread(code);
}

void Resume(Thread& t) {
    std::lock_guard<std::mutex> lock(t.apc_lock);
    if (t.suspend > 0) t.suspend--;
    t.resume_cv.notify_all();
}

uint32_t Suspend(Thread& t) {
    uint32_t prev = uint32_t(t.suspend++);
    if (&t == Current()) SuspendPoint();
    return prev;
}

void SuspendPoint() {
    Thread* t = Current();
    if (!t || t->suspend.load() <= 0) return;
    GuestUnlock unlocked;
    std::unique_lock<std::mutex> lock(t->apc_lock);
    while (t->suspend.load() > 0) t->resume_cv.wait(lock);
}

void QueueApc(Thread& t, uint32_t routine, uint32_t a1, uint32_t a2, uint32_t a3) {
    std::lock_guard<std::mutex> lock(t.apc_lock);
    Apc a = {routine, a1, a2, a3};
    t.apcs.push_back(a);
    SignalChanged();
}

bool RunApcs(Thread& t) {
    bool ran = false;
    for (;;) {
        Apc a;
        {
            std::lock_guard<std::mutex> lock(t.apc_lock);
            if (t.apcs.empty()) break;
            a = t.apcs.front();
            t.apcs.pop_front();
        }
        CallGuest(*t.ctx, a.routine, a.a1, a.a2, a.a3);
        ran = true;
    }
    return ran;
}

}  // namespace kernel
