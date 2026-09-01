#include "updater.h"

#include <shellapi.h>
#include <winhttp.h>

#include <cctype>
#include <vector>

#include "log.h"
#include "version.h"

namespace dm {
namespace {

constexpr wchar_t kApiHost[] = L"api.github.com";
constexpr DWORD kMaxResponseBytes = 1u << 20;      // The JSON is a few KB.
constexpr DWORD kMaxInstallerBytes = 64u << 20;    // The installer is ~330 KB.

std::wstring Widen(const std::string& utf8) {
  if (utf8.empty()) return std::wstring();
  const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                         static_cast<int>(utf8.size()), nullptr, 0);
  if (needed <= 0) return std::wstring();
  std::wstring wide(static_cast<size_t>(needed), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                      wide.data(), needed);
  return wide;
}

// Enough JSON for this one shape: find "key":"value" and return the value with
// the handful of escapes GitHub actually emits in these fields undone.
std::string FindStringField(const std::string& json, const std::string& key,
                            size_t from, size_t* valueEnd) {
  const std::string needle = "\"" + key + "\"";
  const size_t at = json.find(needle, from);
  if (at == std::string::npos) return std::string();

  size_t i = json.find(':', at + needle.size());
  if (i == std::string::npos) return std::string();
  ++i;
  while (i < json.size() && (json[i] == ' ' || json[i] == '\t')) ++i;
  if (i >= json.size() || json[i] != '"') return std::string();
  ++i;

  std::string value;
  while (i < json.size() && json[i] != '"') {
    if (json[i] == '\\' && i + 1 < json.size()) {
      ++i;
      switch (json[i]) {
        case 'n': value += '\n'; break;
        case 't': value += '\t'; break;
        case 'r': break;
        default: value += json[i]; break;
      }
    } else {
      value += json[i];
    }
    ++i;
  }
  if (valueEnd) *valueEnd = i;
  return value;
}

// "v1.2.3" and "1.2.3" both parse; anything unparsable sorts as 0.
void ParseVersion(const std::wstring& text, int parts[3]) {
  parts[0] = parts[1] = parts[2] = 0;
  size_t i = 0;
  while (i < text.size() && (text[i] == L'v' || text[i] == L'V')) ++i;

  for (int p = 0; p < 3 && i < text.size(); ++p) {
    int value = 0;
    bool any = false;
    while (i < text.size() && text[i] >= L'0' && text[i] <= L'9') {
      value = value * 10 + (text[i] - L'0');
      ++i;
      any = true;
    }
    if (!any) break;
    parts[p] = value;
    if (i < text.size() && text[i] == L'.') ++i;
  }
}

bool IsNewerThanThisBuild(const std::wstring& tag) {
  int theirs[3] = {};
  ParseVersion(tag, theirs);
  const int ours[3] = {DM_VERSION_MAJOR, DM_VERSION_MINOR, DM_VERSION_PATCH};
  for (int i = 0; i < 3; ++i) {
    if (theirs[i] != ours[i]) return theirs[i] > ours[i];
  }
  return false;
}

// The download URL comes out of a network response, and it is about to be
// fetched and executed. It has to be https and it has to be GitHub's.
bool IsTrustedGithubUrl(const std::wstring& url) {
  URL_COMPONENTS parts = {};
  parts.dwStructSize = sizeof(parts);
  parts.dwHostNameLength = static_cast<DWORD>(-1);
  parts.dwSchemeLength = static_cast<DWORD>(-1);
  if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) return false;
  if (parts.nScheme != INTERNET_SCHEME_HTTPS) return false;

  const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
  auto endsWith = [&host](const wchar_t* suffix) {
    const size_t n = wcslen(suffix);
    return host.size() >= n && host.compare(host.size() - n, n, suffix) == 0;
  };
  return host == L"github.com" || endsWith(L".github.com") ||
         endsWith(L".githubusercontent.com");
}

struct WinHttpHandle {
  HINTERNET handle = nullptr;
  ~WinHttpHandle() {
    if (handle) WinHttpCloseHandle(handle);
  }
  WinHttpHandle() = default;
  explicit WinHttpHandle(HINTERNET h) : handle(h) {}
  WinHttpHandle(const WinHttpHandle&) = delete;
  WinHttpHandle& operator=(const WinHttpHandle&) = delete;
  explicit operator bool() const { return handle != nullptr; }
};

HINTERNET OpenSession() {
  // WinHTTP validates the certificate chain by default, which is the whole
  // reason this uses WinHTTP rather than a raw socket.
  return WinHttpOpen(L"DisplayMirror/" DM_VERSION_STRING,
                     WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
}

// Issues one GET and returns the body, following redirects.
bool HttpGet(HINTERNET session, const std::wstring& url, DWORD maxBytes,
             std::string* body, std::wstring* error) {
  URL_COMPONENTS parts = {};
  parts.dwStructSize = sizeof(parts);
  parts.dwHostNameLength = static_cast<DWORD>(-1);
  parts.dwUrlPathLength = static_cast<DWORD>(-1);
  parts.dwExtraInfoLength = static_cast<DWORD>(-1);
  parts.dwSchemeLength = static_cast<DWORD>(-1);
  if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) {
    *error = L"The update URL could not be parsed.";
    return false;
  }

  const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
  std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
  if (parts.dwExtraInfoLength > 0) {
    path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
  }

  WinHttpHandle connection(WinHttpConnect(session, host.c_str(), parts.nPort, 0));
  if (!connection) {
    *error = L"Could not connect to " + host + L".";
    return false;
  }

  WinHttpHandle request(WinHttpOpenRequest(
      connection.handle, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
      WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
  if (!request) {
    *error = L"Could not create the request.";
    return false;
  }

  const wchar_t kHeaders[] = L"Accept: application/vnd.github+json\r\n";
  if (!WinHttpSendRequest(request.handle, kHeaders, static_cast<DWORD>(-1),
                          WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !WinHttpReceiveResponse(request.handle, nullptr)) {
    *error = L"The update server could not be reached.";
    return false;
  }

  DWORD status = 0;
  DWORD statusSize = sizeof(status);
  WinHttpQueryHeaders(request.handle,
                      WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                      WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                      WINHTTP_NO_HEADER_INDEX);
  if (status == 404) {
    *error = L"No release has been published yet.";
    return false;
  }
  if (status < 200 || status >= 300) {
    *error = L"The update server answered with HTTP " + std::to_wstring(status) + L".";
    return false;
  }

  body->clear();
  std::vector<char> chunk(16384);
  for (;;) {
    DWORD read = 0;
    if (!WinHttpReadData(request.handle, chunk.data(),
                         static_cast<DWORD>(chunk.size()), &read)) {
      *error = L"The download was interrupted.";
      return false;
    }
    if (read == 0) break;
    if (body->size() + read > maxBytes) {
      *error = L"The download was larger than expected and was abandoned.";
      return false;
    }
    body->append(chunk.data(), read);
  }
  return true;
}

}  // namespace

UpdateInfo CheckForUpdate() {
  UpdateInfo info;

  WinHttpHandle session(OpenSession());
  if (!session) {
    info.error = L"WinHTTP could not be initialised.";
    return info;
  }
  // A check must never hang the worker thread for minutes on a dead network.
  WinHttpSetTimeouts(session.handle, 8000, 8000, 15000, 15000);

  const std::wstring url = std::wstring(L"https://") + kApiHost + L"/repos/" +
                           kUpdateOwner + L"/" + kUpdateRepo + L"/releases/latest";

  std::string body;
  if (!HttpGet(session.handle, url, kMaxResponseBytes, &body, &info.error)) {
    return info;
  }
  info.checked = true;

  const std::wstring tag = Widen(FindStringField(body, "tag_name", 0, nullptr));
  if (tag.empty()) {
    info.error = L"The release information could not be read.";
    info.checked = false;
    return info;
  }
  info.version = tag[0] == L'v' || tag[0] == L'V' ? tag.substr(1) : tag;
  info.releaseUrl = Widen(FindStringField(body, "html_url", 0, nullptr));

  if (!IsNewerThanThisBuild(tag)) {
    DM_INFO(L"Update check: %s is the latest release; this build is %s.", tag.c_str(),
            DM_VERSION_STRING);
    return info;
  }

  // A release can carry both the installer and the portable executable, and
  // launching the portable one would just start the old program again. Prefer
  // an asset that says it is a setup; fall back to any other .exe only if none
  // does.
  std::wstring fallbackUrl;
  size_t cursor = 0;
  for (;;) {
    size_t nameEnd = 0;
    std::string name = FindStringField(body, "name", cursor, &nameEnd);
    if (name.empty()) break;
    cursor = nameEnd;

    const bool isExe = name.size() > 4 &&
                       _stricmp(name.c_str() + name.size() - 4, ".exe") == 0;
    if (!isExe) continue;

    const std::wstring candidate =
        Widen(FindStringField(body, "browser_download_url", cursor, nullptr));
    if (candidate.empty()) continue;
    if (!IsTrustedGithubUrl(candidate)) {
      DM_WARN(L"Ignoring an update asset that is not hosted on GitHub.");
      continue;
    }

    for (char& c : name) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (name.find("setup") != std::string::npos ||
        name.find("install") != std::string::npos) {
      info.downloadUrl = candidate;
      break;
    }
    if (fallbackUrl.empty()) fallbackUrl = candidate;
  }
  if (info.downloadUrl.empty()) info.downloadUrl = fallbackUrl;

  if (info.downloadUrl.empty()) {
    DM_WARN(L"Release %s is newer but carries no installer to download.", tag.c_str());
    return info;
  }

  info.available = true;
  DM_INFO(L"Update available: %s (this build is %s).", info.version.c_str(),
          DM_VERSION_STRING);
  return info;
}

bool DownloadAndRunInstaller(const UpdateInfo& info, std::wstring* error) {
  if (!IsTrustedGithubUrl(info.downloadUrl)) {
    *error = L"The download location is not a GitHub address; nothing was fetched.";
    return false;
  }

  WinHttpHandle session(OpenSession());
  if (!session) {
    *error = L"WinHTTP could not be initialised.";
    return false;
  }
  WinHttpSetTimeouts(session.handle, 8000, 8000, 30000, 60000);

  std::string payload;
  if (!HttpGet(session.handle, info.downloadUrl, kMaxInstallerBytes, &payload, error)) {
    return false;
  }
  // Every Windows executable starts with "MZ". A redirect to an error page
  // would not, and must not be launched.
  if (payload.size() < 2 || payload[0] != 'M' || payload[1] != 'Z') {
    *error = L"What was downloaded is not a Windows program; it was discarded.";
    return false;
  }

  wchar_t tempDir[MAX_PATH] = {};
  if (GetTempPathW(MAX_PATH, tempDir) == 0) {
    *error = L"The temporary directory could not be found.";
    return false;
  }
  const std::wstring path = std::wstring(tempDir) + L"DisplayMirror-" + info.version +
                            L"-Setup.exe";

  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    *error = L"The installer could not be written to the temporary directory.";
    return false;
  }
  DWORD written = 0;
  const BOOL ok = WriteFile(file, payload.data(), static_cast<DWORD>(payload.size()),
                            &written, nullptr);
  CloseHandle(file);
  if (!ok || written != payload.size()) {
    *error = L"The installer could not be written completely.";
    return false;
  }

  DM_INFO(L"Downloaded %s (%zu bytes); starting it.", path.c_str(), payload.size());

  // The installer closes this copy itself before replacing the executable.
  const HINSTANCE result =
      ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  if (reinterpret_cast<INT_PTR>(result) <= 32) {
    *error = L"The installer was downloaded but could not be started.";
    return false;
  }
  return true;
}

}  // namespace dm
