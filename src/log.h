// DisplayMirror - lightweight logging.
//
// Rules of the road: log state transitions, never per-frame events.
#pragma once

#include <string>

namespace dm {

enum class LogLevel { Info, Warn, Error };

// Sink invoked for every line, in addition to the log file and the debugger.
// Used by the config window to show a live log pane. Single-threaded app, so
// no synchronisation beyond the internal mutex in log.cpp.
using LogSink = void (*)(LogLevel, const std::wstring& line);

void LogInit(LogSink sink);
void LogShutdown();

void LogWrite(LogLevel level, const wchar_t* fmt, ...);

// Formats an HRESULT as "0x80070005 (Access is denied)" for diagnostics.
std::wstring HrToString(long hr);

}  // namespace dm

#define DM_INFO(...) ::dm::LogWrite(::dm::LogLevel::Info, __VA_ARGS__)
#define DM_WARN(...) ::dm::LogWrite(::dm::LogLevel::Warn, __VA_ARGS__)
#define DM_ERROR(...) ::dm::LogWrite(::dm::LogLevel::Error, __VA_ARGS__)
