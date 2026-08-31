#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace dm {
namespace {

std::mutex g_mutex;
HANDLE g_file = INVALID_HANDLE_VALUE;
LogSink g_sink = nullptr;

const wchar_t* LevelTag(LogLevel level) {
  switch (level) {
    case LogLevel::Warn: return L"WARN ";
    case LogLevel::Error: return L"ERROR";
    default: return L"INFO ";
  }
}

std::wstring LogFilePath() {
  wchar_t exe[MAX_PATH] = {};
  if (GetModuleFileNameW(nullptr, exe, MAX_PATH) == 0) return L"DisplayMirror.log";
  std::wstring path(exe);
  const size_t slash = path.find_last_of(L'\\');
  if (slash != std::wstring::npos) path.resize(slash + 1);
  return path + L"DisplayMirror.log";
}

}  // namespace

void LogInit(LogSink sink) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_sink = sink;
  if (g_file == INVALID_HANDLE_VALUE) {
    g_file = CreateFileW(LogFilePath().c_str(), FILE_APPEND_DATA,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  }
}

void LogShutdown() {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_sink = nullptr;
  if (g_file != INVALID_HANDLE_VALUE) {
    CloseHandle(g_file);
    g_file = INVALID_HANDLE_VALUE;
  }
}

void LogWrite(LogLevel level, const wchar_t* fmt, ...) {
  wchar_t body[1024];
  va_list args;
  va_start(args, fmt);
  const int written = _vsnwprintf_s(body, _countof(body), _TRUNCATE, fmt, args);
  va_end(args);
  if (written < 0) wcscpy_s(body, L"<log message truncated>");

  SYSTEMTIME st;
  GetLocalTime(&st);

  wchar_t line[1200];
  _snwprintf_s(line, _countof(line), _TRUNCATE, L"[%02u:%02u:%02u.%03u] %s %s",
               st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
               LevelTag(level), body);

  std::lock_guard<std::mutex> lock(g_mutex);

  OutputDebugStringW(line);
  OutputDebugStringW(L"\n");

  if (g_file != INVALID_HANDLE_VALUE) {
    // UTF-8 on disk so the log opens cleanly in any editor.
    const std::wstring wide = std::wstring(line) + L"\r\n";
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (bytes > 1) {
      std::string utf8(static_cast<size_t>(bytes - 1), '\0');
      WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, utf8.data(), bytes, nullptr, nullptr);
      DWORD ignored = 0;
      WriteFile(g_file, utf8.data(), static_cast<DWORD>(utf8.size()), &ignored, nullptr);
    }
  }

  if (g_sink) g_sink(level, line);
}

std::wstring HrToString(long hr) {
  wchar_t buf[64];
  _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"0x%08X", static_cast<unsigned>(hr));

  wchar_t* text = nullptr;
  const DWORD len = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, static_cast<DWORD>(hr), 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);

  std::wstring result(buf);
  if (len && text) {
    std::wstring message(text, len);
    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' ||
                               message.back() == L' ')) {
      message.pop_back();
    }
    if (!message.empty()) result += L" (" + message + L")";
  }
  if (text) LocalFree(text);
  return result;
}

}  // namespace dm
