// DisplayMirror - borderless output window and flip-model presentation.
//
// Owns the target-side of the pipeline: the borderless window sitting exactly
// on the TV's output rectangle, the flip-model swap chain, the scaling pass
// and the cursor pass.
#pragma once

#include <d3d11.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <wrl/client.h>

#include <string>

#include "capture.h"
#include "displays.h"

namespace dm {

enum class PresentMode {
  // Default. Waitable swap chain, maximum frame latency 1, Present(1, 0).
  // Tear-free output locked to the target's refresh, with no render queue.
  VSyncWaitable,
  // Opt-in. Present(0, DXGI_PRESENT_ALLOW_TEARING) on a swap chain created
  // with DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING. Only offered when DXGI reports
  // DXGI_FEATURE_PRESENT_ALLOW_TEARING.
  AllowTearing,
};

struct RendererOptions {
  PresentMode presentMode = PresentMode::VSyncWaitable;
  bool drawCursor = true;
};

// How the captured pixels are carried to the target, decided from the capture
// format and the two displays' colour states.
struct ColorPlan {
  DXGI_FORMAT swapChainFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
  DXGI_COLOR_SPACE_TYPE colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
  bool toneMapHdrToSdr = false;
  float sdrWhiteNits = 80.0f;
  float maxLuminanceNits = 80.0f;
  std::wstring description;
};

class Renderer {
 public:
  ~Renderer();

  // Creates the device-independent pieces: window class, window, shaders.
  bool Initialize(ID3D11Device* device, const DisplayInfo& target,
                  const RendererOptions& options);
  void Shutdown();

  // Builds the swap chain for the given capture format. Safe to call again
  // after a display change or a source format change; it is a no-op when the
  // resulting colour plan is identical to the one already in place, so
  // recovering from a lost duplication does not flash the output.
  bool ConfigureForSource(DXGI_FORMAT captureFormat, const DisplayInfo& source,
                          const DisplayInfo& target);

  // Repositions the window and resizes the buffers after the target display
  // moved or changed resolution.
  bool UpdateTargetGeometry(const DisplayInfo& target);

  // Blocks until the swap chain is ready for a new frame (waitable mode only).
  // Returns false if the wait failed, which is treated as a device problem.
  bool WaitForPresentReady();

  // Draws the source texture scaled into the target and presents.
  // `hr` receives the Present result so the caller can react to device loss.
  bool RenderAndPresent(const DuplicationCapture& capture, HRESULT* hr);

  // Called when Desktop Duplication reports a new pointer shape; the bitmap
  // is re-uploaded on the next frame that actually draws the cursor.
  void InvalidateCursor() { cursorDirty_ = true; }

  void SetDrawCursor(bool draw) { options_.drawCursor = draw; }
  bool DrawCursor() const { return options_.drawCursor; }

  HWND Window() const { return hwnd_; }
  const ColorPlan& Plan() const { return plan_; }
  bool EscapeRequested() const { return escapeRequested_; }
  void ClearEscapeRequest() { escapeRequested_ = false; }

  // True when DXGI reports tearing support on this system.
  static bool SystemSupportsTearing();

 private:
  bool CreateWindowForTarget(const DisplayInfo& target);
  bool CreateShaders();
  bool CreateSwapChain(const ColorPlan& plan);
  bool CreateBackBufferView();
  void ReleaseSwapChain();
  bool UploadCursor(const CursorImage& image);
  void ComputeLetterbox(UINT sourceWidth, UINT sourceHeight, D3D11_VIEWPORT* viewport) const;

  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
  Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain_;
  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> backBufferRtv_;

  Microsoft::WRL::ComPtr<ID3D11VertexShader> mirrorVs_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> mirrorPs_;
  Microsoft::WRL::ComPtr<ID3D11VertexShader> cursorVs_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> cursorPs_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> mirrorCb_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> cursorCb_;
  Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
  Microsoft::WRL::ComPtr<ID3D11BlendState> opaqueBlend_;
  Microsoft::WRL::ComPtr<ID3D11BlendState> alphaBlend_;
  Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer_;

  Microsoft::WRL::ComPtr<ID3D11Texture2D> cursorTexture_;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> cursorSrv_;
  UINT cursorWidth_ = 0;
  UINT cursorHeight_ = 0;
  POINT cursorHotspot_ = {};
  bool cursorDirty_ = true;

  HWND hwnd_ = nullptr;
  HANDLE frameLatencyWaitable_ = nullptr;
  UINT backBufferWidth_ = 0;
  UINT backBufferHeight_ = 0;
  UINT swapChainFlags_ = 0;

  RendererOptions options_;
  ColorPlan plan_;
  DXGI_FORMAT configuredCaptureFormat_ = DXGI_FORMAT_UNKNOWN;
  bool escapeRequested_ = false;
  bool occluded_ = false;
};

}  // namespace dm
