// SPDX-License-Identifier: AGPL-3.0-or-later
// Structured, channelled logging for every layer DLL.
#pragma once
#include <cstdarg>
#include <cstdint>

namespace xsl {
enum class Level : int { Error = 0, Warn = 1, Info = 2, Trace = 3 };

// Channel is a short subsystem tag ("gfx", "rt", "input", ...). The level
// threshold comes from XSL_LOG (e.g. "info" or "gfx=trace,rt=warn").
bool log_enabled(const char* channel, Level level);
// XSL_LOG_ARMED=<spec> (diagnostics): the spec that replaces XSL_LOG from an event the layer picks
// (a call trace of the moments before a failure, without tracing the minutes before it).
void log_arm(bool on);
void logv(const char* channel, Level level, const char* fmt, va_list ap);
void log(const char* channel, Level level, const char* fmt, ...);
[[noreturn]] void fatal(const char* channel, const char* fmt, ...);
}  // namespace xsl

#define XSL_ERR(ch, ...)   ::xsl::log(ch, ::xsl::Level::Error, __VA_ARGS__)
#define XSL_WARN(ch, ...)  ::xsl::log(ch, ::xsl::Level::Warn, __VA_ARGS__)
#define XSL_INFO(ch, ...)  ::xsl::log(ch, ::xsl::Level::Info, __VA_ARGS__)
#define XSL_TRACE(ch, ...) do { if (::xsl::log_enabled(ch, ::xsl::Level::Trace)) ::xsl::log(ch, ::xsl::Level::Trace, __VA_ARGS__); } while (0)
