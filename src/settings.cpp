#include "settings.h"

#include "log.h"

namespace dm {
namespace {

constexpr wchar_t kSettingsKey[] = L"Software\\DisplayMirror";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"DisplayMirror";

// Started from the Run key the window would otherwise pop up in front of
// whatever the user is doing at logon, so the autostart command asks for it
// minimised. main.cpp reads the flag back off the command line.
constexpr wchar_t kMinimizedFlag[] = L"--minimized";

std::wstring ReadString(HKEY key, const wchar_t* name) {
  DWORD type = 0;
  DWORD bytes = 0;
  if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
      type != REG_SZ || bytes < sizeof(wchar_t)) {
    return std::wstring();
  }

  std::wstring value(bytes / sizeof(wchar_t), L'\0');
  if (RegQueryValueExW(key, name, nullptr, nullptr,
                       reinterpret_cast<LPBYTE>(value.data()), &bytes) != ERROR_SUCCESS) {
    return std::wstring();
  }
  // RegQueryValueEx counts the terminator when it is stored, and does not when
  // it is not; trim whatever is there rather than trusting either case.
  const size_t end = value.find(L'\0');
  value.resize(end == std::wstring::npos ? bytes / sizeof(wchar_t) : end);
  return value;
}

bool ReadBool(HKEY key, const wchar_t* name, bool fallback) {
  DWORD type = 0;
  DWORD value = 0;
  DWORD bytes = sizeof(value);
  if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<LPBYTE>(&value),
                       &bytes) != ERROR_SUCCESS ||
      type != REG_DWORD) {
    return fallback;
  }
  return value != 0;
}

void WriteString(HKEY key, const wchar_t* name, const std::wstring& value) {
  RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                 static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
}

void WriteBool(HKEY key, const wchar_t* name, bool value) {
  const DWORD stored = value ? 1u : 0u;
  RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&stored),
                 sizeof(stored));
}

std::wstring QuotedExePath() {
  wchar_t path[MAX_PATH] = {};
  if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0) return std::wstring();
  return std::wstring(L"\"") + path + L"\" " + kMinimizedFlag;
}

}  // namespace

Settings LoadSettings() {
  Settings settings;

  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, KEY_READ, &key) != ERROR_SUCCESS) {
    return settings;  // Never saved: the caller keeps its own defaults.
  }

  settings.sourceId = ReadString(key, L"SourceMonitor");
  settings.targetId = ReadString(key, L"TargetMonitor");
  settings.sourceName = ReadString(key, L"SourceName");
  settings.targetName = ReadString(key, L"TargetName");
  settings.drawCursor = ReadBool(key, L"DrawCursor", settings.drawCursor);
  settings.allowTearing = ReadBool(key, L"AllowTearing", settings.allowTearing);
  settings.autoMirror = ReadBool(key, L"AutoMirror", settings.autoMirror);
  settings.valid = true;
  RegCloseKey(key);

  DM_INFO(L"Settings loaded: source \"%s\", target \"%s\", cursor %s, tearing %s, "
          L"auto-mirror %s",
          settings.sourceName.empty() ? L"<none>" : settings.sourceName.c_str(),
          settings.targetName.empty() ? L"<none>" : settings.targetName.c_str(),
          settings.drawCursor ? L"on" : L"off", settings.allowTearing ? L"on" : L"off",
          settings.autoMirror ? L"on" : L"off");
  return settings;
}

void SaveSettings(const Settings& settings) {
  HKEY key = nullptr;
  const LSTATUS status = RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr,
                                         REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr,
                                         &key, nullptr);
  if (status != ERROR_SUCCESS) {
    DM_WARN(L"Could not open the settings key for writing (error %ld).", status);
    return;
  }

  WriteString(key, L"SourceMonitor", settings.sourceId);
  WriteString(key, L"TargetMonitor", settings.targetId);
  WriteString(key, L"SourceName", settings.sourceName);
  WriteString(key, L"TargetName", settings.targetName);
  WriteBool(key, L"DrawCursor", settings.drawCursor);
  WriteBool(key, L"AllowTearing", settings.allowTearing);
  WriteBool(key, L"AutoMirror", settings.autoMirror);
  RegCloseKey(key);
}

bool IsStartWithWindowsEnabled() {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &key) != ERROR_SUCCESS) {
    return false;
  }
  const bool present = !ReadString(key, kRunValue).empty();
  RegCloseKey(key);
  return present;
}

bool SetStartWithWindows(bool enabled) {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_WRITE, &key) != ERROR_SUCCESS) {
    DM_WARN(L"Could not open the Run key; \"start with Windows\" was not changed.");
    return false;
  }

  bool ok = false;
  if (enabled) {
    // Always rewrite the full path: the entry is stale the moment the exe is
    // moved, and rewriting it on every enable is the cheapest way to fix that.
    const std::wstring command = QuotedExePath();
    if (command.empty()) {
      DM_WARN(L"Could not determine the executable path for autostart.");
    } else {
      ok = RegSetValueExW(key, kRunValue, 0, REG_SZ,
                          reinterpret_cast<const BYTE*>(command.c_str()),
                          static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t))) ==
           ERROR_SUCCESS;
      if (ok) DM_INFO(L"Start with Windows enabled: %s", command.c_str());
    }
  } else {
    const LSTATUS status = RegDeleteValueW(key, kRunValue);
    ok = status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
    if (ok) DM_INFO(L"Start with Windows disabled.");
  }

  RegCloseKey(key);
  if (!ok) DM_WARN(L"Writing the Run key failed; autostart is unchanged.");
  return ok;
}

}  // namespace dm
