// Xbox 360 build of pc-recomp/sdk/runtime/kernel/kernel.h (VS2010 compiler).
// Guest memory is little endian: R16/R32/R64 and W16/W32/W64 swap, G8 is the only direct reference.
#pragma once
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stdint.h>
#include <string.h>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/log.h"
#include "cpu/x86_ops.h"
#include "kernel/memory.h"

namespace kernel {

// NTSTATUS values the kernel hands back
enum : uint32_t {
    kSuccess = 0x00000000,
    kWait0 = 0x00000000,
    kAbandoned = 0x00000080,
    kUserApc = 0x000000C0,
    kAlerted = 0x00000101,
    kTimeout = 0x00000102,
    kPending = 0x00000103,
    kObjectNameExists = 0x40000000,
    kNoMoreFiles = 0x80000006,
    kUnsuccessful = 0xC0000001,
    kNotImplemented = 0xC0000002,
    kInvalidHandle = 0xC0000008,
    kInvalidParameter = 0xC000000D,
    kNoSuchFile = 0xC000000F,
    kEndOfFile = 0xC0000011,
    kNoMemory = 0xC0000017,
    kAccessDenied = 0xC0000022,
    kBufferTooSmall = 0xC0000023,
    kObjectTypeMismatch = 0xC0000024,
    kObjectNameInvalid = 0xC0000033,
    kObjectNameNotFound = 0xC0000034,
    kObjectNameCollision = 0xC0000035,
    kObjectPathNotFound = 0xC000003A,
    kMutantNotOwned = 0xC0000046,
    kSemaphoreLimitExceeded = 0xC0000047,
    kNotADirectory = 0xC0000103,
    kFileIsADirectory = 0xC00000BA,
    kNotSupported = 0xC00000BB,
    kDeviceNotConnected = 0xC000009D,
    kCannotDelete = 0xC0000121,
};

extern uint8_t* g_base;

inline uint8_t* HostAddr(uint32_t va) { return g_base + (va & X86_GUEST_MASK); }
inline void* HostPtr(uint32_t va) { return HostAddr(va); }
inline uint8_t& G8(uint32_t va) { return *HostAddr(va); }
inline uint32_t R32(uint32_t va) { return __loadwordbytereverse(0, HostAddr(va)); }
inline void W32(uint32_t va, uint32_t v) { __storewordbytereverse(v, 0, HostAddr(va)); }
inline uint16_t R16(uint32_t va) { return __loadshortbytereverse(0, HostAddr(va)); }
inline void W16(uint32_t va, uint16_t v) { __storeshortbytereverse(v, 0, HostAddr(va)); }
inline uint64_t R64(uint32_t va) { return __loaddoublewordbytereverse(0, HostAddr(va)); }
inline void W64(uint32_t va, uint64_t v) { __storedoublewordbytereverse(v, 0, HostAddr(va)); }
// guest float, little endian
inline float RF32(uint32_t va) {
    union { uint32_t u; float f; } c;
    c.u = R32(va);
    return c.f;
}
inline void WF32(uint32_t va, float v) {
    union { uint32_t u; float f; } c;
    c.f = v;
    W32(va, c.u);
}
// locked add on a guest 32 bit value, returns the new value
inline int32_t AtomicAdd32(uint32_t va, int32_t d) {
    uint8_t* base = g_base;
    return int32_t(x86_atomic_rmw<uint32_t>(base, va, uint32_t(d), X86_AT_ADD) + uint32_t(d));
}
inline bool AtomicCas32(uint32_t va, uint32_t* expected, uint32_t desired) {
    uint8_t* base = g_base;
    return x86_atomic_cas<uint32_t>(base, va, expected, desired);
}

// ANSI_STRING / OBJECT_STRING: USHORT Length, USHORT MaximumLength, PCHAR Buffer
std::string ReadAnsiString(uint32_t str);
std::string ReadCString(uint32_t va, size_t max = 4096);

// stdcall argument access for host implementations of guest functions
struct Args {
    X86Context& ctx;
    explicit Args(X86Context& c) : ctx(c) {}
    uint32_t operator[](int i) const { return R32(ctx.esp + 4 + 4 * uint32_t(i)); }

private:
    Args& operator=(const Args&);
};
inline void Return(X86Context& ctx, uint32_t value, int stack_args) {
    ctx.eax = value;
    ctx.esp += 4 + 4 * uint32_t(stack_args);
}

// runs guest code from the host on the current guest thread
// JSRF: so uma thread do jogo por vez (guestlock.cpp). GuestUnlock em tudo que bloqueia no host.
void GuestAcquire();
void GuestRelease();
void GuestYield();
// JSRF: a thread que chama espera a vez girando (so a do jogo); e o tempo que a thread esperou desde a ultima leitura
void GuestSpinHere();
int64_t GuestWaitNs();
bool GuestHeld();
struct GuestUnlock {
    bool was;
    GuestUnlock() : was(GuestHeld()) { if (was) GuestRelease(); }
    ~GuestUnlock() { if (was) GuestAcquire(); }
};

uint32_t CallGuest(X86Context& ctx, uint32_t fn, const uint32_t* args, size_t count);
inline uint32_t CallGuest(X86Context& ctx, uint32_t fn) { return CallGuest(ctx, fn, NULL, 0); }
inline uint32_t CallGuest(X86Context& ctx, uint32_t fn, uint32_t a0) { return CallGuest(ctx, fn, &a0, 1); }
inline uint32_t CallGuest(X86Context& ctx, uint32_t fn, uint32_t a0, uint32_t a1) {
    uint32_t a[] = {a0, a1};
    return CallGuest(ctx, fn, a, 2);
}
inline uint32_t CallGuest(X86Context& ctx, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2) {
    uint32_t a[] = {a0, a1, a2};
    return CallGuest(ctx, fn, a, 3);
}
inline uint32_t CallGuest(X86Context& ctx, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    uint32_t a[] = {a0, a1, a2, a3};
    return CallGuest(ctx, fn, a, 4);
}
inline uint32_t CallGuest(X86Context& ctx, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4) {
    uint32_t a[] = {a0, a1, a2, a3, a4};
    return CallGuest(ctx, fn, a, 5);
}
inline uint32_t CallGuest(X86Context& ctx, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4,
                          uint32_t a5) {
    uint32_t a[] = {a0, a1, a2, a3, a4, a5};
    return CallGuest(ctx, fn, a, 6);
}

// objects
enum ObjType { kObjEvent, kObjSemaphore, kObjMutant, kObjTimer, kObjThread, kObjFile, kObjSymLink, kObjDevice, kObjQueue, kObjOther };

struct Object {
    ObjType type;
    uint32_t guest;  // body in guest memory, dispatcher header first for waitables
    // dispatcher state, protected by the dispatcher lock
    int32_t signal;
    int32_t limit;
    bool notification;  // manual reset event or notification timer
    uint32_t owner;     // mutant owner thread
    uint32_t recursion;
    std::string name;
    explicit Object(ObjType t) : type(t), guest(0), signal(0), limit(0), notification(false), owner(0), recursion(0) {}
    virtual ~Object() {}
};
typedef std::shared_ptr<Object> ObjectRef;

inline std::vector<ObjectRef> One(const ObjectRef& o) { return std::vector<ObjectRef>(1, o); }

uint32_t NewHandle(ObjectRef obj);
ObjectRef FromHandle(uint32_t handle);
bool CloseHandle(uint32_t handle);
uint32_t DuplicateHandle(uint32_t handle);
ObjectRef FromPointer(uint32_t guest);  // dispatcher object in guest memory, created on first use
void Bind(ObjectRef obj, uint32_t guest);
ObjectRef FindNamed(const std::string& name);
void AddNamed(const std::string& name, ObjectRef obj);

// dispatcher
std::mutex& DispatchLock();
void SignalChanged();  // wakes every waiter, caller holds the lock
void SetSignal(Object& o, int32_t v);
uint32_t Wait(const std::vector<ObjectRef>& objs, bool wait_all, int64_t timeout_100ns, bool alertable);
int32_t SetEvent(Object& o);
int32_t ResetEvent(Object& o);
int32_t PulseEvent(Object& o);
int32_t ReleaseSemaphore(Object& o, int32_t count, uint32_t& status);
int32_t ReleaseMutant(Object& o, uint32_t& status);
int64_t Now100ns();                     // system time, 100 ns since 1601
int64_t ToDeadline(int64_t due_100ns);  // relative or absolute to a steady clock deadline in ns
void QueueDpc(uint32_t dpc, uint32_t arg1, uint32_t arg2);
struct Thread;
// apc_routine runs on apc_thread with (apc_context, due low, due high) each time the timer fires
void SetTimer(ObjectRef timer, int64_t due, int32_t period_ms, uint32_t dpc,
              std::shared_ptr<Thread> apc_thread = std::shared_ptr<Thread>(), uint32_t apc_routine = 0,
              uint32_t apc_context = 0);
bool CancelTimer(Object& timer);

// threads
struct Apc {
    uint32_t routine, a1, a2, a3;
};
struct Thread : Object {
    uint32_t id;
    uint32_t kpcr, ethread, stack_top, stack_size, tls;
    uint32_t exit_code;
    int priority;
    std::atomic<int> suspend;
    std::mutex apc_lock;
    std::deque<Apc> apcs;
    std::condition_variable resume_cv;
    X86Context* ctx;
    Thread()
        : Object(kObjThread), id(0), kpcr(0), ethread(0), stack_top(0), stack_size(0), tls(0), exit_code(0), priority(0),
          suspend(0), ctx(NULL) {}
};
typedef std::shared_ptr<Thread> ThreadRef;

Thread* Current();
ThreadRef CurrentRef();
void StartHostThread(const char* name, std::function<void(X86Context&)> fn);
void RunDpcs(X86Context& ctx);
void InitKernelData();
void LogGuestStack(X86Context& ctx, int words);
X86Context& CurrentContext();
ThreadRef CreateThread(uint32_t stack_size, uint32_t tls_size, uint32_t system_routine, uint32_t start_routine,
                       uint32_t start_context, bool suspended);
void StartThread(ThreadRef t);
__declspec(noreturn) void ExitThread(uint32_t code);
void Resume(Thread& t);
uint32_t Suspend(Thread& t);  // previous count; another thread stops at its next kernel wait, delay or yield
void SuspendPoint();          // blocks the current thread while it is suspended
bool RunApcs(Thread& t);
void QueueApc(Thread& t, uint32_t routine, uint32_t a1, uint32_t a2, uint32_t a3);
std::vector<ThreadRef> Threads();

// files
void MountDefaults(const std::string& game_dir, const std::string& data_dir);
bool CreateSymbolicLink(const std::string& link, const std::string& target);
bool DeleteSymbolicLink(const std::string& link);
std::string QuerySymbolicLink(const std::string& link);
std::string ResolvePath(const std::string& xbox_path, uint32_t root_handle);  // host path or empty
bool ReadGameFile(const std::string& xbox_path, std::vector<uint8_t>& out);

// boot
bool Boot(const std::string& xbe_path, const std::string& game_dir, const std::string& data_dir);
__declspec(noreturn) void Shutdown(int code);
uint32_t SavedDataAddress();
void SetSavedDataAddress(uint32_t surface);

typedef void (*KernelFn)(X86Context& ctx, uint8_t* base);
void RegisterExport(uint32_t ordinal, const char* name, KernelFn fn);
uint32_t RegisterData(uint32_t ordinal, const char* name, uint32_t size);  // allocates kernel data, returns address
uint32_t DataAddress(uint32_t ordinal);
const char* ExportName(uint32_t ordinal);
bool HasExport(uint32_t ordinal);           // implemented as a function or data
const char* OrdinalName(uint32_t ordinal);  // public name, "?" past the table

// guest kernel variables updated by the host
extern uint32_t g_tick_count_va;
extern uint32_t g_launch_data_page_va;

}  // namespace kernel
