// Locked operations on little endian guest memory (cpu/x86_ops.h): a compare exchange on the aligned host word
// that holds the value, with the values byte reversed into memory order.
#include <string.h>

#include "cpu/x86_ops.h"
#include "host.h"

namespace {

// memory order bytes of an aligned word, read and written as one
inline uint32_t LoadRaw(volatile LONG* w) { return uint32_t(*w); }

host::Mutex g_unaligned;

}  // namespace

uint64_t x86_atomic_cas_raw(uint8_t* p, unsigned size, uint64_t expected, uint64_t desired, bool* ok) {
    uintptr_t addr = reinterpret_cast<uintptr_t>(p);
    if (size == 4 && !(addr & 3)) {
        LONG e = LONG(_byteswap_ulong(uint32_t(expected)));
        LONG old = InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(p), LONG(_byteswap_ulong(uint32_t(desired))), e);
        *ok = old == e;
        return _byteswap_ulong(uint32_t(old));
    }
    if (size == 8 && !(addr & 7)) {
        LONGLONG e = LONGLONG(_byteswap_uint64(expected));
        LONGLONG old = InterlockedCompareExchange64(reinterpret_cast<volatile LONGLONG*>(p), LONGLONG(_byteswap_uint64(desired)), e);
        *ok = old == e;
        return _byteswap_uint64(uint64_t(old));
    }
    if ((size == 1 || size == 2) && (addr & 3) + size <= 4) {
        volatile LONG* w = reinterpret_cast<volatile LONG*>(addr & ~uintptr_t(3));
        unsigned at = unsigned(addr & 3);
        for (;;) {
            uint32_t raw = LoadRaw(w);
            uint8_t bytes[4];
            memcpy(bytes, &raw, 4);  // host big endian: bytes[] is memory order
            uint64_t cur = size == 1 ? bytes[at] : uint64_t(bytes[at] | bytes[at + 1] << 8);
            if (cur != expected) {
                *ok = false;
                return cur;
            }
            bytes[at] = uint8_t(desired);
            if (size == 2) bytes[at + 1] = uint8_t(desired >> 8);
            uint32_t next;
            memcpy(&next, bytes, 4);
            if (InterlockedCompareExchange(w, LONG(next), LONG(raw)) == LONG(raw)) {
                *ok = true;
                return cur;
            }
        }
    }
    // unaligned: a lock against the other unaligned operations is the best the host can do
    g_unaligned.lock();
    uint64_t cur = 0;
    for (unsigned i = 0; i < size; i++) cur |= uint64_t(p[i]) << (8 * i);
    *ok = cur == expected;
    if (*ok)
        for (unsigned i = 0; i < size; i++) p[i] = uint8_t(desired >> (8 * i));
    g_unaligned.unlock();
    return cur;
}
