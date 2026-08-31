#include "displays.h"

#include <wrl/client.h>

#include <cstdio>
#include <map>

#include "log.h"

using Microsoft::WRL::ComPtr;

namespace dm {
namespace {

// The subset of QueryDisplayConfig data we care about, keyed by GDI device
// name so it can be joined onto the DXGI output list.
struct PathExtras {
  std::wstring friendlyName;
  std::wstring monitorDevicePath;
  UINT refreshNumerator = 0;
  UINT refreshDenominator = 0;
  float sdrWhiteLevelNits = 80.0f;
  bool advancedColorEnabled = false;
};

// Windows expresses the SDR white level as a multiple of 80 nits scaled by
// 1000 (so 1000 == 80 nits == scRGB 1.0). Anything outside a sane range means
// the query failed or the display lied; fall back to the scRGB reference.
float DecodeSdrWhiteLevel(ULONG raw) {
  const float nits = static_cast<float>(raw) / 1000.0f * 80.0f;
  if (nits < 40.0f || nits > 1000.0f) return 80.0f;
  return nits;
}

std::map<std::wstring, PathExtras> QueryPathExtras() {
  std::map<std::wstring, PathExtras> result;

  UINT32 pathCount = 0;
  UINT32 modeCount = 0;
  if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) {
    return result;
  }

  std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
  std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
  if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount,
                         modes.data(), nullptr) != ERROR_SUCCESS) {
    return result;
  }
  paths.resize(pathCount);
  modes.resize(modeCount);

  for (const DISPLAYCONFIG_PATH_INFO& path : paths) {
    DISPLAYCONFIG_SOURCE_DEVICE_NAME source = {};
    source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    source.header.size = sizeof(source);
    source.header.adapterId = path.sourceInfo.adapterId;
    source.header.id = path.sourceInfo.id;
    if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) continue;

    PathExtras extras;
    extras.refreshNumerator = path.targetInfo.refreshRate.Numerator;
    extras.refreshDenominator = path.targetInfo.refreshRate.Denominator;

    DISPLAYCONFIG_TARGET_DEVICE_NAME target = {};
    target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    target.header.size = sizeof(target);
    target.header.adapterId = path.targetInfo.adapterId;
    target.header.id = path.targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS) {
      extras.friendlyName = target.monitorFriendlyDeviceName;
      // The device interface path of the physical monitor. Stable across
      // reboots and across the display being switched off, and distinct for
      // two monitors of the same model.
      extras.monitorDevicePath = target.monitorDevicePath;
    }

    DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO advanced = {};
    advanced.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
    advanced.header.size = sizeof(advanced);
    advanced.header.adapterId = path.targetInfo.adapterId;
    advanced.header.id = path.targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&advanced.header) == ERROR_SUCCESS) {
      extras.advancedColorEnabled = advanced.advancedColorEnabled != 0;
    }

    DISPLAYCONFIG_SDR_WHITE_LEVEL white = {};
    white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
    white.header.size = sizeof(white);
    white.header.adapterId = path.targetInfo.adapterId;
    white.header.id = path.targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&white.header) == ERROR_SUCCESS) {
      extras.sdrWhiteLevelNits = DecodeSdrWhiteLevel(white.SDRWhiteLevel);
    }

    result.emplace(source.viewGdiDeviceName, extras);
  }

  return result;
}

// DXGI does not report the refresh rate of an output, only of its mode list.
// If QueryDisplayConfig did not cover this output, fall back to the integer
// frequency from EnumDisplaySettings.
void FillRefreshFallback(DisplayInfo& info) {
  if (info.refreshDenominator != 0 && info.refreshNumerator != 0) return;
  DEVMODEW mode = {};
  mode.dmSize = sizeof(mode);
  if (EnumDisplaySettingsW(info.deviceName.c_str(), ENUM_CURRENT_SETTINGS, &mode)) {
    info.refreshNumerator = mode.dmDisplayFrequency;
    info.refreshDenominator = 1;
  }
}

}  // namespace

std::wstring DisplayInfo::Describe() const {
  wchar_t buf[512];
  _snwprintf_s(buf, _countof(buf), _TRUNCATE,
               L"%s  |  %ux%u @ %.2f Hz  |  Pos %d,%d  |  %s  |  %s%s",
               friendlyName.c_str(), width, height, RefreshHz(),
               static_cast<int>(desktopCoordinates.left),
               static_cast<int>(desktopCoordinates.top), adapterName.c_str(),
               hdrEnabled ? L"HDR" : L"SDR", primary ? L"  |  Primary" : L"");
  return buf;
}

std::vector<DisplayInfo> EnumerateDisplays() {
  std::vector<DisplayInfo> displays;

  ComPtr<IDXGIFactory1> factory;
  HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
  if (FAILED(hr)) {
    DM_ERROR(L"CreateDXGIFactory1 failed: %s", HrToString(hr).c_str());
    return displays;
  }

  const std::map<std::wstring, PathExtras> extras = QueryPathExtras();

  for (UINT adapterIndex = 0;; ++adapterIndex) {
    ComPtr<IDXGIAdapter1> adapter;
    if (factory->EnumAdapters1(adapterIndex, &adapter) == DXGI_ERROR_NOT_FOUND) break;

    DXGI_ADAPTER_DESC1 adapterDesc = {};
    adapter->GetDesc1(&adapterDesc);

    for (UINT outputIndex = 0;; ++outputIndex) {
      ComPtr<IDXGIOutput> output;
      if (adapter->EnumOutputs(outputIndex, &output) == DXGI_ERROR_NOT_FOUND) break;

      DXGI_OUTPUT_DESC desc = {};
      if (FAILED(output->GetDesc(&desc)) || !desc.AttachedToDesktop) continue;

      DisplayInfo info;
      info.adapterIndex = adapterIndex;
      info.outputIndex = outputIndex;
      info.adapterLuid = adapterDesc.AdapterLuid;
      info.adapterName = adapterDesc.Description;
      info.deviceName = desc.DeviceName;
      info.friendlyName = desc.DeviceName;
      info.monitor = desc.Monitor;
      info.desktopCoordinates = desc.DesktopCoordinates;
      info.width = static_cast<UINT>(desc.DesktopCoordinates.right - desc.DesktopCoordinates.left);
      info.height = static_cast<UINT>(desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top);
      info.rotation = desc.Rotation;

      MONITORINFO monitorInfo = {};
      monitorInfo.cbSize = sizeof(monitorInfo);
      if (GetMonitorInfoW(desc.Monitor, &monitorInfo)) {
        info.primary = (monitorInfo.dwFlags & MONITORINFOF_PRIMARY) != 0;
      }

      // IDXGIOutput6 reports the colour space of the *active* mode, which is
      // the authoritative signal for "is this display in HDR mode right now".
      ComPtr<IDXGIOutput6> output6;
      if (SUCCEEDED(output.As(&output6))) {
        DXGI_OUTPUT_DESC1 desc1 = {};
        if (SUCCEEDED(output6->GetDesc1(&desc1))) {
          info.colorSpace = desc1.ColorSpace;
          info.bitsPerColor = desc1.BitsPerColor;
          info.maxLuminanceNits = desc1.MaxLuminance;
          info.hdrEnabled =
              desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
        }
      }

      const auto it = extras.find(info.deviceName);
      if (it != extras.end()) {
        if (!it->second.friendlyName.empty()) info.friendlyName = it->second.friendlyName;
        info.persistentId = it->second.monitorDevicePath;
        info.refreshNumerator = it->second.refreshNumerator;
        info.refreshDenominator = it->second.refreshDenominator;
        info.sdrWhiteLevelNits = it->second.sdrWhiteLevelNits;
        // DisplayConfig and DXGI can disagree briefly during a mode change;
        // treat either saying "advanced colour" as HDR so we never silently
        // render HDR content down an SDR path.
        info.hdrEnabled = info.hdrEnabled || it->second.advancedColorEnabled;
      }
      FillRefreshFallback(info);
      // Without a monitor device path there is still an identity worth having,
      // even if it only survives until the displays are renumbered.
      if (info.persistentId.empty()) info.persistentId = info.deviceName;

      if (info.maxLuminanceNits < info.sdrWhiteLevelNits) {
        info.maxLuminanceNits = info.sdrWhiteLevelNits;
      }

      displays.push_back(std::move(info));
    }
  }

  return displays;
}

void LogDisplays(const std::vector<DisplayInfo>& displays) {
  DM_INFO(L"%zu active display(s) detected", displays.size());
  LUID previousLuid = {};
  for (const DisplayInfo& d : displays) {
    if (d.adapterLuid.LowPart != previousLuid.LowPart ||
        d.adapterLuid.HighPart != previousLuid.HighPart) {
      DM_INFO(L"  Adapter: %s (LUID %08X-%08X)", d.adapterName.c_str(),
              static_cast<unsigned>(d.adapterLuid.HighPart),
              static_cast<unsigned>(d.adapterLuid.LowPart));
      previousLuid = d.adapterLuid;
    }
    DM_INFO(L"    %s (%s): %ux%u @ %.3f Hz, pos %d,%d, %u bpc, %s%s",
            d.friendlyName.c_str(), d.deviceName.c_str(), d.width, d.height,
            d.RefreshHz(), static_cast<int>(d.desktopCoordinates.left),
            static_cast<int>(d.desktopCoordinates.top), d.bitsPerColor,
            d.hdrEnabled ? L"HDR" : L"SDR", d.primary ? L", primary" : L"");
  }
}

const DisplayInfo* FindDisplayById(const std::vector<DisplayInfo>& displays,
                                   const std::wstring& persistentId) {
  if (persistentId.empty()) return nullptr;
  for (const DisplayInfo& d : displays) {
    if (d.persistentId == persistentId) return &d;
  }
  return nullptr;
}

const DisplayInfo* FindMatchingDisplay(const std::vector<DisplayInfo>& displays,
                                       const DisplayInfo& previous) {
  // The monitor device path first: device names are renumbered when a display
  // is switched off and back on, which is precisely when this is called.
  if (const DisplayInfo* byId = FindDisplayById(displays, previous.persistentId)) {
    return byId;
  }
  for (const DisplayInfo& d : displays) {
    if (d.deviceName == previous.deviceName) return &d;
  }
  for (const DisplayInfo& d : displays) {
    if (d.monitor != nullptr && d.monitor == previous.monitor) return &d;
  }
  return nullptr;
}

}  // namespace dm
