#pragma once
#include <stdint.h>
#include <string.h>
#include <string>
#include <vector>

namespace xbe {

// the header XORs the entry point and kernel thunk address with these
const uint32_t kRetailEntryKey = 0xA8FC57AB;
const uint32_t kDebugEntryKey = 0x94859D4B;
const uint32_t kRetailThunkKey = 0x5B6D40B6;
const uint32_t kDebugThunkKey = 0xEFB1F152;

enum SectionFlags : uint32_t {
    kWritable = 1,
    kPreload = 2,
    kExecutable = 4,
    kInsertedFile = 8,
};

struct Section {
    std::string name;
    uint32_t flags;
    uint32_t va, vsize;
    uint32_t raw, rsize;
    Section() : flags(0), va(0), vsize(0), raw(0), rsize(0) {}
};

struct Library {
    std::string name;
    uint16_t major, minor, build, flags;
    Library() : major(0), minor(0), build(0), flags(0) {}
};

struct Tls {
    uint32_t data_start, data_end, index_addr, callbacks, zero_fill, characteristics;
    Tls() : data_start(0), data_end(0), index_addr(0), callbacks(0), zero_fill(0), characteristics(0) {}
};

struct Image {
    std::vector<uint8_t> file;
    uint32_t base, header_size, image_size;
    uint32_t entry, kernel_thunk, tls_addr;
    uint32_t stack_commit, heap_reserve, heap_commit;
    uint32_t cert, title_id, init_flags;
    bool debug;
    std::vector<Section> sections;
    std::vector<Library> libraries;
    Tls tls;
    Image()
        : base(0), header_size(0), image_size(0), entry(0), kernel_thunk(0), tls_addr(0), stack_commit(0),
          heap_reserve(0), heap_commit(0), cert(0), title_id(0), init_flags(0), debug(false) {}

    uint32_t U32(uint32_t off) const {
        const uint8_t* p = &file[off];
        return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
    }
    uint16_t U16(uint32_t off) const { return uint16_t(file[off] | file[off + 1] << 8); }

    // file offset for a virtual address, -1 if unmapped or in the zero filled tail
    int64_t Offset(uint32_t va) const {
        if (va >= base && va < base + header_size) return va - base;
        for each (auto& s in sections)
            if (va >= s.va && va < s.va + s.rsize) return int64_t(s.raw) + (va - s.va);
        return -1;
    }

    const Section* SectionAt(uint32_t va) const {
        for each (auto& s in sections)
            if (va >= s.va && va < s.va + s.vsize) return &s;
        return NULL;
    }

    bool Read32(uint32_t va, uint32_t& out) const {
        int64_t o = Offset(va);
        if (o < 0 || o + 4 > int64_t(file.size())) return false;
        out = U32(uint32_t(o));
        return true;
    }

    std::string CStr(uint32_t va) const {
        int64_t o = Offset(va);
        if (o < 0) return std::string();
        std::string s;
        while (size_t(o) < file.size() && file[size_t(o)]) s += char(file[size_t(o++)]);
        return s;
    }

    bool Parse(std::vector<uint8_t>& data, std::string& error) {
        file.swap(data);
        if (file.size() < 0x180 || memcmp(&file[0], "XBEH", 4)) {
            error = "not an XBE";
            return false;
        }
        base = U32(0x104);
        header_size = U32(0x108);
        image_size = U32(0x10C);
        cert = U32(0x118);
        uint32_t nsec = U32(0x11C), sec_addr = U32(0x120);
        init_flags = U32(0x124);
        uint32_t ep = U32(0x128), kt = U32(0x158);
        tls_addr = U32(0x12C);
        stack_commit = U32(0x130);
        heap_reserve = U32(0x134);
        heap_commit = U32(0x138);
        uint32_t lo = base, hi = base + image_size;
#define XBE_INSIDE(v) ((v) >= lo && (v) < hi)
        if (XBE_INSIDE(ep ^ kRetailEntryKey) && XBE_INSIDE(kt ^ kRetailThunkKey)) {
            entry = ep ^ kRetailEntryKey;
            kernel_thunk = kt ^ kRetailThunkKey;
        } else if (XBE_INSIDE(ep ^ kDebugEntryKey) && XBE_INSIDE(kt ^ kDebugThunkKey)) {
            entry = ep ^ kDebugEntryKey;
            kernel_thunk = kt ^ kDebugThunkKey;
            debug = true;
        } else {
            error = "entry point key not recognised";
            return false;
        }
        for (uint32_t i = 0; i < nsec; i++) {
            uint32_t o = sec_addr - base + i * 56;
            Section s;
            s.flags = U32(o);
            s.va = U32(o + 4);
            s.vsize = U32(o + 8);
            s.raw = U32(o + 12);
            s.rsize = U32(o + 16);
            uint32_t name = U32(o + 20);
            if (name - base < header_size)
                s.name = reinterpret_cast<const char*>(&file[name - base]);
            sections.push_back(s);
        }
        uint32_t nlib = U32(0x160), lib_addr = U32(0x164);
        for (uint32_t i = 0; i < nlib && lib_addr; i++) {
            uint32_t o = lib_addr - base + i * 16;
            Library l;
            char n[9] = {0};
            memcpy(n, &file[o], 8);
            l.name = n;
            l.major = U16(o + 8);
            l.minor = U16(o + 10);
            l.build = U16(o + 12);
            l.flags = U16(o + 14);
            libraries.push_back(l);
        }
        if (cert - base + 12 <= file.size()) title_id = U32(cert - base + 8);  // certificate +8 is the title id
        if (tls_addr) {
            int64_t o = Offset(tls_addr);
            if (o >= 0) {
                tls.data_start = U32(uint32_t(o));
                tls.data_end = U32(uint32_t(o) + 4);
                tls.index_addr = U32(uint32_t(o) + 8);
                tls.callbacks = U32(uint32_t(o) + 12);
                tls.zero_fill = U32(uint32_t(o) + 16);
                tls.characteristics = U32(uint32_t(o) + 20);
            }
        }
        return true;
    }
};

}  // namespace xbe
