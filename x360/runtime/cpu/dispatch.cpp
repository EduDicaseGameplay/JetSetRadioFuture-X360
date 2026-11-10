#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <unordered_set>
#include <vector>

#include "core/log.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"
#include "host.h"


namespace {

std::vector<X86Func*> g_table;  // one slot per code byte
std::vector<bool> g_continuation;
uint32_t g_lo = 0, g_hi = 0;

struct Export {
    const char* name;
    kernel::KernelFn fn;
    uint32_t data;
};
Export g_exports[400];

}  // namespace

// synthetic guest addresses for kernel functions reached through a pointer
const uint32_t kKernelThunkBase = 0xFE000000;

namespace dispatch {

void Init() {
    g_lo = X86CodeBase;
    g_hi = X86CodeEnd;
    g_table.assign(g_hi - g_lo, nullptr);
    size_t n = 0;
    for (const X86FuncMapping* m = X86FuncMappings; m->host; m++, n++)
        if (m->guest >= g_lo && m->guest < g_hi) g_table[m->guest - g_lo] = m->host;
    g_continuation.assign(g_hi - g_lo, false);
    for (const uint32_t* c = X86Continuations; *c; c++)
        if (*c >= g_lo && *c < g_hi) g_continuation[*c - g_lo] = true;
    LOGI("dispatch: %u functions over %08X..%08X", n, g_lo, g_hi);
}

X86Func* Find(uint32_t guest) {
    if (guest >= g_lo && guest < g_hi) return g_table[guest - g_lo];
    return nullptr;
}

// host code the guest may call through a pointer gets an address in this range
const uint32_t kHostThunkBase = 0xFD000000;
static std::vector<X86Func*> g_host_thunks;
static std::mutex g_host_lock;

uint32_t HostThunk(X86Func* fn) {
    std::lock_guard<std::mutex> lock(g_host_lock);
    for (size_t i = 0; i < g_host_thunks.size(); i++)
        if (g_host_thunks[i] == fn) return kHostThunkBase + uint32_t(i) * 4;
    g_host_thunks.push_back(fn);
    return kHostThunkBase + uint32_t(g_host_thunks.size() - 1) * 4;
}

uint32_t KernelThunkAddress(uint32_t ordinal) { return kKernelThunkBase + ordinal * 4; }

}  // namespace dispatch

// JSRF, diagnostico: ultimas chamadas indiretas de cada thread, para quem achar um esp fora da pilha (main360.cpp)
namespace kernel {
static uint32_t g_recent[64][16], g_recent_esp[64][16];
static volatile uint32_t g_recent_n[64];
void LogRecentCalls(uint32_t id) {
    uint32_t s = (id >> 2) & 63, n = g_recent_n[s];
    for (uint32_t i = 0; i < 16; i++) {
        uint32_t k = (n - 16 + i) & 15;
        LOGE("pilha:   recente %08X esp %08X", g_recent[s][k], g_recent_esp[s][k]);
    }
}
}  // namespace kernel
static inline void NoteCall(X86Context& ctx, uint32_t target) {
    kernel::Thread* me = kernel::Current();
    if (!me) return;
    uint32_t s = (me->id >> 2) & 63, n = kernel::g_recent_n[s];
    kernel::g_recent[s][n & 15] = target;
    kernel::g_recent_esp[s][n & 15] = ctx.esp;
    kernel::g_recent_n[s] = n + 1;
}

// JSRF, diagnostico: funcao cujo esp andou demais desde a entrada
extern "C" void x86_esp_jump(const char* fn, uint32_t esp0, uint32_t esp, uint32_t ebp) {
    static volatile long told = 0;
    if (told >= 8) return;
    told++;
    LOGE("pilha: %s entrou com esp %08X e esta com %08X, ebp %08X", fn, esp0, esp, ebp);
}

// JSRF: o jogo e C++ e usa muita funcao virtual. Cada uma consultava g_continuation e g_table, tabelas de varios MB com uma
// posicao por byte de codigo, com falta de cache quase sempre. Agora passa antes por 4096 pares endereco/funcao, em 64 bits para
// uma thread nunca ler metade de um par escrito por outra.
__declspec(align(128)) static volatile uint64_t g_icall_cache[4096];

void x86_icall(X86Context& ctx, uint8_t* base, uint32_t target) {
    if (g_guest_preempt) kernel::GuestYield();
    volatile uint64_t& slot = g_icall_cache[(target ^ (target >> 12)) & 4095];
    uint64_t e = slot;
    if (uint32_t(e >> 32) == target) {
        reinterpret_cast<X86Func*>(uintptr_t(uint32_t(e)))(ctx, base);
        return;
    }
    ctx.last_icall = target;
    if (target >= g_lo && target < g_hi && g_continuation[target - g_lo]) { X86Resume r = {target, ctx.esp}; throw r; }
    if (X86Func* f = dispatch::Find(target)) {
        slot = uint64_t(target) << 32 | uint32_t(uintptr_t(f));
        f(ctx, base);
        return;
    }
    if (target >= kKernelThunkBase && target < kKernelThunkBase + 400 * 4) {
        x86_kernel(ctx, base, (target - kKernelThunkBase) / 4);
        return;
    }
    if (target >= dispatch::kHostThunkBase) {
        size_t i = (target - dispatch::kHostThunkBase) / 4;
        X86Func* f = nullptr;
        {
            std::lock_guard<std::mutex> lock(dispatch::g_host_lock);
            if (i < dispatch::g_host_thunks.size()) f = dispatch::g_host_thunks[i];
        }
        if (f) {
            f(ctx, base);
            return;
        }
    }
    uint32_t ret = kernel::R32(ctx.esp);
    LOGE("indirect call to %08X has no recompiled function (return address %08X, esp %08X)", target, ret, ctx.esp);
    LOGE("add 0x%X to [functions] in the image's config/*.toml and rebuild", target);
    x86_trap(ctx, ret, "indirect call to unknown code");
}

void x86_resume(X86Context& ctx, uint8_t* base, const X86Resume& r) {
    ctx.esp = r.esp;
    dispatch::Find(r.target)(ctx, base);
}

void x86_kernel(X86Context& ctx, uint8_t* base, uint32_t ordinal) {
    if (ordinal < 400 && g_exports[ordinal].fn) {
        g_exports[ordinal].fn(ctx, base);
        return;
    }
    LOGE("kernel export %u (%s) is not implemented, return address %08X", ordinal, kernel::ExportName(ordinal),
         kernel::R32(ctx.esp));
    x86_trap(ctx, kernel::R32(ctx.esp), "unimplemented kernel export");
}

namespace kernel {

void RegisterExport(uint32_t ordinal, const char* name, KernelFn fn) {
    g_exports[ordinal].name = name;
    g_exports[ordinal].fn = fn;
}

static uint32_t g_kdata_next = mem::kKernelDataBase + 0x1000;

uint32_t RegisterData(uint32_t ordinal, const char* name, uint32_t size) {
    static std::mutex m;
    std::lock_guard<std::mutex> lock(m);
    if (g_exports[ordinal].data) return g_exports[ordinal].data;
    uint32_t a = (g_kdata_next + 15) & ~15u;
    g_kdata_next = a + ((size + 15) & ~15u);
    g_exports[ordinal].name = name;
    g_exports[ordinal].data = a;
    return a;
}

uint32_t DataAddress(uint32_t ordinal) { return ordinal < 400 ? g_exports[ordinal].data : 0; }
const char* ExportName(uint32_t ordinal) { return ordinal < 400 && g_exports[ordinal].name ? g_exports[ordinal].name : OrdinalName(ordinal); }
bool HasExport(uint32_t ordinal) { return ordinal < 400 && (g_exports[ordinal].fn || g_exports[ordinal].data); }

uint32_t CallGuest(X86Context& ctx, uint32_t fn, const uint32_t* args, size_t count) {
    // the D3D and DirectSound hooks call this several times a draw: no heap, only the registers put back are kept
    struct Saved {
        uint32_t eax, ecx, edx, ebx, esi, edi, ebp;
    } saved = {ctx.eax, ctx.ecx, ctx.edx, ctx.ebx, ctx.esi, ctx.edi, ctx.ebp};
    uint32_t esp = ctx.esp;
    const bool took = !GuestHeld();  // entrada vinda do host: thread nova, DPC, callback
    if (took) GuestAcquire();
    for (const uint32_t* p = args + count; p != args;) {
        ctx.esp -= 4;
        W32(ctx.esp, *--p);
    }
    ctx.esp -= 4;
    W32(ctx.esp, 0xFFFFFFF0u);  // never used as a target, calls return through the host stack
    x86_icall(ctx, g_base, fn);
    uint32_t result = ctx.eax;
    ctx.eax = saved.eax;
    ctx.ecx = saved.ecx;
    ctx.edx = saved.edx;
    ctx.ebx = saved.ebx;
    ctx.esi = saved.esi;
    ctx.edi = saved.edi;
    ctx.ebp = saved.ebp;
    ctx.esp = esp;
    if (took) GuestRelease();
    return result;
}

}  // namespace kernel

namespace kernel {
// return addresses found on the guest stack, the cheapest way to place a guest event
void LogGuestStack(X86Context& ctx, int words) {
    LOGE("  eax %08X ecx %08X edx %08X ebx %08X esp %08X ebp %08X esi %08X edi %08X", ctx.eax, ctx.ecx, ctx.edx, ctx.ebx,
         ctx.esp, ctx.ebp, ctx.esi, ctx.edi);
    for (int i = 0; i < words; i++) {
        uint32_t a = ctx.esp + uint32_t(i) * 4;
        if (!mem::IsCommitted(a)) break;
        uint32_t v = R32(a);
        if (v >= X86CodeBase && v < X86CodeEnd) LOGE("  [esp+%03X] %08X", i * 4, v);
    }
}
}  // namespace kernel

void x86_trap(X86Context& ctx, uint32_t va, const char* what) {
    LOGE("guest trap at %08X: %s", va, what);
    kernel::LogGuestStack(ctx, 64);
    logx::Flush();
    // JSRF: __debugbreak e abort reiniciam o console. Voltar ao painel, com o log ja gravado.
    kernel::Shutdown(2);
    for (;;) {}
}

uint32_t x86_trace(X86Context& ctx, uint32_t va, uint32_t ret) {
    using kernel::R32;
    if (!ret) {
        ret = R32(ctx.esp);
        LOGI("trace %08X from %08X (%08X, %08X, %08X, %08X) ecx %08X", va, ret, R32(ctx.esp + 4), R32(ctx.esp + 8),
             R32(ctx.esp + 12), R32(ctx.esp + 16), ctx.ecx);
    } else {
        LOGI("trace %08X returned %08X to %08X", va, ctx.eax, ret);
    }
    return ret;
}

void x86_priv(X86Context& ctx, uint32_t va, const char* what) {
    static std::mutex m;
    static std::unordered_set<uint32_t> seen;
    std::lock_guard<std::mutex> lock(m);
    if (seen.insert(va).second) LOGW("privileged instruction %s at %08X ignored", what, va);
    (void)ctx;
}

// the guest measured time against a 733.33 MHz time stamp counter
uint64_t x86_rdtsc() {
    static const int64_t start = host::NowNs();
    int64_t ns = host::NowNs() - start;
    uint64_t sec = uint64_t(ns) / 1000000000ull, rem = uint64_t(ns) % 1000000000ull;
    return sec * 733333333ull + rem * 733333333ull / 1000000000ull;
}

void x86_cpuid(X86Context& ctx) {
    switch (ctx.eax) {
        case 0:
            ctx.eax = 2;
            ctx.ebx = 0x756E6547;  // GenuineIntel
            ctx.edx = 0x49656E69;
            ctx.ecx = 0x6C65746E;
            break;
        case 1:
            ctx.eax = 0x0000068A;  // family 6 model 8, Pentium III Coppermine
            ctx.ebx = 0;
            ctx.ecx = 0;
            ctx.edx = 0x0383F9FF;  // fpu tsc cx8 cmov mmx fxsr sse
            break;
        default:
            ctx.eax = ctx.ebx = ctx.ecx = ctx.edx = 0;
            break;
    }
}
