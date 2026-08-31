// DisplayMirror - enumeration of the physical displays attached to the system.
//
// DXGI gives us the adapter/output topology (which is what capture and the
// D3D device need); QueryDisplayConfig fills in the parts DXGI does not
// expose: the monitor's friendly name, the exact rational refresh rate, and
// the SDR white level that Windows uses when an HDR display is active.
#pragma once

#include <dxgi1_6.h>
#include <windows.h>

#include <string>
#include <vector>

namespace dm {

struct DisplayInfo {
  // Topology. adapterLuid decides how a frame reaches the target: when source
  // and target share it everything stays on one GPU, otherwise DWM copies each
  // presented frame across to the target's adapter.
  UINT adapterIndex = 0;
  UINT outputIndex = 0;
  LUID adapterLuid = {};
  std::wstring adapterName;

  std::wstring deviceName;    // "\\.\DISPLAY1"
  std::wstring friendlyName;  // "LG TV SSCR2", falls back to deviceName
  HMONITOR monitor = nullptr;

  // Stable identity of the physical monitor, from the display config's
  // monitorDevicePath. Unlike deviceName it survives a reboot and a display
  // being switched off, and unlike friendlyName it tells two identical
  // monitors apart. This is what a saved source/target pair is stored as.
  // Falls back to deviceName when the path is unavailable.
  std::wstring persistentId;

  RECT desktopCoordinates = {};
  UINT width = 0;
  UINT height = 0;
  UINT refreshNumerator = 0;
  UINT refreshDenominator = 0;
  DXGI_MODE_ROTATION rotation = DXGI_MODE_ROTATION_IDENTITY;
  bool primary = false;

  // Colour. hdrEnabled reflects the *currently active* mode, not the panel's
  // capability: it is what decides how we set up the swap chain.
  bool hdrEnabled = false;
  DXGI_COLOR_SPACE_TYPE colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
  UINT bitsPerColor = 8;
  float sdrWhiteLevelNits = 80.0f;
  float maxLuminanceNits = 80.0f;

  float RefreshHz() const {
    if (refreshDenominator == 0) return 0.0f;
    return static_cast<float>(refreshNumerator) / static_cast<float>(refreshDenominator);
  }

  // One line for the config window's dropdown.
  std::wstring Describe() const;
};

// Enumerates every output that is attached to the desktop, across all adapters.
// Returns an empty vector only if DXGI itself fails.
std::vector<DisplayInfo> EnumerateDisplays();

// Logs the full topology. Called once at startup and again after a display
// change, so the log explains what the user was looking at.
void LogDisplays(const std::vector<DisplayInfo>& displays);

// Finds the display that still matches `previous` after a display-configuration
// change. Matches on the persistent monitor id first, then on device name, then
// on the monitor handle. Returns nullptr when the display is gone (TV switched
// off, HDMI unplugged).
const DisplayInfo* FindMatchingDisplay(const std::vector<DisplayInfo>& displays,
                                       const DisplayInfo& previous);

// Looks up a display by the persistent id a saved configuration refers to.
// Returns nullptr when that monitor is not currently attached.
const DisplayInfo* FindDisplayById(const std::vector<DisplayInfo>& displays,
                                   const std::wstring& persistentId);

}  // namespace dm
