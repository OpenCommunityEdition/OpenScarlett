// SPDX-License-Identifier: AGPL-3.0-or-later
#include "xsl/log.h"
#include "xsl/stub.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace xsl {
namespace {
struct Config {
    Level def = Level::Info;
    std::string spec, armedSpec;   // armedSpec: XSL_LOG_ARMED, in force between log_arm(true) and log_arm(false)
    volatile bool armed = false;
    FILE* file = nullptr;
    std::mutex mu;
    Config() {
        char buf[512];
        if (GetEnvironmentVariableA("XSL_LOG", buf, sizeof buf)) spec = buf;
        auto parse = [](const char* s) {
            if (!strcmp(s, "error")) return Level::Error;
            if (!strcmp(s, "warn")) return Level::Warn;
            if (!strcmp(s, "trace")) return Level::Trace;
            return Level::Info;
        };
        if (!spec.empty() && spec.find('=') == std::string::npos) def = parse(spec.c_str());
        if (GetEnvironmentVariableA("XSL_LOG_ARMED", buf, sizeof buf)) armedSpec = buf;
        if (GetEnvironmentVariableA("XSL_LOG_FILE", buf, sizeof buf)) file = fopen(buf, "a");
    }
    Level level_for(const char* ch) {
        // "gfx=trace,rt=warn" — first match wins.
        std::string key = std::string(ch) + "=";
        const std::string& spec = armed ? armedSpec : this->spec;
        auto p = spec.find(key);
        if (p == std::string::npos) return def;
        std::string v = spec.substr(p + key.size(), spec.find(',', p) - p - key.size());
        if (v == "error") return Level::Error;
        if (v == "warn") return Level::Warn;
        if (v == "trace") return Level::Trace;
        return Level::Info;
    }
};
Config& cfg() { static Config c; return c; }
const char* kLevel[] = {"err", "warn", "info", "trace"};
}  // namespace

bool log_enabled(const char* channel, Level level) { return level <= cfg().level_for(channel); }
void log_arm(bool on) { if (!cfg().armedSpec.empty()) cfg().armed = on; }

void logv(const char* channel, Level level, const char* fmt, va_list ap) {
    if (!log_enabled(channel, level)) return;
    char msg[2048];
    // XSL_LOG_TIME=1: seconds since the first line, in front of each line
    static const bool timed = [] { char b[4]; return GetEnvironmentVariableA("XSL_LOG_TIME", b, 4) && b[0] == '1'; }();
    static const ULONGLONG t0 = GetTickCount64();
    int n = 0;
    if (timed) n = snprintf(msg, sizeof msg, "%7.2f ", (GetTickCount64() - t0) / 1000.0);
    n += snprintf(msg + n, sizeof msg - n, "[xsl:%s:%s:%04lx] ", channel, kLevel[(int)level], GetCurrentThreadId());
    vsnprintf(msg + n, sizeof msg - n - 2, fmt, ap);
    strcat(msg, "\n");
    auto& c = cfg();
    std::lock_guard<std::mutex> g(c.mu);
    fputs(msg, stderr);
    fflush(stderr);
    if (c.file) { fputs(msg, c.file); fflush(c.file); }
    OutputDebugStringA(msg);
}

void log(const char* channel, Level level, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); logv(channel, level, fmt, ap); va_end(ap);
}

void fatal(const char* channel, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); logv(channel, Level::Error, fmt, ap); va_end(ap);
    TerminateProcess(GetCurrentProcess(), 3);
    for (;;) {}
}
}  // namespace xsl

long xsl_stub_hresult() {
    static long hr = [] {
        char b[8];
        return (GetEnvironmentVariableA("XSL_STUB_HRESULT", b, sizeof b) && !strcmp(b, "ok")) ? 0L : (long)0x80004001L;
    }();
    return hr;
}
XSL_STUB_RET xsl_stub_hit(int id, const char* name, void* self) {
    static volatile long counts[4096];
    long n = id < 4096 ? InterlockedIncrement(&counts[id]) : 1;
    if (n <= 3) xsl::log("stub", xsl::Level::Warn, "unimplemented %s (this=%p, call %ld)", name, self, n);
    return 0;
}
