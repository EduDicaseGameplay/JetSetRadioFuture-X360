#include "core/log.h"

#include <mutex>
#include <stdio.h>

#include "host.h"

namespace logx {

// A line written and flushed where it was logged took 6 to 23 ms on the console (v26 logs), on the title's and the
// renderer's threads too. Lines wait here and a thread of their own takes them to the disk; an error goes at once.
static std::mutex g_mutex;   // g_pending
static std::mutex g_disk;    // g_file
static std::string g_pending;
static FILE* g_file = NULL;
static std::string g_path;
static int64_t g_start = host::NowNs();

static void Drain() {
    std::lock_guard<std::mutex> disk(g_disk);
    std::string lines;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        lines.swap(g_pending);
    }
    if (g_file && !lines.empty()) {
        fwrite(lines.data(), 1, lines.size(), g_file);
        fflush(g_file);
    }
}

void Init(const std::string& path) {
    g_path = path;
    g_file = fopen(path.c_str(), "w");
    host::Spawn([] {
        for (;;) {
            host::SleepNs(250000000);
            Drain();
        }
    }, "log", 1);
}

const std::string& Path() { return g_path; }

void Write(Level lv, const char* fmt, ...) {
    if (!kVerbose && lv < Error) return;
    char msg[4096];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(msg, sizeof(msg) - 1, fmt, ap);
    msg[sizeof(msg) - 1] = 0;
    va_end(ap);
    double t = double(host::NowNs() - g_start) / 1e9;
    static const char* names[] = {"debug", "info", "warn", "error", "info"};
    char line[4300];
    int n = _snprintf(line, sizeof(line) - 1, "[%9.3f] [%s] [t%04x] %s\n", t, names[int(lv)],
                      unsigned(GetCurrentThreadId() & 0xFFFF), msg);
    if (n < 0 || n > int(sizeof(line)) - 1) n = int(sizeof(line)) - 1;
    line[n] = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_pending.append(line, size_t(n));
    }
    if (lv == Error) Drain();
}

void Flush() { Drain(); }

}  // namespace logx
