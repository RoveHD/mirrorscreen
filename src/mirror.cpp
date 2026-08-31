#include "mirror.h"

#include <algorithm>

#include "log.h"

using Microsoft::WRL::ComPtr;

namespace dm {
namespace {

// How long AcquireNextFrame is allowed to block. It is the idle path: with a
// static desktop the thread sleeps in here instead of spinning, and the value
// also bounds how long the message pump has to wait.
constexpr UINT kAcquireTimeoutMs = 16;

// Backoff for reinitialising a lost duplication. Short at first, because the
// common case (a game going full screen, the lock screen appearing) clears in
// well under a second.
constexpr DWORD kRetryDelayMs = 200;
constexpr DWORD kSlowRetryDelayMs = 1000;
constexpr UINT kFastRetryCount = 25;

bool SameLuid(const LUID& a, const LUID& b) {
  return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
}

}  // namespace

MirrorSession::~MirrorSession() { Stop(); }

bool MirrorSession::CreateDeviceForAdapter(LUID luid) {
  device_.Reset();
  adapter_.Reset();

  ComPtr<IDXGIFactory1> factory;
  HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
  if (FAILED(hr)) {
    DM_ERROR(L"CreateDXGIFactory1 failed: %s", HrToString(hr).c_str());
    return false;
  }

  for (UINT i = 0;; ++i) {
    ComPtr<IDXGIAdapter1> adapter;
    if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
    DXGI_ADAPTER_DESC1 desc = {};
    adapter->GetDesc1(&desc);
    if (SameLuid(desc.AdapterLuid, luid)) {
      adapter_ = adapter;
      break;
    }
  }

  if (!adapter_) {
    DM_ERROR(L"The adapter the source display belongs to is no longer present.");
    return false;
  }

  // BGRA_SUPPORT is required for the B8G8R8A8 formats the desktop uses.
  const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                      D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
  D3D_FEATURE_LEVEL obtained = D3D_FEATURE_LEVEL_11_0;

  // DriverType must be UNKNOWN when an adapter is supplied explicitly.
  hr = D3D11CreateDevice(adapter_.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels,
                         _countof(levels), D3D11_SDK_VERSION, &device_, &obtained, nullptr);
  if (FAILED(hr)) {
    DM_ERROR(L"D3D11CreateDevice failed: %s", HrToString(hr).c_str());
    return false;
  }

  // Desktop Duplication feeds one frame at a time from a background source; a
  // deeper DXGI queue would only add latency.
  ComPtr<IDXGIDevice1> dxgiDevice;
  if (SUCCEEDED(device_.As(&dxgiDevice))) dxgiDevice->SetMaximumFrameLatency(1);

  DXGI_ADAPTER_DESC1 desc = {};
  adapter_->GetDesc1(&desc);
  DM_INFO(L"D3D11 device created on %s (feature level %u.%u)", desc.Description,
          static_cast<unsigned>(obtained >> 12) & 0xF,
          static_cast<unsigned>(obtained >> 8) & 0xF);
  return true;
}

bool MirrorSession::FindOutput(const DisplayInfo& display, IDXGIOutput** output) {
  if (!adapter_) return false;
  for (UINT i = 0;; ++i) {
    ComPtr<IDXGIOutput> candidate;
    if (adapter_->EnumOutputs(i, &candidate) == DXGI_ERROR_NOT_FOUND) break;
    DXGI_OUTPUT_DESC desc = {};
    if (FAILED(candidate->GetDesc(&desc))) continue;
    // Match on the device name rather than the index: indices shift when a
    // display is switched off and back on.
    if (display.deviceName == desc.DeviceName) {
      *output = candidate.Detach();
      return true;
    }
  }
  return false;
}

bool MirrorSession::StartCapture() {
  ComPtr<IDXGIOutput> output;
  if (!FindOutput(config_.source, &output)) {
    DM_WARN(L"Source display %s is not currently attached.", config_.source.deviceName.c_str());
    return false;
  }
  return capture_.Start(device_.Get(), output.Get());
}

bool MirrorSession::BuildPipeline(StartError* error, std::wstring* message) {
  if (!CreateDeviceForAdapter(config_.source.adapterLuid)) {
    *error = StartError::DeviceCreationFailed;
    *message = L"The Direct3D device for the source display's GPU could not be created.";
    return false;
  }

  if (!StartCapture()) {
    *error = StartError::CaptureFailed;
    *message = L"Desktop Duplication could not be started on the source display. "
               L"See the log for the exact error.";
    return false;
  }

  RendererOptions options;
  options.presentMode = config_.presentMode;
  options.drawCursor = config_.drawCursor;

  if (!renderer_.Initialize(device_.Get(), config_.target, options)) {
    *error = StartError::RendererFailed;
    *message = L"The output window could not be created on the target display.";
    return false;
  }

  if (!renderer_.ConfigureForSource(capture_.SourceFormat(), config_.source, config_.target)) {
    *error = StartError::RendererFailed;
    *message = L"The swap chain for the target display could not be created.";
    return false;
  }

  renderer_.InvalidateCursor();
  haveFrame_ = false;
  return true;
}

bool MirrorSession::Start(const MirrorConfig& config, StartError* error,
                          std::wstring* message) {
  Stop();

  *error = StartError::None;
  message->clear();
  config_ = config;

  if (config_.source.deviceName == config_.target.deviceName) {
    *error = StartError::SameDisplay;
    *message = L"Source and target must be different displays.";
    return false;
  }

  if (!SameLuid(config_.source.adapterLuid, config_.target.adapterLuid)) {
    // Version 1 deliberately does not build a cross-adapter path. Say so
    // plainly instead of failing somewhere deep in D3D.
    *error = StartError::DifferentAdapters;
    *message = L"Source and target are connected to different GPUs (" +
               config_.source.adapterName + L" and " + config_.target.adapterName +
               L"). DisplayMirror only supports displays on the same GPU. "
               L"Connect both displays to the same graphics card.";
    DM_ERROR(L"Refusing to start: source is on %s, target on %s.",
             config_.source.adapterName.c_str(), config_.target.adapterName.c_str());
    return false;
  }

  DM_INFO(L"Starting: %s (%ux%u @ %.2f Hz) -> %s (%ux%u @ %.2f Hz)",
          config_.source.friendlyName.c_str(), config_.source.width, config_.source.height,
          config_.source.RefreshHz(), config_.target.friendlyName.c_str(),
          config_.target.width, config_.target.height, config_.target.RefreshHz());

  if (!BuildPipeline(error, message)) {
    TearDownPipeline();
    return false;
  }

  running_ = true;
  needCaptureRestart_ = false;
  needFullRestart_ = false;
  retryAtTick_ = 0;
  retryCount_ = 0;
  retryLogged_ = false;
  DM_INFO(L"Mirroring started.");
  return true;
}

void MirrorSession::TearDownPipeline() {
  capture_.Stop();
  renderer_.Shutdown();
  adapter_.Reset();
  device_.Reset();
}

void MirrorSession::Stop() {
  if (running_) DM_INFO(L"Mirroring stopped.");
  TearDownPipeline();
  running_ = false;
  needCaptureRestart_ = false;
  needFullRestart_ = false;
  haveFrame_ = false;
}

void MirrorSession::ScheduleRetry(const wchar_t* reason) {
  ++retryCount_;
  const DWORD delay = retryCount_ <= kFastRetryCount ? kRetryDelayMs : kSlowRetryDelayMs;
  retryAtTick_ = GetTickCount() + delay;

  // Log the transition, not every attempt: a game holding the display can keep
  // us retrying for minutes and the log must stay readable.
  if (!retryLogged_) {
    DM_WARN(L"%s Reinitialising; will keep retrying.", reason);
    retryLogged_ = true;
  }
}

void MirrorSession::OnDisplayChange() {
  if (!running_) return;

  DM_INFO(L"Display configuration changed; re-checking source and target.");
  const std::vector<DisplayInfo> displays = EnumerateDisplays();
  LogDisplays(displays);

  const DisplayInfo* source = FindMatchingDisplay(displays, config_.source);
  const DisplayInfo* target = FindMatchingDisplay(displays, config_.target);

  if (!source) {
    DM_WARN(L"Source display %s is gone; waiting for it to come back.",
            config_.source.friendlyName.c_str());
    needFullRestart_ = true;
    ScheduleRetry(L"Source display lost.");
    return;
  }
  if (!target) {
    DM_WARN(L"Target display %s is gone (TV switched off or HDMI unplugged); "
            L"waiting for it to come back.", config_.target.friendlyName.c_str());
    needFullRestart_ = true;
    ScheduleRetry(L"Target display lost.");
    return;
  }

  // Classify the change before the new state is copied over the old one.
  // A resolution or HDR change on either end alters the duplication surface or
  // the swap chain format and needs a rebuild; a target that merely moved or
  // was resized only needs the window and the buffers to follow it.
  const bool sourceNeedsRebuild = source->width != config_.source.width ||
                                  source->height != config_.source.height ||
                                  source->hdrEnabled != config_.source.hdrEnabled;
  const bool targetNeedsRebuild = target->hdrEnabled != config_.target.hdrEnabled ||
                                  target->bitsPerColor != config_.target.bitsPerColor;
  const bool targetMoved =
      target->width != config_.target.width || target->height != config_.target.height ||
      target->desktopCoordinates.left != config_.target.desktopCoordinates.left ||
      target->desktopCoordinates.top != config_.target.desktopCoordinates.top;

  if (sourceNeedsRebuild) {
    DM_INFO(L"Source changed: %ux%u @ %.2f Hz, %s", source->width, source->height,
            source->RefreshHz(), source->hdrEnabled ? L"HDR" : L"SDR");
  }
  if (targetNeedsRebuild || targetMoved) {
    DM_INFO(L"Target changed: %ux%u @ %.2f Hz, %s", target->width, target->height,
            target->RefreshHz(), target->hdrEnabled ? L"HDR" : L"SDR");
  }

  config_.source = *source;
  config_.target = *target;

  if (sourceNeedsRebuild) {
    needFullRestart_ = true;
    ScheduleRetry(L"Source mode changed.");
    return;
  }
  if (targetNeedsRebuild) {
    needFullRestart_ = true;
    ScheduleRetry(L"Target colour mode changed.");
    return;
  }
  if (targetMoved && !renderer_.UpdateTargetGeometry(config_.target)) {
    needFullRestart_ = true;
    ScheduleRetry(L"Resizing the output failed.");
  }
}

bool MirrorSession::Tick() {
  if (!running_) return false;

  // Recovery path. Everything that went wrong ends up here, throttled so a
  // display that stays unavailable does not turn into a busy loop.
  if (needFullRestart_ || needCaptureRestart_) {
    const DWORD now = GetTickCount();
    // Signed comparison so the 49-day tick wrap cannot strand us.
    if (static_cast<LONG>(now - retryAtTick_) < 0) {
      Sleep(10);
      return true;
    }

    if (needFullRestart_) {
      TearDownPipeline();

      // Re-read the topology: the display we are about to rebuild on may have
      // come back with a different resolution, refresh rate or HDR state.
      const std::vector<DisplayInfo> displays = EnumerateDisplays();
      const DisplayInfo* source = FindMatchingDisplay(displays, config_.source);
      const DisplayInfo* target = FindMatchingDisplay(displays, config_.target);
      if (!source || !target) {
        ScheduleRetry(L"Waiting for both displays to be available.");
        return true;
      }
      config_.source = *source;
      config_.target = *target;

      if (!SameLuid(config_.source.adapterLuid, config_.target.adapterLuid)) {
        DM_ERROR(L"Source and target are now on different GPUs; stopping.");
        Stop();
        return false;
      }

      StartError error = StartError::None;
      std::wstring message;
      if (!BuildPipeline(&error, &message)) {
        TearDownPipeline();
        ScheduleRetry(L"Rebuilding the pipeline failed.");
        return true;
      }
      needFullRestart_ = false;
      needCaptureRestart_ = false;
    } else {
      capture_.Stop();
      if (!StartCapture()) {
        ScheduleRetry(L"Desktop Duplication is not available yet.");
        return true;
      }

      // A full-screen application can hand the duplication back with a
      // different format or size than we set the swap chain up for.
      if (capture_.SourceFormat() != DXGI_FORMAT_UNKNOWN) {
        if (!renderer_.ConfigureForSource(capture_.SourceFormat(), config_.source,
                                          config_.target)) {
          ScheduleRetry(L"Reconfiguring the output failed.");
          return true;
        }
      }
      needCaptureRestart_ = false;
    }

    renderer_.InvalidateCursor();
    haveFrame_ = false;
    retryCount_ = 0;
    retryLogged_ = false;
    DM_INFO(L"Duplication reinitialised; mirroring resumed.");
    return true;
  }

  // Wait for the swap chain first, then take the newest capture frame. Doing
  // it in this order is what keeps latency down: by the time we ask Desktop
  // Duplication for a frame, the target is already ready to show it.
  if (!renderer_.WaitForPresentReady()) {
    needFullRestart_ = true;
    ScheduleRetry(L"The swap chain stopped signalling.");
    return true;
  }

  const DuplicationCapture::FrameResult frame = capture_.AcquireFrame(kAcquireTimeoutMs);

  switch (frame.status) {
    case DuplicationCapture::Status::Timeout:
      // No new content on the source. Nothing to present: the flip model keeps
      // the last frame on screen, so this costs nothing and adds no latency.
      return true;

    case DuplicationCapture::Status::Lost:
      // The classic DXGI_ERROR_ACCESS_LOST: a full-screen application took the
      // display, the desktop switched (lock screen, UAC), or the mode changed.
      needCaptureRestart_ = true;
      ScheduleRetry(L"Desktop Duplication access lost.");
      return true;

    case DuplicationCapture::Status::Fatal:
      DM_WARN(L"The graphics device was reset or removed; rebuilding everything.");
      needFullRestart_ = true;
      ScheduleRetry(L"Graphics device lost.");
      return true;

    case DuplicationCapture::Status::Frame:
      break;
  }

  if (frame.pointerShapeChanged) renderer_.InvalidateCursor();
  if (frame.desktopUpdated) haveFrame_ = true;

  // A pointer-only update still has to be presented, because we composite the
  // cursor ourselves. But there is nothing to show before the first real frame.
  if (!haveFrame_) return true;
  if (!frame.desktopUpdated && !frame.pointerMoved && !frame.pointerShapeChanged) {
    return true;
  }

  HRESULT presentHr = S_OK;
  if (!renderer_.RenderAndPresent(capture_, &presentHr)) {
    if (presentHr == DXGI_ERROR_DEVICE_REMOVED || presentHr == DXGI_ERROR_DEVICE_RESET) {
      const HRESULT reason = device_ ? device_->GetDeviceRemovedReason() : S_OK;
      DM_ERROR(L"Present failed, device removed. Reason: %s", HrToString(reason).c_str());
    } else {
      DM_WARN(L"Present failed: %s", HrToString(presentHr).c_str());
    }
    needFullRestart_ = true;
    ScheduleRetry(L"Presentation failed.");
  }

  return true;
}

}  // namespace dm
