// Bloody Roar Extreme on the Xbox 360: entry point. The game's files sit beside default.xex (game:\) as they are on
// the disc, default.xbe among them; saves go under game:\hdd. Log: game:\jsrf360.log.
// Back + Start held for two seconds returns to the dashboard.
#include "app/app.h"

#include <algorithm>
#include <map>
#include <vector>

#include "core/input.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "host.h"
#include "kernel/kernel.h"

namespace game {
void CycleFrameSkip();
}
namespace kernel {
void LogRecentCalls(uint32_t id);
}

namespace app {

uint32_t Language() {
    // XC_LANGUAGE: 1 English, 2 Japanese, 3 German, 4 French, 5 Spanish, 6 Italian; the USA release has English only
    return 1;
}

bool Widescreen() {
    XVIDEO_MODE mode;
    memset(&mode, 0, sizeof(mode));
    XGetVideoMode(&mode);
    return mode.fIsWideScreen != 0;
}

}  // namespace app

namespace {

// bring-up: where each guest thread is, every 5 s
const bool kWatch = false;

// the title's code (dispatch: 4700 functions over 00011000..0013F770)
const uint32_t kCodeLo = 0x00011000, kCodeHi = 0x0018CB30;  // .text do JSRF

// a sampling profiler of the guest threads: the innermost return address on each one's stack, about a thousand times
// a second, the most frequent ones logged every 10 s (the time the title's own code takes, by caller)
struct Sampler {
    std::map<uint64_t, uint32_t> hits;  // thread id << 32 | return address
    std::map<uint32_t, uint32_t> totals;
    std::vector<kernel::ThreadRef> threads;
    int64_t next_list, next_log;
    Sampler() : next_list(0), next_log(0) {}

    void Sample(int64_t now) {
        if (now >= next_list) {
            threads = kernel::Threads();
            next_list = now + 100000000ll;
        }
        for (size_t i = 0; i < threads.size(); i++) {
            X86Context* c = threads[i]->ctx;
            if (!c) continue;
            uint32_t esp = c->esp, ra = 0;
            if (esp < 0x1000) continue;
            {
                static uint64_t told = 0;
                uint32_t id = threads[i]->id, top = threads[i]->stack_top, low = top - threads[i]->stack_size - 0x1000;
                if ((esp > top || esp < low) && !(told >> ((id >> 2) & 63) & 1)) {
                    told |= uint64_t(1) << ((id >> 2) & 63);
                    LOGE("pilha: thread %X com esp %08X ebp %08X fora de [%08X, %08X]", id, esp, c->ebp, low, top);
                }
            }
            for (uint32_t k = 0; k < 48; k++) {
                // perto do fim da area de pilhas, esp + k * 4 cai em pagina sem commit (queda no Xenia, 2026-11-04)
                if (!mem::IsCommitted(esp + k * 4)) break;
                uint32_t v = kernel::R32(esp + k * 4);
                if (v >= kCodeLo && v < kCodeHi) {
                    ra = v;
                    break;
                }
            }
            hits[uint64_t(threads[i]->id) << 32 | ra]++;
            totals[threads[i]->id]++;
        }
        if (now < next_log) return;
        if (next_log) {
            for (std::map<uint32_t, uint32_t>::iterator t = totals.begin(); t != totals.end(); ++t) {
                std::vector<std::pair<uint32_t, uint32_t> > top;
                for (std::map<uint64_t, uint32_t>::iterator h = hits.begin(); h != hits.end(); ++h)
                    if (uint32_t(h->first >> 32) == t->first) top.push_back(std::make_pair(h->second, uint32_t(h->first)));
                std::sort(top.rbegin(), top.rend());
                std::string line;
                for (size_t k = 0; k < top.size() && k < 60; k++) {
                    char b[32];
                    _snprintf(b, sizeof(b), " %08X:%u%%", top[k].second, top[k].first * 100 / t->second);
                    line += b;
                }
                LOGI("prof: thread %X, %u samples:%s", t->first, t->second, line.c_str());
            }
        }
        hits.clear();
        totals.clear();
        next_log = now + 10000000000ll;
    }
};

}  // namespace

void __cdecl main() {
    logx::Init("game:\\jsrf360.log");
    LOGR("JSRF for Xbox 360, built " __DATE__ " " __TIME__);
    MEMORYSTATUS ms;
    GlobalMemoryStatus(&ms);
    LOGI("memory: %u MB free of %u MB", unsigned(ms.dwAvailPhys >> 20), unsigned(ms.dwTotalPhys >> 20));

    if (!kernel::Boot("game:\\default.xbe", "game:", "game:\\hdd")) {
        LOGE("boot failed, see the lines above");
        logx::Flush();
        // stay up long enough for the log to be read, then leave
        Sleep(3000);
        XLaunchNewImage(XLAUNCH_KEYWORD_DEFAULT_APP, 0);
        return;
    }
    LOGI("boot: guest started");

    int64_t held_since = 0, start = host::NowNs(), next_dump = start + 5000000000ll, next_pad = 0;
    Sampler sampler;
    bool cap_held = false, fs_held = false, gm_held = false;
    for (;;) {
        Sleep(1);
        sampler.Sample(host::NowNs());
        if (host::NowNs() < next_pad) continue;
        next_pad = host::NowNs() + 50000000ll;
        // bring-up diagnostics: where each guest thread is, every 5 s for the first two minutes
        int64_t now = host::NowNs();
        if (kWatch && now >= next_dump && now - start < 120000000000ll) {
            next_dump += 5000000000ll;
            std::vector<kernel::ThreadRef> ts = kernel::Threads();
            LOGI("watch: %.0f s, %u guest threads", double(now - start) / 1e9, unsigned(ts.size()));
            for (size_t i = 0; i < ts.size(); i++)
                if (ts[i]->ctx) {
                    LOGI("watch: thread %X last indirect call %08X", ts[i]->id, ts[i]->ctx->last_icall);
                    kernel::LogGuestStack(*ts[i]->ctx, 24);
                }
        }
        input::Pad p = input::Poll(0);
        // JSRF: botoes de depuracao do BRE, so com game:\debug_buttons.txt. L3 e Back sao do jogo, e a captura congelava
        // a imagem por 6 a 38 s no console (tests/evidence/console_boot7).
        static const bool debug_buttons = GetFileAttributesA("game:\\debug_buttons.txt") != DWORD(-1);
        if (debug_buttons) {
            bool cap = (p.buttons & 0x40) != 0;
            if (cap && !cap_held) gpu::RequestCapture();
            cap_held = cap;
            bool gm = (p.buttons & 0x30) == 0x20;
            if (gm && !gm_held) gpu::CycleGamma();
            gm_held = gm;
        }
        bool exit_combo = (p.buttons & 0x30) == 0x30;  // start and back
        if (exit_combo) {
            if (!held_since) held_since = host::NowNs();
            if (host::NowNs() - held_since > 2000000000ll) {
                LOGI("jsrf: saida pedida com Start + Back");
                kernel::Shutdown(0);
            }
        } else {
            held_since = 0;
        }
    }
}
