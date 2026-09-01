// DisplayMirror - persisted configuration.
//
// Everything the config window can change is written back as soon as it
// changes, so the program comes up in the state it was left in and can start
// mirroring on its own once the saved pair of displays is connected.
//
// Storage is HKCU\Software\DisplayMirror. The registry rather than a file
// next to the executable: the exe may live somewhere unwritable, and "start
// with Windows" is a registry key anyway, so this keeps it to one mechanism.
#pragma once

#include <windows.h>

#include <string>

namespace dm {

struct Settings {
  // DisplayInfo::persistentId of the saved pair. Empty until something is
  // saved. Stored as the monitor device path so it survives a reboot and
  // tells two monitors of the same model apart.
  std::wstring sourceId;
  std::wstring targetId;
  // Friendly names, kept only so the log can name a display that is currently
  // disconnected. Never used to match.
  std::wstring sourceName;
  std::wstring targetName;

  bool drawCursor = true;
  // On by default: it is the lower-latency path, and on a cross-GPU pair it is
  // the one that behaves best. Downgraded automatically when DXGI reports no
  // tearing support.
  bool allowTearing = true;
  // Off until the user asks for it. Auto-starting on a pair the user never
  // chose would be a surprise.
  bool autoMirror = false;
  // Look for a new release on GitHub. The check is a single HTTPS request on a
  // worker thread, at most once a day; nothing is ever installed unasked.
  bool checkForUpdates = true;
  // When the last check ran, as a Unix timestamp, so restarting the program
  // ten times in a row does not mean ten requests.
  ULONGLONG lastUpdateCheck = 0;

  // False when nothing has ever been saved, so the caller knows to fall back
  // to its own defaults for the display selection.
  bool valid = false;
};

Settings LoadSettings();
void SaveSettings(const Settings& settings);

// "Start with Windows" is the Run key itself rather than a copy of its state,
// so the checkbox cannot drift out of sync with what Windows will actually do.
bool IsStartWithWindowsEnabled();
bool SetStartWithWindows(bool enabled);

// No installer is involved: the Run entry is simply the absolute path of
// wherever this executable happens to live. Moving it therefore breaks the
// entry, and Windows fails silently on a path that no longer exists. Called at
// startup, this rewrites an enabled entry that points somewhere other than the
// running executable, so moving the exe and starting it once repairs it.
// Returns true when it had to change something.
bool RefreshStartWithWindowsPath();

}  // namespace dm
