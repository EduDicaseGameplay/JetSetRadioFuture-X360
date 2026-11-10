// Xbox 360 build of pc-recomp/sdk/runtime/kernel/boot.cpp: loads default.xbe into the guest window, resolves its
// kernel imports and starts its entry point on a guest thread.
#include <algorithm>
#include <stdio.h>

#include "host.h"
#include "kernel/kernel.h"
#include "xbe/xbe.h"

namespace dispatch {
void Init();
uint32_t KernelThunkAddress(uint32_t ordinal);
}  // namespace dispatch

namespace kernel {

uint8_t* g_base = NULL;
const xbe::Image* g_image = NULL;
static xbe::Image g_xbe;
static uint32_t g_saved_data = 0;

uint32_t SavedDataAddress() { return g_saved_data; }
void SetSavedDataAddress(uint32_t surface) { g_saved_data = surface; }

static bool ReadFile(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(size_t(n));
    bool ok = n > 0 && fread(&out[0], 1, size_t(n), f) == size_t(n);
    fclose(f);
    return ok;
}

bool Boot(const std::string& xbe_path, const std::string& game_dir, const std::string& data_dir) {
    if (!mem::Init()) return false;
    g_base = mem::Base();

    std::vector<uint8_t> file;
    if (!ReadFile(xbe_path, file)) {
        LOGE("boot: cannot read %s", xbe_path.c_str());
        return false;
    }
    std::string err;
    if (!g_xbe.Parse(file, err)) {
        LOGE("boot: %s: %s", xbe_path.c_str(), err.c_str());
        return false;
    }
    g_image = &g_xbe;
    const xbe::Image& img = g_xbe;
    if (img.entry != X86EntryPoint) {
        LOGE("boot: %s entry %08X does not match the recompiled entry %08X, wrong executable", xbe_path.c_str(), img.entry,
             X86EntryPoint);
        return false;
    }
    LOGI("boot: title %08X, entry %08X, %u sections", img.title_id, img.entry, unsigned(img.sections.size()));
    for (size_t i = 0; i < img.libraries.size(); i++)
        LOGI("boot: library %-8s %u.%u.%u", img.libraries[i].name.c_str(), img.libraries[i].major, img.libraries[i].minor,
             img.libraries[i].build);

    mem::ReserveImage(img.base, img.image_size);
    memcpy(HostPtr(img.base), &img.file[0], std::min<size_t>(img.header_size, img.file.size()));
    // XBE header 0x120 points at the section headers, 56 bytes each, +0x18 is the reference count that marks one loaded
    uint32_t sec_hdr = img.U32(0x120);
    for (size_t i = 0; i < img.sections.size(); i++) {
        const xbe::Section& s = img.sections[i];
        if (!(s.flags & xbe::kPreload)) continue;
        memcpy(HostPtr(s.va), &img.file[s.raw], std::min(s.rsize, s.vsize));
        W32(sec_hdr + uint32_t(i) * 56 + 0x18, 1);
    }

    InitKernelData();
    dispatch::Init();

    uint32_t missing = 0;
    for (uint32_t slot = img.kernel_thunk;; slot += 4) {
        uint32_t v = R32(slot);
        if (!v) break;
        uint32_t ord = v & 0x7FFFFFFF;  // high bit flags an import by ordinal
        uint32_t data = DataAddress(ord);
        W32(slot, data ? data : dispatch::KernelThunkAddress(ord));
        if (!HasExport(ord)) {
            LOGW("boot: kernel import %u (%s) has no implementation", ord, ExportName(ord));
            missing++;
        }
    }
    LOGI("boot: kernel imports resolved, %u missing", missing);

    MountDefaults(game_dir, data_dir);

    uint32_t tls = 0;
    if (img.tls_addr) tls = ((img.tls.data_end - img.tls.data_start + img.tls.zero_fill + 15) & ~15u) + 4;
    ThreadRef t = CreateThread(std::max<uint32_t>(img.stack_commit, 0x10000), tls, 0, img.entry, 0, false);
    StartThread(t);
    return true;
}

void Shutdown(int code) {
    LOGR("Exit (%d)", code);
    logx::Flush();
    XLaunchNewImage(XLAUNCH_KEYWORD_DEFAULT_APP, 0);
    for (;;) Sleep(1000);
}

}  // namespace kernel
