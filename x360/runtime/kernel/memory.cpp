// Guest memory on the Xbox 360 (layout in memory.h). Same interface and NT semantics as the PC runtime's
// memory.cpp; user virtual memory and contiguous memory share one physical page allocator, so a user address
// and its 0x80000000 mirror are the same bytes.
#include "kernel/memory.h"

#include <map>
#include <mutex>
#include <string.h>
#include <vector>

#include "core/log.h"
#include "cpu/x86_ops.h"
#include "host.h"

namespace mem {

namespace {

uint8_t* g_base = NULL;
std::recursive_mutex g_lock;

inline uint32_t Fold(uint32_t va) { return va & X86_GUEST_MASK; }
inline bool InPhysical(uint32_t va) { return Fold(va) < kPhysicalSize; }

// first fit allocator over a guest range
class Ranges {
public:
    void Init(uint32_t start, uint32_t size) { free_[start] = size; }

    void Remove(uint32_t start, uint32_t size) {
        for (std::map<uint32_t, uint32_t>::iterator it = free_.begin(); it != free_.end(); ++it) {
            uint32_t fs = it->first, fz = it->second;
            if (start >= fs && uint64_t(start) + size <= uint64_t(fs) + fz) {
                free_.erase(it);
                if (start > fs) free_[fs] = start - fs;
                if (uint64_t(start) + size < uint64_t(fs) + fz) free_[start + size] = uint32_t(uint64_t(fs) + fz - start - size);
                return;
            }
        }
    }

    uint32_t Alloc(uint32_t size, uint32_t align, uint32_t lo, uint32_t hi, bool top_down) {
        if (!align) align = 1;
        if (top_down) {
            for (std::map<uint32_t, uint32_t>::reverse_iterator it = free_.rbegin(); it != free_.rend(); ++it) {
                uint64_t fs = it->first, fe = fs + it->second;
                uint64_t e = fe < uint64_t(hi) + 1 ? fe : uint64_t(hi) + 1;
                if (e < size) continue;
                uint64_t a = (e - size) & ~uint64_t(align - 1);
                if (a < fs || a < lo) continue;
                Take(uint32_t(a), size);
                return uint32_t(a);
            }
            return 0;
        }
        for (std::map<uint32_t, uint32_t>::iterator it = free_.begin(); it != free_.end(); ++it) {
            uint64_t fs = it->first, fz = it->second;
            uint64_t a = fs > lo ? fs : lo;
            a = (a + align - 1) & ~uint64_t(align - 1);
            if (a + size <= fs + fz && a + size - 1 <= hi) {
                Take(uint32_t(a), size);
                return uint32_t(a);
            }
        }
        return 0;
    }

    bool IsFree(uint32_t start, uint32_t size) const {
        std::map<uint32_t, uint32_t>::const_iterator it = free_.upper_bound(start);
        if (it == free_.begin()) return false;
        --it;
        return uint64_t(start) + size <= uint64_t(it->first) + it->second;
    }

    bool Free(uint32_t a, uint32_t* size_out) {
        std::map<uint32_t, uint32_t>::iterator it = used_.find(a);
        if (it == used_.end()) return false;
        uint32_t size = it->second;
        used_.erase(it);
        if (size_out) *size_out = size;
        Release(a, size);
        return true;
    }

    void Release(uint32_t a, uint32_t size) {
        std::map<uint32_t, uint32_t>::iterator n = free_.insert(std::make_pair(a, size)).first;
        std::map<uint32_t, uint32_t>::iterator next = n;
        ++next;
        if (next != free_.end() && n->first + n->second == next->first) {
            n->second += next->second;
            free_.erase(next);
        }
        if (n != free_.begin()) {
            std::map<uint32_t, uint32_t>::iterator prev = n;
            --prev;
            if (prev->first + prev->second == n->first) {
                prev->second += n->second;
                free_.erase(n);
            }
        }
    }

    void Forget(uint32_t a) { used_.erase(a); }

    uint32_t SizeOf(uint32_t a) const {
        std::map<uint32_t, uint32_t>::const_iterator it = used_.find(a);
        return it == used_.end() ? 0 : it->second;
    }

    void Dump() const {
        uint64_t n = 0; uint32_t big = 0;
        for (std::map<uint32_t, uint32_t>::const_iterator it = free_.begin(); it != free_.end(); ++it) { n += it->second; if (it->second > big) big = it->second; }
        LOGE("memory:  livre %u KB em %u blocos, maior %X", unsigned(n >> 10), unsigned(free_.size()), big);
        for (std::map<uint32_t, uint32_t>::const_iterator it = used_.begin(); it != used_.end(); ++it)
            if (it->second >= 0x80000) LOGE("memory:  fisica %08X %X", it->first, it->second);
    }

    uint64_t Used() const {
        uint64_t n = 0;
        for (std::map<uint32_t, uint32_t>::const_iterator it = used_.begin(); it != used_.end(); ++it) n += it->second;
        return n;
    }

private:
    void Take(uint32_t a, uint32_t size) {
        Remove(a, size);
        used_[a] = size;
    }
    std::map<uint32_t, uint32_t> free_;
    std::map<uint32_t, uint32_t> used_;
};

// physical pages (user and contiguous), system pool, stacks: guest addresses as the guest sees them
Ranges g_phys, g_system, g_stacks;
// JSRF: memoria virtual do jogo fora dos 64 MB fisicos, como faz o Unleashed no PC (heap e contigua separados). Medido:
// 32,8 MB de usuario + 31,5 MB contiguos encheram os 64 MB no carregamento (2026-11-04). O console tem 512 MB; esta
// faixa da janela estava livre. Nada a enxerga pelo endereco 0x80000000: quem precisa de GPU usa memoria contigua.
const uint32_t kUserExtraBase = 0x08000000, kUserExtraSize = 0x05000000;  // 80 MB
Ranges g_user_extra;
inline bool InUserExtra(uint32_t a) { return a >= kUserExtraBase && a - kUserExtraBase < kUserExtraSize; }
std::map<uint32_t, uint32_t> g_stack_tops;

struct Reservation {
    uint32_t size;
    uint32_t protect;
    std::vector<uint8_t> commit;  // per page
};
std::map<uint32_t, Reservation> g_reserved;

// 64 KB pages instead of 4 KB for the window: the title's data spans 64 MB, which 4 KB pages make a TLB miss on
// most of its accesses on the Xbox 360 (the TLB maps 4 MB of 4 KB pages, 64 MB of 64 KB ones)
const bool kLargePages = true;
bool g_large_pages = false;

// committed host pages of the window, one bit per 64 KB
std::vector<uint8_t> g_host_committed(kWindowSize >> 16, 0);

bool CommitHost(uint32_t off, uint32_t size) {
    uint32_t first = off & ~0xFFFFu, end = (off + size + 0xFFFF) & ~0xFFFFu;
    for (uint32_t p = first; p < end; p += 0x10000) {
        if (g_host_committed[p >> 16]) continue;
        if (!VirtualAlloc(g_base + p, 0x10000, MEM_COMMIT | (g_large_pages ? MEM_LARGE_PAGES : 0), PAGE_READWRITE)) {
            LOGE("memory: commit of window %08X failed", p);
            return false;
        }
        memset(g_base + p, 0, 0x10000);
        g_host_committed[p >> 16] = 1;
    }
    return true;
}

}  // namespace

bool Init() {
    if (kLargePages) {
        g_base = static_cast<uint8_t*>(VirtualAlloc(NULL, kWindowSize, MEM_RESERVE | MEM_LARGE_PAGES, PAGE_READWRITE));
        g_large_pages = g_base != NULL;
    }
    if (!g_base) g_base = static_cast<uint8_t*>(VirtualAlloc(NULL, kWindowSize, MEM_RESERVE, PAGE_READWRITE));
    LOGI("memory: guest window in %s pages", g_large_pages ? "64 KB" : "4 KB");
    if (!g_base) {
        LOGE("memory: could not reserve the 256 MB guest window");
        return false;
    }
    // the guest's 64 MB, kernel data and the hardware register sinks are there from the start
    bool ok = CommitHost(0, kPhysicalSize) && CommitHost(Fold(kKernelDataBase), kKernelDataSize) &&
              CommitHost(0x0D000000, 0x03000000);
    if (!ok) return false;
    g_phys.Init(kUserLow, kPhysicalSize - kUserLow);
    g_user_extra.Init(kUserExtraBase, kUserExtraSize);
    g_system.Init(kSystemBase, kSystemSize);
    g_stacks.Init(kStackBase, kStackSize);
    LOGI("memory: guest window at %p", static_cast<void*>(g_base));
    return true;
}

uint8_t* Base() { return g_base; }

bool Commit(uint32_t va, uint32_t size) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (!size) return true;
    return CommitHost(Fold(va), size);
}

// pages stay committed: the window is ours alone and a decommit would only cost a later fault
void Decommit(uint32_t, uint32_t) {}

bool IsCommitted(uint32_t va) { return g_host_committed[Fold(va) >> 16] != 0; }

static Reservation* FindReservation(uint32_t addr, uint32_t* start) {
    std::map<uint32_t, Reservation>::iterator it = g_reserved.upper_bound(addr);
    if (it == g_reserved.begin()) return NULL;
    --it;
    if (addr >= it->first + it->second.size) return NULL;
    *start = it->first;
    return &it->second;
}

void ReserveImage(uint32_t base, uint32_t size) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t b = base & ~0xFFFFu;
    uint32_t e = (base + size + 0xFFFF) & ~0xFFFFu;
    g_phys.Remove(b, e - b);
    Reservation r;
    r.size = e - b;
    r.protect = 4;  // PAGE_READWRITE
    r.commit.assign((e - b) / kPage, 1);
    g_reserved[b] = r;
}

uint32_t AllocateVirtual(uint32_t& base, uint32_t& size, uint32_t type, uint32_t protect) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (!size) return 0xC000000D;  // STATUS_INVALID_PARAMETER
    bool reserve = (type & kMemReserve) != 0;
    bool commit = (type & kMemCommit) != 0;
    if (!base && commit) reserve = true;
    if (reserve) {
        uint32_t sz = (size + 0xFFFF) & ~0xFFFFu;
        uint32_t a;
        if (base) {
            a = base & ~0xFFFFu;
            sz = ((base + size + 0xFFFF) & ~0xFFFFu) - a;
            if (a >= kPhysicalSize || !g_phys.IsFree(a, sz)) return 0xC0000018;  // STATUS_CONFLICTING_ADDRESSES
            g_phys.Remove(a, sz);
        } else {
            // user memory from the bottom, contiguous memory from the top
            a = g_user_extra.Alloc(sz, 0x10000, kUserExtraBase, kUserExtraBase + kUserExtraSize - 1, (type & kMemTopDown) != 0);
            if (a) {
                g_user_extra.Forget(a);
            } else {
                a = g_phys.Alloc(sz, 0x10000, kUserLow, kPhysicalSize - 1, (type & kMemTopDown) != 0);
                if (a) g_phys.Forget(a);
            }
            if (!a) {
                { static bool told = false; if (!told) { told = true;  // JSRF: quem ocupa os 64 MB
                    uint64_t res = 0; for (std::map<uint32_t, Reservation>::iterator i = g_reserved.begin(); i != g_reserved.end(); ++i) {
                        res += i->second.size; if (i->second.size >= 0x80000) LOGE("memory:  reserva %08X %X", i->first, i->second.size); }
                    LOGE("memory:  reservado %u KB em %u blocos", unsigned(res >> 10), unsigned(g_reserved.size()));
                    g_phys.Dump(); } }
                LOGE("memory: user allocation of %X bytes failed, %u KB of physical memory in use", sz,
                     unsigned(g_phys.Used() >> 10));
                return 0xC0000017;  // STATUS_NO_MEMORY
            }
        }
        Reservation r;
        r.size = sz;
        r.protect = protect;
        r.commit.assign(sz / kPage, 0);
        g_reserved[a] = r;
        base = a;
        size = sz;
    }
    if (commit) {
        uint32_t start;
        Reservation* r = FindReservation(base, &start);
        if (!r) return 0xC0000045;
        uint32_t first = base & ~(kPage - 1);
        uint32_t end = (base + size + kPage - 1) & ~(kPage - 1);
        if (end > start + r->size) return 0xC0000018;
        if (InUserExtra(first) && !CommitHost(Fold(first), end - first)) return 0xC0000017;
        for (uint32_t p = first; p < end; p += kPage) {
            uint8_t& c = r->commit[(p - start) / kPage];
            if (!c && !(type & kMemNoZero)) memset(g_base + Fold(p), 0, kPage);  // fresh pages read zero
            c = 1;
        }
        base = first;
        size = end - first;
    }
    return 0;
}

uint32_t FreeVirtual(uint32_t& base, uint32_t& size, uint32_t type) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t start;
    Reservation* r = FindReservation(base, &start);
    if (!r) return 0xC000009F;  // STATUS_MEMORY_NOT_ALLOCATED
    if (type & kMemRelease) {
        uint32_t sz = r->size;
        g_reserved.erase(start);
        if (InUserExtra(start)) g_user_extra.Release(start, sz);
        else g_phys.Release(start, sz);
        base = start;
        size = sz;
        return 0;
    }
    uint32_t first = base & ~(kPage - 1);
    uint32_t end = size ? (base + size + kPage - 1) & ~(kPage - 1) : start + r->size;
    for (uint32_t p = first; p < end; p += kPage) r->commit[(p - start) / kPage] = 0;
    base = first;
    size = end - first;
    return 0;
}

bool QueryVirtual(uint32_t addr, RegionInfo& out) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t start;
    Reservation* r = FindReservation(addr, &start);
    memset(&out, 0, sizeof(out));
    if (!r) {
        out.base = addr & ~(kPage - 1);
        std::map<uint32_t, Reservation>::iterator it = g_reserved.upper_bound(addr);
        uint32_t next = it == g_reserved.end() ? 0x7FFE0000 : it->first;
        out.size = next > out.base ? next - out.base : kPage;
        out.state = 0x10000;  // MEM_FREE
        out.protect = 1;      // PAGE_NOACCESS
        return addr < 0x7FFE0000;
    }
    uint32_t page = (addr - start) / kPage;
    uint8_t c = r->commit[page];
    uint32_t n = page;
    while (n < r->commit.size() && r->commit[n] == c) n++;
    out.base = start + page * kPage;
    out.alloc_base = start;
    out.alloc_protect = r->protect;
    out.size = (n - page) * kPage;
    out.state = c ? kMemCommit : kMemReserve;
    out.protect = c ? r->protect : 0;
    out.type = 0x20000;  // MEM_PRIVATE
    return true;
}

uint32_t AllocContiguous(uint32_t size, uint32_t lowest, uint32_t highest, uint32_t align) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    size = (size + kPage - 1) & ~(kPage - 1);
    if (!size) size = kPage;
    if (align < kPage) align = kPage;
    uint32_t lo = lowest & (kPhysicalSize - 1);
    if (lo < kUserLow) lo = kUserLow;
    uint32_t hi = highest >= kPhysicalSize - 1 ? kPhysicalSize - 1 : highest;
    // highest fit first: low memory stays free for requests limited to it and for user memory
    uint32_t pa = g_phys.Alloc(size, align, lo, hi, true);
    if (!pa) {
        LOGE("memory: contiguous allocation of %X bytes failed (%X..%X align %X), %u KB in use", size, lowest, highest,
             align, unsigned(g_phys.Used() >> 10));
        return 0;
    }
    memset(g_base + pa, 0, size);
    return kContiguousBase | pa;
}

void FreeContiguous(uint32_t va) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_phys.Free(va & (kPhysicalSize - 1), NULL);
}

uint32_t AllocSystem(uint32_t size, uint32_t align) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    size = (size + 15) & ~15u;
    if (!size) size = 16;
    uint32_t a = g_system.Alloc(size, align > 16 ? align : 16, kSystemBase, kSystemBase + kSystemSize - 1, false);
    if (!a) {
        LOGE("memory: system pool allocation of %X bytes failed", size);
        return 0;
    }
    CommitHost(Fold(a), size);
    memset(g_base + Fold(a), 0, size);
    return a;
}

void FreeSystem(uint32_t va) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_system.Free(va, NULL);
}

uint32_t AllocStack(uint32_t size) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    size = (size + 0xFFFF) & ~0xFFFFu;
    uint32_t a = g_stacks.Alloc(size, 0x10000, kStackBase, kStackBase + kStackSize - 1, false);
    if (!a) {
        LOGE("memory: stack allocation of %X bytes failed", size);
        return 0;
    }
    CommitHost(Fold(a), size);
    g_stack_tops[a + size] = a;
    return a + size;
}

void FreeStack(uint32_t top) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    std::map<uint32_t, uint32_t>::iterator it = g_stack_tops.find(top);
    if (it == g_stack_tops.end()) return;
    g_stacks.Free(it->second, NULL);
    g_stack_tops.erase(it);
}

uint32_t AllocationSize(uint32_t va) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (InPhysical(va)) {
        if (uint32_t s = g_phys.SizeOf(Fold(va))) return s;
    }
    if (uint32_t s = g_system.SizeOf(va)) return s;
    uint32_t start;
    if (Reservation* r = FindReservation(va, &start)) return r->size - (va - start);
    return 0;
}

uint32_t PhysicalFromVirtual(uint32_t va) {
    if (InPhysical(va)) return Fold(va);
    return va;
}

uint32_t VirtualFromPhysical(uint32_t pa) {
    if (pa < kPhysicalSize) return kContiguousBase | pa;
    return pa;
}

Stats Query() {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Stats s;
    memset(&s, 0, sizeof(s));
    s.total_pages = kPhysicalSize / kPage;
    uint64_t vc = 0;
    for (std::map<uint32_t, Reservation>::iterator it = g_reserved.begin(); it != g_reserved.end(); ++it)
        for (size_t i = 0; i < it->second.commit.size(); i++) vc += it->second.commit[i];
    s.virtual_committed = uint32_t(vc);
    s.contiguous_used = uint32_t(g_phys.Used() / kPage);
    s.system_used = uint32_t(g_system.Used() / kPage);
    uint64_t used = vc + s.contiguous_used;
    s.available_pages = used >= s.total_pages ? 0 : uint32_t(s.total_pages - used);
    return s;
}

}  // namespace mem
