// DisplayMirror - the mirroring session.
//
// Owns the D3D device, the capture and the renderer, and the recovery state
// machine that keeps them alive across everything Windows throws at a
// duplication client: a game taking the display, a mode change, the TV being
// switched off, the lock screen, sleep/wake and device resets.
#pragma once

#include <d3d11.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <wrl/client.h>

#include <string>

#include "capture.h"
#include "displays.h"
#include "renderer.h"

namespace dm {

struct MirrorConfig {
  DisplayInfo source;
  DisplayInfo target;
  PresentMode presentMode = PresentMode::VSyncWaitable;
  bool drawCursor = true;
};

// Why a Start() attempt failed, so the UI can say something useful.
enum class StartError {
  None,
  SameDisplay,
  // Source and target are on different GPUs *and* the cross-adapter present
  // path was refused by DXGI. The mismatch on its own is not an error.
  DifferentAdapters,
  DeviceCreationFailed,
  OutputNotFound,
  RendererFailed,
  CaptureFailed,
};

class MirrorSession {
 public:
  ~MirrorSession();

  bool Start(const MirrorConfig& config, StartError* error, std::wstring* message);
  void Stop();
  bool IsRunning() const { return running_; }

  // Runs one iteration: wait for the swap chain, take the newest capture frame,
  // scale it and present. Blocks for at most a few milliseconds when nothing is
  // happening. Returns false when the session has stopped for good.
  bool Tick();

  // Called when Windows reports that the display topology changed.
  void OnDisplayChange();

  // The cursor can be toggled while the session is running.
  void SetDrawCursor(bool draw) {
    config_.drawCursor = draw;
    renderer_.SetDrawCursor(draw);
  }

  // True once the user pressed ESC on the output window.
  bool EscapeRequested() const { return renderer_.EscapeRequested(); }

  const MirrorConfig& Config() const { return config_; }

 private:
  bool CreateDeviceForAdapter(LUID luid);
  // Recomputes crossAdapter_ from the current source and target, logging the
  // transition. Nothing is refused: it only decides what gets said.
  void ApplyAdapterPolicy(bool announce);
  bool FindOutput(const DisplayInfo& display, IDXGIOutput** output);
  bool StartCapture();
  bool BuildPipeline(StartError* error, std::wstring* message);
  void TearDownPipeline();
  void ScheduleRetry(const wchar_t* reason);
  // Emits one line of frame statistics per interval. Silent while nothing is
  // being presented, so an idle desktop does not fill the log.
  void LogFrameStatsIfDue();

  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter_;

  DuplicationCapture capture_;
  Renderer renderer_;
  MirrorConfig config_;

  bool running_ = false;
  // Source and target displays are driven by different GPUs.
  bool crossAdapter_ = false;
  bool needCaptureRestart_ = false;
  bool needFullRestart_ = false;
  DWORD retryAtTick_ = 0;
  UINT retryCount_ = 0;
  bool retryLogged_ = false;
  bool haveFrame_ = false;

  // Frame statistics for the current interval. Presented and acquired should
  // track each other; a high timeout count means the source is producing
  // fewer frames than the target can show, which is normal for video.
  DWORD statsStartedAtTick_ = 0;
  UINT statPresented_ = 0;
  UINT statAcquired_ = 0;
  UINT statTimeouts_ = 0;
  UINT statNothingToShow_ = 0;
};

}  // namespace dm
