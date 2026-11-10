// Xbox 360 build of the generated code's helpers (VS2010 PowerPC compiler). Same names as
// pc-recomp/sdk/runtime/cpu/x86_ops.h; this copy is found first on the 360 include path.
//
// Guest memory keeps the original Xbox (little endian) byte order: every access goes through the
// PowerPC byte reversed loads and stores. Guest addresses fold into one 256 MB host window with a
// mask, so 0x80000000|pa and 0xF0000000|pa alias pa exactly as the console's mirrors do (see
// kernel/memory.cpp for the layout).
#pragma once
#include <float.h>
#include <limits>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <cstring>
#include <ppcintrinsics.h>
#include <vectorintrinsics.h>

#include "x86_context.h"

extern "C" void x86_esp_jump(const char* fn, uint32_t esp0, uint32_t esp, uint32_t ebp);  // JSRF, diagnostico
// JSRF: cessao do bloqueio das threads do jogo (kernel/guestlock.cpp)
extern volatile long g_guest_preempt;
extern "C" void x86_guest_yield();
#define X86_BACKEDGE() do { if (g_guest_preempt) x86_guest_yield(); } while (0)
#define X86_GUEST_MASK 0x0FFFFFFFu
#define X86_ADDR(a) (base + ((uint32_t)(a) & X86_GUEST_MASK))

// The VS2010 PowerPC optimizer folds a byte reversed load of what a byte reversed store just wrote into the stored
// value without reversing it back (push 0x0C then pop ecx gave 0x0C000000: a rep stosd over the whole stack, the
// console rebooted), a compiler barrier between them does not stop it. Loads are a volatile plain load and a swap
// instead, which it cannot fold.
extern "C" void _ReadWriteBarrier(void);
#pragma intrinsic(_ReadWriteBarrier)
inline uint16_t X86_LOAD16(const uint8_t* p) { return _byteswap_ushort(*(const volatile uint16_t*)p); }
inline uint32_t X86_LOAD32(const uint8_t* p) { return _byteswap_ulong(*(const volatile uint32_t*)p); }
inline uint64_t X86_LOAD64(const uint8_t* p) { return _byteswap_uint64(*(const volatile uint64_t*)p); }

// little endian guest values as lvalues
// JSRF: base e indice separados. O compilador junta os dois em lwbrx/stwbrx rD,rIdx,rBase; com o ponteiro pronto, gerava um add a
// mais em cada acesso, 11% das instrucoes do codigo gerado (conferido no .asm). Leitura segue volatil, pelo motivo acima.
inline uint32_t X86_LD32(const uint8_t* b, uint32_t o) { return _byteswap_ulong(*(const volatile uint32_t*)(b + o)); }
inline uint16_t X86_LD16(const uint8_t* b, uint32_t o) { return _byteswap_ushort(*(const volatile uint16_t*)(b + o)); }
inline uint64_t X86_LD64(const uint8_t* b, uint32_t o) { return _byteswap_uint64(*(const volatile uint64_t*)(b + o)); }
struct X86Le16 {
    uint8_t* p;
    uint32_t o;
    operator uint16_t() const { return X86_LD16(p, o); }
    X86Le16& operator=(uint16_t v) { __storeshortbytereverse(v, o, p); return *this; }
    X86Le16& operator=(const X86Le16& x) { return *this = (uint16_t)x; }
};
struct X86Le32 {
    uint8_t* p;
    uint32_t o;
    operator uint32_t() const { return X86_LD32(p, o); }
    X86Le32& operator=(uint32_t v) { __storewordbytereverse(v, o, p); return *this; }
    X86Le32& operator=(const X86Le32& x) { return *this = (uint32_t)x; }
};
struct X86Le64 {
    uint8_t* p;
    uint32_t o;
    operator uint64_t() const { return X86_LD64(p, o); }
    X86Le64& operator=(uint64_t v) { __storedoublewordbytereverse(v, o, p); return *this; }
    X86Le64& operator=(const X86Le64& x) { return *this = (uint64_t)x; }
};
struct X86LeF32 {
    uint8_t* p;
    uint32_t o;
    operator float() const {
        union { uint32_t u; float f; } c;
        c.u = X86_LD32(p, o);
        return c.f;
    }
    X86LeF32& operator=(float v) {
        union { uint32_t u; float f; } c;
        c.f = v;
        __storewordbytereverse(c.u, o, p);
        return *this;
    }
    X86LeF32& operator=(const X86LeF32& x) { return *this = (float)x; }
};
struct X86LeF64 {
    uint8_t* p;
    uint32_t o;
    operator double() const {
        union { uint64_t u; double f; } c;
        c.u = X86_LD64(p, o);
        return c.f;
    }
    X86LeF64& operator=(double v) {
        union { uint64_t u; double f; } c;
        c.f = v;
        __storedoublewordbytereverse(c.u, o, p);
        return *this;
    }
    X86LeF64& operator=(const X86LeF64& x) { return *this = (double)x; }
};
inline X86Le16 x86_le16(uint8_t* p, uint32_t o = 0) { X86Le16 r = {p, o}; return r; }
inline X86Le32 x86_le32(uint8_t* p, uint32_t o = 0) { X86Le32 r = {p, o}; return r; }
inline X86Le64 x86_le64(uint8_t* p, uint32_t o = 0) { X86Le64 r = {p, o}; return r; }
inline X86LeF32 x86_lef32(uint8_t* p, uint32_t o = 0) { X86LeF32 r = {p, o}; return r; }
inline X86LeF64 x86_lef64(uint8_t* p, uint32_t o = 0) { X86LeF64 r = {p, o}; return r; }

#define X86_MEM8(a) (*(uint8_t*)X86_ADDR(a))
#define X86_MEM16(a) x86_le16(base, (uint32_t)(a) & X86_GUEST_MASK)
#define X86_MEM32(a) x86_le32(base, (uint32_t)(a) & X86_GUEST_MASK)
#define X86_MEM64(a) x86_le64(base, (uint32_t)(a) & X86_GUEST_MASK)
#define X86_MEMF32(a) x86_lef32(base, (uint32_t)(a) & X86_GUEST_MASK)
#define X86_MEMF64(a) x86_lef64(base, (uint32_t)(a) & X86_GUEST_MASK)
// raw host pointer: byte data only
#define X86_PTR(T, a) ((T*)X86_ADDR(a))

// MSVC has no weak symbols: the linker takes impl when no host code defines name
#define X86_STR2(x) #x
#define X86_STR(x) X86_STR2(x)
// no __restrict: the kernel and the HLE write guest memory through g_base, which a restrict promise would let the
// compiler ignore across those calls (stale values, a crash at boot on the console)
#define X86_FUNC(name) extern "C" void name(X86Context& ctx, uint8_t* base)
#define X86_EXTERN_FUNC(name) extern "C" void name(X86Context& ctx, uint8_t* base)
#define X86_WEAK_FUNC(name, impl) __pragma(comment(linker, "/alternatename:" X86_STR(name) "=" X86_STR(impl)))
#define X86_TRACED_FUNC(name, impl, va)                                \
    extern "C" void name(X86Context& ctx, uint8_t* base) {            \
        uint32_t x86_ret = x86_trace(ctx, va, 0);                     \
        impl(ctx, base);                                              \
        x86_trace(ctx, va, x86_ret);                                  \
    }

#define X86_PUSH32(v)            \
    do {                         \
        uint32_t _v = (v);       \
        ctx.esp -= 4;            \
        X86_MEM32(ctx.esp) = _v; \
    } while (0)

#define X86_SAVE_FLAGS() (ctx.cf = cf, ctx.zf = zf, ctx.sf = sf, ctx.of = of, ctx.pf = pf)
#define X86_LOAD_FLAGS() uint32_t cf = ctx.cf, zf = ctx.zf, sf = ctx.sf, of = ctx.of, pf = ctx.pf
#define X86_BARRIER() _ReadWriteBarrier()
// PF: set when the low byte has an even number of ones
#define X86_PARITY(r) ((uint32_t)(((0x6996u >> (((uint32_t)(r) ^ ((uint32_t)(r) >> 4)) & 0xF)) & 1) ^ 1))

// [xbe] noopt functions: the VS2010 PowerPC optimizer has an internal error on them
#define X86_NOOPT_BEGIN __pragma(optimize("", off))
#define X86_NOOPT_END __pragma(optimize("", on))

#define X87_ST(i) ctx.st[(ctx.top + (i)) & 7]

// runtime services, implemented in cpu/dispatch.cpp
void x86_icall(X86Context& ctx, uint8_t* base, uint32_t target);
struct X86Resume {
    uint32_t target, esp;
};
void x86_resume(X86Context& ctx, uint8_t* base, const X86Resume& r);
void x86_kernel(X86Context& ctx, uint8_t* base, uint32_t ordinal);
__declspec(noreturn) void x86_trap(X86Context& ctx, uint32_t va, const char* what);
void x86_priv(X86Context& ctx, uint32_t va, const char* what);
uint32_t x86_trace(X86Context& ctx, uint32_t va, uint32_t ret);
uint64_t x86_rdtsc();
void x86_cpuid(X86Context& ctx);

// C99 math the VS2010 runtime lacks, under the names the generated code uses
inline bool x86_isnan(double v) { return _isnan(v) != 0; }
inline bool x86_signbit(double v) { return _copysign(1.0, v) < 0; }
inline double x86_trunc(double v) { return v < 0 ? ceil(v) : floor(v); }
inline float x86_truncf(float v) { return v < 0 ? ceilf(v) : floorf(v); }
// round half to even, the FPU's default mode
inline double x86_rint(double v) {
    if (x86_isnan(v) || !_finite(v)) return v;
    double f = floor(v), d = v - f;
    if (d > 0.5) return f + 1;
    if (d < 0.5) return f;
    return fmod(f, 2.0) == 0 ? f : f + 1;
}
namespace std {
inline double trunc(double v) { return x86_trunc(v); }
inline float trunc(float v) { return x86_truncf(v); }
inline double exp2(double v) { return pow(2.0, v); }
inline double log2(double v) { return log(v) * 1.4426950408889634; }
inline double remainder(double a, double b) { return a - x86_rint(a / b) * b; }
}  // namespace std

#define X86_INF (std::numeric_limits<double>::infinity())
#define X86_NAN (std::numeric_limits<double>::quiet_NaN())

inline void x87_push(X86Context& ctx, double v) {
    ctx.top = (ctx.top - 1) & 7;
    ctx.st[ctx.top] = v;
}
inline void x87_pop(X86Context& ctx) { ctx.top = (ctx.top + 1) & 7; }

// JSRF: sem ler ctx.fcw e arredondar a cada operacao de ponto flutuante. Os valores voltam a float ao serem gravados.
inline double x87_prec(X86Context&, double v) { return v; }

inline void x87_compare(X86Context& ctx, double a, double b) {
    uint16_t c;
    if (a > b) c = 0;
    else if (a < b) c = 0x0100;
    else if (a == b) c = 0x4000;
    else c = 0x4500;
    ctx.fsw = uint16_t((ctx.fsw & ~0x4500) | c);
}

inline uint16_t x87_status(X86Context& ctx) { return uint16_t((ctx.fsw & ~0x3800) | ((ctx.top & 7) << 11)); }

inline double x87_round(X86Context& ctx, double v) {
    switch ((ctx.fcw >> 10) & 3) {
        case 0: return x86_rint(v);
        case 1: return floor(v);
        case 2: return ceil(v);
        default: return x86_trunc(v);
    }
}

inline int32_t x87_to_i32(X86Context& ctx, double v) {
    double r = x87_round(ctx, v);
    if (!(r >= -2147483648.0 && r <= 2147483647.0)) return INT32_MIN;
    return int32_t(r);
}
inline int16_t x87_to_i16(X86Context& ctx, double v) {
    double r = x87_round(ctx, v);
    if (!(r >= -32768.0 && r <= 32767.0)) return INT16_MIN;
    return int16_t(r);
}
inline int64_t x87_to_i64(X86Context& ctx, double v) {
    double r = x87_round(ctx, v);
    if (!(r >= -9223372036854775808.0 && r < 9223372036854775808.0)) return INT64_MIN;
    return int64_t(r);
}

// 80 bit values in guest memory: 64 bit mantissa then sign and exponent, little endian
inline double x87_load80(uint8_t* base, uint32_t ea) {
    uint64_t mant = X86_MEM64(ea);
    uint16_t se = X86_MEM16(ea + 8);
    int sign = se >> 15;
    int exp = se & 0x7FFF;
    double v;
    if (exp == 0 && mant == 0) v = 0.0;
    else if (exp == 0x7FFF) v = (mant << 1) ? X86_NAN : X86_INF;
    else v = ldexp(double(mant), exp - 16383 - 63);
    return sign ? -v : v;
}

inline void x87_store80(uint8_t* base, uint32_t ea, double v) {
    uint64_t mant = 0;
    uint16_t se = x86_signbit(v) ? 0x8000 : 0;
    double a = fabs(v);
    if (x86_isnan(v)) {
        se |= 0x7FFF;
        mant = 0xC000000000000000ull;
    } else if (!_finite(v)) {
        se |= 0x7FFF;
        mant = 0x8000000000000000ull;
    } else if (a != 0.0) {
        int e;
        double m = frexp(a, &e);
        mant = uint64_t(ldexp(m, 64));
        se |= uint16_t(e - 1 + 16383);
    }
    X86_MEM64(ea) = mant;
    X86_MEM16(ea + 8) = se;
}

inline uint32_t x87_examine(X86Context& ctx) {
    double v = X87_ST(0);
    uint16_t c;
    int cls = _fpclass(v);
    if (x86_isnan(v)) c = 0x0100;
    else if (!_finite(v)) c = 0x0500;
    else if (v == 0.0) c = 0x4000;
    else if (cls & (_FPCLASS_ND | _FPCLASS_PD)) c = 0x4400;
    else c = 0x0400;
    if (x86_signbit(v)) c |= 0x0200;
    ctx.fsw = uint16_t((ctx.fsw & ~0x4700) | c);
    return 0;
}

inline float sse_min(float a, float b) { return a < b ? a : b; }
inline float sse_max(float a, float b) { return a > b ? a : b; }

inline uint32_t sse_cmp(float a, float b, int pred) {
    bool r;
    switch (pred & 7) {
        case 0: r = a == b; break;
        case 1: r = a < b; break;
        case 2: r = a <= b; break;
        case 3: r = x86_isnan(a) || x86_isnan(b); break;
        case 4: r = !(a == b); break;
        case 5: r = !(a < b); break;
        case 6: r = !(a <= b); break;
        default: r = !(x86_isnan(a) || x86_isnan(b)); break;
    }
    return r ? 0xFFFFFFFFu : 0u;
}

inline int32_t sse_cvt(float v) {
    double r = x86_rint(v);
    if (!(r >= -2147483648.0 && r < 2147483648.0)) return INT32_MIN;
    return int32_t(r);
}
inline int32_t sse_cvtt(float v) {
    if (!(v >= -2147483648.0f && v < 2147483648.0f)) return INT32_MIN;
    return int32_t(v);
}

template <typename T>
inline T sat(int64_t v) {
    const int64_t lo = (int64_t)std::numeric_limits<T>::min(), hi = (int64_t)std::numeric_limits<T>::max();
    return T(v < lo ? lo : v > hi ? hi : v);
}

// MMX lanes in x86 order (lane 0 = low bits), independent of the host byte order
template <typename T, int N>
inline void x86_split(T (&out)[N], uint64_t v) {
    for (int i = 0; i < N; i++) out[i] = T(v >> (i * 8 * sizeof(T)));
}
template <typename T> struct X86Unsigned { typedef T type; };
template <> struct X86Unsigned<int8_t> { typedef uint8_t type; };
template <> struct X86Unsigned<int16_t> { typedef uint16_t type; };
template <> struct X86Unsigned<int32_t> { typedef uint32_t type; };
template <typename T, int N>
inline uint64_t x86_join(const T (&in)[N]) {
    uint64_t v = 0;
    for (int i = 0; i < N; i++) v |= uint64_t(typename X86Unsigned<T>::type(in[i])) << (i * 8 * sizeof(T));
    return v;
}

template <typename T, typename F>
inline uint64_t mmx_map2(uint64_t a, uint64_t b, F f) {
    T x[8 / sizeof(T)], y[8 / sizeof(T)], r[8 / sizeof(T)];
    x86_split(x, a);
    x86_split(y, b);
    for (int i = 0; i < int(8 / sizeof(T)); i++) r[i] = T(f(x[i], y[i]));
    return x86_join(r);
}

// 128 bit SSE values: four little endian lanes
inline X86Xmm x86_ldxmm(uint8_t* base, uint32_t ea) {
    X86Xmm v;
    for (int i = 0; i < 4; i++) v.u[i] = X86_MEM32(ea + i * 4);
    return v;
}
inline void x86_stxmm(uint8_t* base, uint32_t ea, const X86Xmm& v) {
    for (int i = 0; i < 4; i++) X86_MEM32(ea + i * 4) = v.u[i];
}
// JSRF: xmm em VMX128 (tools/sse_local.py). Lane i = elemento i; x86_xs_swap inverte os bytes de cada palavra.
static __declspec(align(16)) const uint32_t x86_xs_swap_tab[4] = {0x03020100u, 0x07060504u, 0x0B0A0908u, 0x0F0E0D0Cu};
static __declspec(align(16)) const uint32_t x86_xs_first_tab[4] = {0xFFFFFFFFu, 0, 0, 0};
#define x86_xs_swap() __lvx(x86_xs_swap_tab, 0)
inline __vector4 x86_xs_load(X86Context& ctx, int n) {
    const void* p = &ctx.xmm[n];
    return __vor(__lvlx(p, 0), __lvrx(p, 16));
}
inline void x86_xs_save(X86Context& ctx, int n, __vector4 v) {
    void* p = &ctx.xmm[n];
    __stvlx(v, p, 0);
    __stvrx(v, p, 16);
}
// 16 bytes da memoria do jogo, em qualquer alinhamento
inline __vector4 x86_xs_ldu(uint8_t* base, uint32_t ea) {
    const uint8_t* p = base + (ea & X86_GUEST_MASK);
    __vector4 s = x86_xs_swap();
    __vector4 v = __vor(__lvlx(p, 0), __lvrx(p, 16));
    return __vperm(v, v, s);
}
inline void x86_xs_stu(uint8_t* base, uint32_t ea, __vector4 v) {
    uint8_t* p = base + (ea & X86_GUEST_MASK);
    __vector4 s = x86_xs_swap();
    v = __vperm(v, v, s);
    __stvlx(v, p, 0);
    __stvrx(v, p, 16);
}
// movss: elemento 0, os outros zerados
inline __vector4 x86_xs_lds(uint8_t* base, uint32_t ea) {
    if ((ea & 15) <= 12) {  // os 4 bytes no mesmo bloco de 16: uma carga so, sem tocar no bloco seguinte
        const uint8_t* p = base + (ea & X86_GUEST_MASK);
        __vector4 s = x86_xs_swap();
        __vector4 v = __lvlx(p, 0);
        return __vand(__vperm(v, v, s), __lvx(x86_xs_first_tab, 0));
    }
    return __vand(x86_xs_ldu(base, ea), __lvx(x86_xs_first_tab, 0));
}
inline void x86_xs_sts(uint8_t* base, uint32_t ea, __vector4 v) {
    uint8_t* p = base + (ea & X86_GUEST_MASK);
    __vector4 s = x86_xs_swap();
    v = __vperm(v, v, s);
    if (ea & 3) {  // fora do alinhamento o stvewx gravaria em outro lugar
        __declspec(align(16)) uint32_t w[4];
        __stvx(v, w, 0);
        memcpy(p, &w[0], 4);
        return;
    }
    __stvewx(__vspltw(v, 0), p, 0);
}
// movlps, movhps: duas palavras
inline void x86_xs_stq(uint8_t* base, uint32_t ea, __vector4 v, int high) {
    uint8_t* p = base + (ea & X86_GUEST_MASK);
    __vector4 s = x86_xs_swap();
    v = __vperm(v, v, s);
    __stvewx(high ? __vspltw(v, 2) : __vspltw(v, 0), p, 0);
    __stvewx(high ? __vspltw(v, 3) : __vspltw(v, 1), p + 4, 0);
}
// comiss, elemento 0
inline void x86_xs_comi(__vector4 a, __vector4 b, uint32_t& zf, uint32_t& pf, uint32_t& cf) {
    __vector4 x = __vspltw(a, 0), y = __vspltw(b, 0);
    unsigned int ceq, cgt, clt;
    __vcmpeqfpR(x, y, &ceq);
    __vcmpgtfpR(x, y, &cgt);
    __vcmpgtfpR(y, x, &clt);
    uint32_t eq = (ceq & 0x80) != 0, gt = (cgt & 0x80) != 0, lt = (clt & 0x80) != 0, u = !(eq | gt | lt);
    zf = u | eq;
    pf = u;
    cf = u | lt;
}
inline uint64_t x86_getq(const X86Xmm& x, int k) { return uint64_t(x.u[2 * k]) | (uint64_t(x.u[2 * k + 1]) << 32); }
inline void x86_setq(X86Xmm& x, int k, uint64_t v) {
    x.u[2 * k] = uint32_t(v);
    x.u[2 * k + 1] = uint32_t(v >> 32);
}

inline uint32_t x86_bswap32(uint32_t v) { return _byteswap_ulong(v); }
inline uint32_t x86_clz(uint32_t v) { return _CountLeadingZeros(v); }
inline uint32_t x86_ctz(uint32_t v) { return 31 - _CountLeadingZeros(v & (0 - v)); }

// locked guest memory operations, on the aligned host word that holds the value
uint64_t x86_atomic_cas_raw(uint8_t* p, unsigned size, uint64_t expected, uint64_t desired, bool* ok);
template <typename T>
inline bool x86_atomic_cas(uint8_t* base, uint32_t ea, T* expected, T desired) {
    bool ok;
    T old = (T)x86_atomic_cas_raw(X86_ADDR(ea), sizeof(T), (uint64_t)*expected, (uint64_t)desired, &ok);
    if (!ok) *expected = old;
    return ok;
}
template <typename T>
inline T x86_atomic_load(uint8_t* base, uint32_t ea) {
    __lwsync();
    T v;
    if (sizeof(T) == 1) v = (T)X86_MEM8(ea);
    else if (sizeof(T) == 2) v = (T)(uint16_t)X86_MEM16(ea);
    else if (sizeof(T) == 4) v = (T)(uint32_t)X86_MEM32(ea);
    else v = (T)(uint64_t)X86_MEM64(ea);
    __lwsync();
    return v;
}
enum { X86_AT_XCHG, X86_AT_ADD, X86_AT_OR, X86_AT_AND, X86_AT_XOR };
template <typename T>
inline T x86_atomic_rmw(uint8_t* base, uint32_t ea, T v, int op) {
    T a = x86_atomic_load<T>(base, ea), r;
    do {
        switch (op) {
            case X86_AT_XCHG: r = v; break;
            case X86_AT_ADD: r = T(a + v); break;
            case X86_AT_OR: r = T(a | v); break;
            case X86_AT_AND: r = T(a & v); break;
            default: r = T(a ^ v); break;
        }
    } while (!x86_atomic_cas<T>(base, ea, &a, r));
    return a;
}
#define X86_ATOMIC_LOAD(T, ea) x86_atomic_load<T>(base, ea)
#define X86_ATOMIC_CAS(T, ea, expected, desired) x86_atomic_cas<T>(base, ea, expected, desired)
#define X86_ATOMIC_XCHG(T, ea, v) x86_atomic_rmw<T>(base, ea, (T)(v), X86_AT_XCHG)
#define X86_ATOMIC_XADD(T, ea, v) x86_atomic_rmw<T>(base, ea, (T)(v), X86_AT_ADD)
#define X86_ATOMIC_OR(T, ea, v) x86_atomic_rmw<T>(base, ea, (T)(v), X86_AT_OR)
#define X86_ATOMIC_AND(T, ea, v) x86_atomic_rmw<T>(base, ea, (T)(v), X86_AT_AND)
#define X86_ATOMIC_XOR(T, ea, v) x86_atomic_rmw<T>(base, ea, (T)(v), X86_AT_XOR)
#define X86_FENCE() __sync()

// rep movs / stos: element by element semantics, fast path when the ranges cannot interfere
inline void x86_rep_movs(X86Context& ctx, uint8_t* base, uint32_t w) {
    uint32_t n = ctx.ecx;
    if (!n) return;
    uint32_t bytes = n * w;
    if (!ctx.df && (ctx.edi - ctx.esi >= bytes || ctx.edi <= ctx.esi)) {
        memmove(X86_ADDR(ctx.edi), X86_ADDR(ctx.esi), bytes);
        ctx.esi += bytes;
        ctx.edi += bytes;
        ctx.ecx = 0;
        return;
    }
    int32_t d = ctx.df ? -int32_t(w) : int32_t(w);
    for (; n; n--) {
        memcpy(X86_ADDR(ctx.edi), X86_ADDR(ctx.esi), w);
        ctx.esi += d;
        ctx.edi += d;
    }
    ctx.ecx = 0;
}

inline void x86_rep_stos(X86Context& ctx, uint8_t* base, uint32_t w) {
    uint32_t n = ctx.ecx;
    if (!n) return;
    if (!ctx.df && w == 1) {
        memset(X86_ADDR(ctx.edi), uint8_t(ctx.eax), n);
        ctx.edi += n;
        ctx.ecx = 0;
        return;
    }
    int32_t d = ctx.df ? -int32_t(w) : int32_t(w);
    uint32_t v = ctx.eax;
    for (; n; n--) {
        if (w == 1) X86_MEM8(ctx.edi) = uint8_t(v);
        else if (w == 2) X86_MEM16(ctx.edi) = uint16_t(v);
        else X86_MEM32(ctx.edi) = v;
        ctx.edi += d;
    }
    ctx.ecx = 0;
}
