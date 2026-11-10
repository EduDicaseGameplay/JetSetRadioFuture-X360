// Xbox 360 build of pc-recomp/sdk/runtime/core/log.h: game:\bre360.log, plus the debugger output when a
// devkit is attached. Info lines are kept while the port is brought up (kVerbose).
#pragma once
#include <atomic>
#include <stdarg.h>
#include <stdint.h>
#include <string>

namespace logx {

enum Level { Debug, Info, Warn, Error, Report };

void Init(const std::string& path);
void Write(Level lv, const char* fmt, ...);
void Flush();
const std::string& Path();

static const bool kVerbose = true;

}  // namespace logx

#define LOGD(...) ::logx::Write(::logx::Debug, __VA_ARGS__)
#define LOGI(...) ::logx::Write(::logx::Info, __VA_ARGS__)
#define LOGW(...) ::logx::Write(::logx::Warn, __VA_ARGS__)
#define LOGE(...) ::logx::Write(::logx::Error, __VA_ARGS__)
#define LOGR(...) ::logx::Write(::logx::Report, __VA_ARGS__)

// logs only the first n times a call site is reached
#define LOG_FIRST(n, ...)                                \
    do {                                                 \
        static std::atomic<int> _c(0);                   \
        if (_c.fetch_add(1) < (n)) LOGI(__VA_ARGS__);    \
    } while (0)
