// DisplayMirror - update check against the GitHub releases API.
//
// Asks api.github.com for the latest release of the repository below, compares
// its tag against the built-in version, and can fetch and launch the installer
// from that release. No update is ever installed without being asked for.
#pragma once

#include <windows.h>

#include <string>

namespace dm {

// The repository the releases are published to.
constexpr wchar_t kUpdateOwner[] = L"RoveHD";
constexpr wchar_t kUpdateRepo[] = L"mirrorscreen";

struct UpdateInfo {
  bool available = false;      // A release newer than this build exists.
  bool checked = false;        // The query itself succeeded.
  std::wstring version;        // "1.2.0", the tag with any leading v removed.
  std::wstring downloadUrl;    // The installer asset on that release.
  std::wstring releaseUrl;     // The release page, for "what changed".
  std::wstring error;          // Empty unless the check failed.
};

// Blocking network call: run it on a worker thread, never on the message loop.
UpdateInfo CheckForUpdate();

// Downloads the installer to the temp directory and starts it. The installer
// closes the running copy itself, so there is nothing to do here afterwards.
bool DownloadAndRunInstaller(const UpdateInfo& info, std::wstring* error);

}  // namespace dm
