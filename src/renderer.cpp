#include "renderer.h"

#include <d3dcompiler.h>

#include <algorithm>

#include "log.h"

using Microsoft::WRL::ComPtr;

namespace dm {
namespace {

constexpr wchar_t kWindowClass[] = L"DisplayMirrorOutputWindow";

// One HLSL blob, four entry points. Compiled at startup with D3DCompile;
// d3dcompiler_47.dll is a Windows component on Windows 10 and 11, so this adds
// no redistributable dependency.
constexpr char kShaderSource[] = R"HLSL(
struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

cbuffer MirrorCB : register(b0) {
    uint  gRotation;      // 0 = identity, 1 = 90, 2 = 180, 3 = 270
    uint  gToneMap;       // 1 = scRGB HDR source -> SDR target
    float gSdrWhiteNits;  // SDR reference white of the source display
    float gMaxLumNits;    // Peak luminance of the source display
};

// A distinct register from MirrorCB on purpose: the two are used by different
// stages, but sharing b0 in one file is an overlapping-register error.
cbuffer CursorCB : register(b1) {
    float4 gCursorRect;   // x0, y0, x1, y1 in the viewport's NDC
};

Texture2D    gTexture : register(t0);
SamplerState gSampler : register(s0);

// Full-screen triangle. No vertex or index buffer required.
VSOut VSMirror(uint vid : SV_VertexID) {
    VSOut o;
    float2 uv = float2(float((vid << 1) & 2), float(vid & 2));
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.uv  = uv;
    return o;
}

// The duplication surface is always un-rotated; the desktop image is rotated
// inside it. Undo that here so the target sees an upright picture.
float2 ApplyRotation(float2 uv, uint rotation) {
    if (rotation == 1) return float2(uv.y, 1.0 - uv.x);
    if (rotation == 2) return float2(1.0 - uv.x, 1.0 - uv.y);
    if (rotation == 3) return float2(1.0 - uv.y, uv.x);
    return uv;
}

float3 LinearToSrgb(float3 c) {
    c = saturate(c);
    return (c <= 0.0031308) ? (c * 12.92)
                            : (1.055 * pow(c, 1.0 / 2.4) - 0.055);
}

float4 PSMirror(VSOut i) : SV_Target {
    float4 c = gTexture.Sample(gSampler, ApplyRotation(i.uv, gRotation));

    if (gToneMap != 0) {
        // The capture is scRGB: linear, Rec.709 primaries, 1.0 == 80 nits.
        // Rescale so 1.0 is the source display's SDR white, then roll the
        // highlights off with an extended Reinhard curve and re-encode to sRGB.
        // This is an approximation, and it is logged as one.
        float3 l = max(c.rgb, 0.0) * (80.0 / max(gSdrWhiteNits, 1.0));
        float  white = max(gMaxLumNits / max(gSdrWhiteNits, 1.0), 1.0001);
        l = (l * (1.0 + l / (white * white))) / (1.0 + l);
        c.rgb = LinearToSrgb(l);
    }

    return float4(c.rgb, 1.0);
}

VSOut VSCursor(uint vid : SV_VertexID) {
    VSOut o;
    float2 t = float2(float(vid & 1), float((vid >> 1) & 1));
    o.pos = float4(lerp(gCursorRect.xy, gCursorRect.zw, t), 0.0, 1.0);
    o.uv  = t;
    return o;
}

float4 PSCursor(VSOut i) : SV_Target {
    return gTexture.Sample(gSampler, i.uv);
}
)HLSL";

struct MirrorConstants {
  UINT rotation;
  UINT toneMap;
  float sdrWhiteNits;
  float maxLumNits;
};

struct CursorConstants {
  float rect[4];
};

const wchar_t* ColorSpaceName(DXGI_COLOR_SPACE_TYPE space) {
  switch (space) {
    case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709: return L"sRGB / G22 P709";
    case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709: return L"scRGB / linear P709";
    case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020: return L"HDR10 / PQ P2020";
    default: return L"<other>";
  }
}

const wchar_t* FormatName(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_B8G8R8A8_UNORM: return L"B8G8R8A8_UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM: return L"R8G8B8A8_UNORM";
    case DXGI_FORMAT_R10G10B10A2_UNORM: return L"R10G10B10A2_UNORM";
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return L"R16G16B16A16_FLOAT";
    default: return L"<other>";
  }
}

ComPtr<ID3DBlob> CompileShader(const char* entryPoint, const char* target) {
  ComPtr<ID3DBlob> code;
  ComPtr<ID3DBlob> errors;
  UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_ENABLE_STRICTNESS;
  const HRESULT hr =
      D3DCompile(kShaderSource, sizeof(kShaderSource) - 1, "DisplayMirror.hlsl",
                 nullptr, nullptr, entryPoint, target, flags, 0, &code, &errors);
  if (FAILED(hr)) {
    if (errors) {
      DM_ERROR(L"Shader '%hs' failed to compile: %hs", entryPoint,
               static_cast<const char*>(errors->GetBufferPointer()));
    } else {
      DM_ERROR(L"Shader '%hs' failed to compile: %s", entryPoint, HrToString(hr).c_str());
    }
    return nullptr;
  }
  return code;
}

// Chooses the swap chain format and colour space from the capture format and
// the two displays' current colour state. The rule is "follow the source":
// an SDR capture goes out as SDR (Windows composites that correctly onto an
// HDR TV), and an HDR capture stays in scRGB when the TV can take it.
ColorPlan DecideColorPlan(DXGI_FORMAT captureFormat, const DisplayInfo& source,
                          const DisplayInfo& target) {
  ColorPlan plan;
  plan.sdrWhiteNits = source.sdrWhiteLevelNits;
  plan.maxLuminanceNits = source.maxLuminanceNits;

  const bool hdrCapture = captureFormat == DXGI_FORMAT_R16G16B16A16_FLOAT;

  if (hdrCapture && target.hdrEnabled) {
    // Direct pass-through: Desktop Duplication hands us scRGB and the swap
    // chain takes scRGB, so no conversion happens at all.
    plan.swapChainFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    plan.colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
    plan.toneMapHdrToSdr = false;
    plan.description = L"HDR pass-through (scRGB FP16, source and target both HDR)";
    return plan;
  }

  if (hdrCapture) {
    // HDR source, SDR target. Converting is unavoidable; do it explicitly and
    // say so, rather than letting the values clip into a washed-out picture.
    plan.swapChainFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    plan.colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    plan.toneMapHdrToSdr = true;
    plan.description = L"HDR source on an SDR target: approximate tone mapping (limited mode)";
    return plan;
  }

  // SDR capture. Keep 10-bit precision if both ends have it.
  if (captureFormat == DXGI_FORMAT_R10G10B10A2_UNORM && target.bitsPerColor >= 10) {
    plan.swapChainFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
  } else {
    plan.swapChainFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
  }
  plan.colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
  plan.toneMapHdrToSdr = false;
  plan.description = L"SDR pass-through";
  return plan;
}

}  // namespace

bool Renderer::SystemSupportsTearing() {
  ComPtr<IDXGIFactory5> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
  BOOL allowTearing = FALSE;
  if (FAILED(factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                                          &allowTearing, sizeof(allowTearing)))) {
    return false;
  }
  return allowTearing != FALSE;
}

Renderer::~Renderer() { Shutdown(); }

LRESULT CALLBACK Renderer::WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  auto* self = reinterpret_cast<Renderer*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

  switch (msg) {
    case WM_NCCREATE: {
      auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(create->lpCreateParams));
      break;
    }
    case WM_KEYDOWN:
      if (wparam == VK_ESCAPE && self) {
        self->escapeRequested_ = true;
        return 0;
      }
      break;
    case WM_CLOSE:
      if (self) self->escapeRequested_ = true;
      return 0;
    case WM_ERASEBKGND:
      return 1;  // The swap chain owns every pixel; never let GDI paint.
    case WM_PAINT: {
      PAINTSTRUCT ps;
      BeginPaint(hwnd, &ps);
      EndPaint(hwnd, &ps);
      return 0;
    }
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

bool Renderer::Initialize(ID3D11Device* device, const DisplayInfo& target,
                          const RendererOptions& options) {
  Shutdown();

  device_ = device;
  device_->GetImmediateContext(&context_);
  options_ = options;

  if (!CreateWindowForTarget(target)) return false;
  if (!CreateShaders()) return false;
  return true;
}

void Renderer::Shutdown() {
  ReleaseSwapChain();

  cursorSrv_.Reset();
  cursorTexture_.Reset();
  cursorWidth_ = cursorHeight_ = 0;
  cursorDirty_ = true;

  rasterizer_.Reset();
  alphaBlend_.Reset();
  opaqueBlend_.Reset();
  sampler_.Reset();
  cursorCb_.Reset();
  mirrorCb_.Reset();
  cursorPs_.Reset();
  cursorVs_.Reset();
  mirrorPs_.Reset();
  mirrorVs_.Reset();

  if (hwnd_) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }

  context_.Reset();
  device_.Reset();
  escapeRequested_ = false;
  occluded_ = false;
}

bool Renderer::CreateWindowForTarget(const DisplayInfo& target) {
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = &Renderer::WndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hCursor = nullptr;  // No cursor over the mirror; we draw the source's.
  wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
  wc.lpszClassName = kWindowClass;
  RegisterClassExW(&wc);  // Harmless if it is already registered.

  const RECT& r = target.desktopCoordinates;

  // WS_POPUP gives a window with no frame and no title bar. WS_EX_NOACTIVATE
  // is the important one for game compatibility: the mirror must never steal
  // focus from the game running on the source monitor. WS_EX_TOOLWINDOW keeps
  // it out of Alt+Tab, and WS_EX_TOPMOST keeps it above the taskbar on the TV.
  hwnd_ = CreateWindowExW(
      WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, kWindowClass,
      L"DisplayMirror", WS_POPUP, r.left, r.top, r.right - r.left, r.bottom - r.top,
      nullptr, nullptr, GetModuleHandleW(nullptr), this);

  if (!hwnd_) {
    DM_ERROR(L"CreateWindowEx for the output window failed: %s",
             HrToString(HRESULT_FROM_WIN32(GetLastError())).c_str());
    return false;
  }

  ShowWindow(hwnd_, SW_SHOWNA);  // SHOWNA: show without activating.
  return true;
}

bool Renderer::CreateShaders() {
  ComPtr<ID3DBlob> vsMirror = CompileShader("VSMirror", "vs_5_0");
  ComPtr<ID3DBlob> psMirror = CompileShader("PSMirror", "ps_5_0");
  ComPtr<ID3DBlob> vsCursor = CompileShader("VSCursor", "vs_5_0");
  ComPtr<ID3DBlob> psCursor = CompileShader("PSCursor", "ps_5_0");
  if (!vsMirror || !psMirror || !vsCursor || !psCursor) return false;

  HRESULT hr = device_->CreateVertexShader(vsMirror->GetBufferPointer(),
                                           vsMirror->GetBufferSize(), nullptr, &mirrorVs_);
  if (SUCCEEDED(hr)) {
    hr = device_->CreatePixelShader(psMirror->GetBufferPointer(),
                                    psMirror->GetBufferSize(), nullptr, &mirrorPs_);
  }
  if (SUCCEEDED(hr)) {
    hr = device_->CreateVertexShader(vsCursor->GetBufferPointer(),
                                     vsCursor->GetBufferSize(), nullptr, &cursorVs_);
  }
  if (SUCCEEDED(hr)) {
    hr = device_->CreatePixelShader(psCursor->GetBufferPointer(),
                                    psCursor->GetBufferSize(), nullptr, &cursorPs_);
  }
  if (FAILED(hr)) {
    DM_ERROR(L"Creating shaders failed: %s", HrToString(hr).c_str());
    return false;
  }

  D3D11_BUFFER_DESC cb = {};
  cb.ByteWidth = sizeof(MirrorConstants);
  cb.Usage = D3D11_USAGE_DYNAMIC;
  cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  hr = device_->CreateBuffer(&cb, nullptr, &mirrorCb_);
  if (SUCCEEDED(hr)) {
    cb.ByteWidth = sizeof(CursorConstants);
    hr = device_->CreateBuffer(&cb, nullptr, &cursorCb_);
  }
  if (FAILED(hr)) {
    DM_ERROR(L"Creating constant buffers failed: %s", HrToString(hr).c_str());
    return false;
  }

  D3D11_SAMPLER_DESC sampler = {};
  sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler.MaxLOD = D3D11_FLOAT32_MAX;
  hr = device_->CreateSamplerState(&sampler, &sampler_);
  if (FAILED(hr)) {
    DM_ERROR(L"CreateSamplerState failed: %s", HrToString(hr).c_str());
    return false;
  }

  D3D11_BLEND_DESC blend = {};
  blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  hr = device_->CreateBlendState(&blend, &opaqueBlend_);
  if (SUCCEEDED(hr)) {
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    hr = device_->CreateBlendState(&blend, &alphaBlend_);
  }
  if (FAILED(hr)) {
    DM_ERROR(L"CreateBlendState failed: %s", HrToString(hr).c_str());
    return false;
  }

  D3D11_RASTERIZER_DESC raster = {};
  raster.FillMode = D3D11_FILL_SOLID;
  raster.CullMode = D3D11_CULL_NONE;  // Both passes are simple screen-space geometry.
  raster.DepthClipEnable = TRUE;
  hr = device_->CreateRasterizerState(&raster, &rasterizer_);
  if (FAILED(hr)) {
    DM_ERROR(L"CreateRasterizerState failed: %s", HrToString(hr).c_str());
    return false;
  }

  return true;
}

void Renderer::ReleaseSwapChain() {
  if (context_) {
    // A swap chain cannot be released while its buffers are bound.
    context_->OMSetRenderTargets(0, nullptr, nullptr);
    context_->Flush();
  }
  backBufferRtv_.Reset();
  if (frameLatencyWaitable_) {
    CloseHandle(frameLatencyWaitable_);
    frameLatencyWaitable_ = nullptr;
  }
  presentCredit_ = false;
  swapChain_.Reset();
  backBufferWidth_ = backBufferHeight_ = 0;
  swapChainFlags_ = 0;
  configuredCaptureFormat_ = DXGI_FORMAT_UNKNOWN;
}

bool Renderer::CreateSwapChain(const ColorPlan& plan) {
  ReleaseSwapChain();

  ComPtr<IDXGIDevice> dxgiDevice;
  HRESULT hr = device_.As(&dxgiDevice);
  if (FAILED(hr)) return false;

  ComPtr<IDXGIAdapter> adapter;
  hr = dxgiDevice->GetAdapter(&adapter);
  if (FAILED(hr)) return false;

  ComPtr<IDXGIFactory2> factory;
  hr = adapter->GetParent(IID_PPV_ARGS(&factory));
  if (FAILED(hr)) {
    DM_ERROR(L"Getting the DXGI factory failed: %s", HrToString(hr).c_str());
    return false;
  }

  RECT client = {};
  GetClientRect(hwnd_, &client);
  const UINT width = std::max<UINT>(1, static_cast<UINT>(client.right - client.left));
  const UINT height = std::max<UINT>(1, static_cast<UINT>(client.bottom - client.top));

  UINT flags = 0;
  if (options_.presentMode == PresentMode::AllowTearing) {
    flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
  } else {
    // The waitable object lets us block until the swap chain is ready for the
    // next frame instead of letting Present build a queue behind our back.
    flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
  }

  DXGI_SWAP_CHAIN_DESC1 desc = {};
  desc.Width = width;
  desc.Height = height;
  desc.Format = plan.swapChainFormat;
  desc.SampleDesc.Count = 1;  // Flip model does not allow MSAA on the back buffer.
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.BufferCount = 2;  // Minimum for FLIP_DISCARD: one on screen, one to draw.
  desc.Scaling = DXGI_SCALING_NONE;
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
  desc.Flags = flags;

  hr = factory->CreateSwapChainForHwnd(device_.Get(), hwnd_, &desc, nullptr, nullptr,
                                       &swapChain_);
  if (FAILED(hr)) {
    DM_ERROR(L"CreateSwapChainForHwnd (%s) failed: %s", FormatName(plan.swapChainFormat),
             HrToString(hr).c_str());
    return false;
  }

  // We stay in borderless windowed mode on purpose. SetFullscreenState would
  // give DXGI ownership of the display mode, which is exactly what must not
  // happen here: the TV has to keep its own resolution and refresh rate, and
  // an exclusive-fullscreen transition on the target would fight the game on
  // the source monitor. Blocking Alt+Enter prevents DXGI doing it behind us.
  factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);

  swapChainFlags_ = flags;
  backBufferWidth_ = width;
  backBufferHeight_ = height;

  ComPtr<IDXGISwapChain3> swapChain3;
  if (SUCCEEDED(swapChain_.As(&swapChain3))) {
    UINT support = 0;
    if (SUCCEEDED(swapChain3->CheckColorSpaceSupport(plan.colorSpace, &support)) &&
        (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT)) {
      const HRESULT csHr = swapChain3->SetColorSpace1(plan.colorSpace);
      if (FAILED(csHr)) {
        DM_WARN(L"SetColorSpace1(%s) failed: %s", ColorSpaceName(plan.colorSpace),
                HrToString(csHr).c_str());
        return false;
      }
    } else if (plan.colorSpace != DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709) {
      DM_WARN(L"Target does not accept %s; falling back.", ColorSpaceName(plan.colorSpace));
      return false;
    }
  }

  if (options_.presentMode == PresentMode::VSyncWaitable) {
    ComPtr<IDXGISwapChain2> swapChain2;
    if (SUCCEEDED(swapChain_.As(&swapChain2))) {
      // One frame of latency: present the frame we just drew, do not queue.
      swapChain2->SetMaximumFrameLatency(1);
      frameLatencyWaitable_ = swapChain2->GetFrameLatencyWaitableObject();
    }
    if (!frameLatencyWaitable_) {
      DM_WARN(L"Waitable swap chain unavailable; pacing on Present(1, 0) alone.");
    }
  }

  return CreateBackBufferView();
}

bool Renderer::CreateBackBufferView() {
  ComPtr<ID3D11Texture2D> backBuffer;
  HRESULT hr = swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
  if (FAILED(hr)) {
    DM_ERROR(L"Getting the back buffer failed: %s", HrToString(hr).c_str());
    return false;
  }
  hr = device_->CreateRenderTargetView(backBuffer.Get(), nullptr, &backBufferRtv_);
  if (FAILED(hr)) {
    DM_ERROR(L"CreateRenderTargetView failed: %s", HrToString(hr).c_str());
    return false;
  }
  return true;
}

bool Renderer::ConfigureForSource(DXGI_FORMAT captureFormat, const DisplayInfo& source,
                                  const DisplayInfo& target) {
  ColorPlan plan = DecideColorPlan(captureFormat, source, target);

  // Access-lost recovery runs this on every game alt-tab. If the capture format
  // and the resulting plan are unchanged, keep the swap chain we already have.
  if (swapChain_ && backBufferRtv_ && captureFormat == configuredCaptureFormat_ &&
      plan.swapChainFormat == plan_.swapChainFormat &&
      plan.colorSpace == plan_.colorSpace &&
      plan.toneMapHdrToSdr == plan_.toneMapHdrToSdr) {
    plan_ = plan;  // Luminance values may have drifted; the pipeline has not.
    return true;
  }

  if (!CreateSwapChain(plan)) {
    // The only planned failure is a target that will not take the wide-gamut
    // colour space. Retry once as plain SDR rather than giving up or, worse,
    // presenting HDR values into an SDR swap chain.
    if (plan.swapChainFormat == DXGI_FORMAT_B8G8R8A8_UNORM &&
        plan.colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709) {
      return false;
    }
    DM_WARN(L"Falling back to an SDR swap chain for the target.");
    plan.swapChainFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    plan.colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    plan.toneMapHdrToSdr = (captureFormat == DXGI_FORMAT_R16G16B16A16_FLOAT);
    plan.description = plan.toneMapHdrToSdr
                           ? L"HDR source, SDR fallback swap chain: approximate tone mapping"
                           : L"SDR pass-through (fallback)";
    if (!CreateSwapChain(plan)) return false;
  }

  plan_ = plan;
  configuredCaptureFormat_ = captureFormat;
  DM_INFO(L"Output configured: %ux%u, %s, %s", backBufferWidth_, backBufferHeight_,
          FormatName(plan_.swapChainFormat), ColorSpaceName(plan_.colorSpace));
  DM_INFO(L"Colour handling: %s", plan_.description.c_str());
  if (plan_.toneMapHdrToSdr) {
    DM_WARN(L"HDR is running in limited mode. Colours are tone mapped, not exact. "
            L"Enable HDR on the target display for a true pass-through.");
  }
  DM_INFO(L"Present mode: %s", options_.presentMode == PresentMode::AllowTearing
                                   ? L"Present(0) with ALLOW_TEARING"
                                   : L"Present(1) on a waitable swap chain, max latency 1");
  return true;
}

bool Renderer::UpdateTargetGeometry(const DisplayInfo& target) {
  if (!hwnd_ || !swapChain_) return false;

  const RECT& r = target.desktopCoordinates;
  const UINT width = static_cast<UINT>(r.right - r.left);
  const UINT height = static_cast<UINT>(r.bottom - r.top);

  SetWindowPos(hwnd_, HWND_TOPMOST, r.left, r.top, static_cast<int>(width),
               static_cast<int>(height), SWP_NOACTIVATE | SWP_SHOWWINDOW);

  if (width == backBufferWidth_ && height == backBufferHeight_) return true;

  DM_INFO(L"Target geometry changed to %ux%u at %d,%d; resizing buffers.", width, height,
          static_cast<int>(r.left), static_cast<int>(r.top));

  context_->OMSetRenderTargets(0, nullptr, nullptr);
  backBufferRtv_.Reset();

  // ResizeBuffers must be given the same flags the swap chain was created with.
  const HRESULT hr = swapChain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN,
                                               swapChainFlags_);
  if (FAILED(hr)) {
    DM_ERROR(L"ResizeBuffers failed: %s", HrToString(hr).c_str());
    return false;
  }

  backBufferWidth_ = width;
  backBufferHeight_ = height;
  return CreateBackBufferView();
}

bool Renderer::WaitForPresentReady() {
  if (!frameLatencyWaitable_) return true;

  // The frame latency waitable object is a *semaphore*, not an event: it is
  // created with one token per allowed frame in flight and Present releases a
  // token back when the frame retires. Waiting takes a token.
  //
  // So a wait that is not followed by a Present spends a token that nothing
  // gives back, and the next wait finds the semaphore empty and blocks until
  // the cap below. That is exactly what happens on the frame path: the caller
  // waits here first and only then asks Desktop Duplication for a frame, and
  // any acquire that comes back empty - a timeout, an access loss, a
  // pointer-only update - leaves without presenting.
  //
  // Holding the token across calls instead fixes it. Once taken it stays taken
  // until RenderAndPresent actually spends it, so a caller may wait, find
  // nothing worth showing, and come back later without paying twice.
  if (presentCredit_) return true;

  // A one-second cap: if the swap chain never signals, something is wrong and
  // the caller should get a chance to re-check the display topology.
  const DWORD result = WaitForSingleObjectEx(frameLatencyWaitable_, 1000, TRUE);
  if (result == WAIT_OBJECT_0) {
    presentCredit_ = true;
    return true;
  }
  // A timed-out or alerted wait takes no token, so nothing is held here.
  return result == WAIT_TIMEOUT || result == WAIT_IO_COMPLETION;
}

bool Renderer::UploadCursor(const CursorImage& image) {
  cursorSrv_.Reset();
  cursorTexture_.Reset();
  cursorWidth_ = cursorHeight_ = 0;

  if (image.width == 0 || image.height == 0 || image.pixels.empty()) return false;

  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = image.width;
  desc.Height = image.height;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_IMMUTABLE;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

  D3D11_SUBRESOURCE_DATA data = {};
  data.pSysMem = image.pixels.data();
  data.SysMemPitch = image.width * 4;

  HRESULT hr = device_->CreateTexture2D(&desc, &data, &cursorTexture_);
  if (SUCCEEDED(hr)) {
    hr = device_->CreateShaderResourceView(cursorTexture_.Get(), nullptr, &cursorSrv_);
  }
  if (FAILED(hr)) {
    DM_WARN(L"Uploading the cursor bitmap failed: %s", HrToString(hr).c_str());
    cursorSrv_.Reset();
    cursorTexture_.Reset();
    return false;
  }

  cursorWidth_ = image.width;
  cursorHeight_ = image.height;
  cursorHotspot_ = image.hotspot;
  return true;
}

// Aspect-fit: the source keeps its shape, the unused strips stay black.
void Renderer::ComputeLetterbox(UINT sourceWidth, UINT sourceHeight,
                                D3D11_VIEWPORT* viewport) const {
  const float dstW = static_cast<float>(backBufferWidth_);
  const float dstH = static_cast<float>(backBufferHeight_);
  const float srcW = static_cast<float>(std::max<UINT>(1, sourceWidth));
  const float srcH = static_cast<float>(std::max<UINT>(1, sourceHeight));

  const float scale = std::min(dstW / srcW, dstH / srcH);
  const float w = srcW * scale;
  const float h = srcH * scale;

  viewport->TopLeftX = (dstW - w) * 0.5f;
  viewport->TopLeftY = (dstH - h) * 0.5f;
  viewport->Width = w;
  viewport->Height = h;
  viewport->MinDepth = 0.0f;
  viewport->MaxDepth = 1.0f;
}

bool Renderer::RenderAndPresent(const DuplicationCapture& capture, HRESULT* hr) {
  *hr = S_OK;
  if (!swapChain_ || !backBufferRtv_) return false;

  // The duplication surface is un-rotated; the upright picture we present has
  // its dimensions swapped for a portrait mode.
  const bool swapAxes = capture.Rotation() == DXGI_MODE_ROTATION_ROTATE90 ||
                        capture.Rotation() == DXGI_MODE_ROTATION_ROTATE270;
  const UINT logicalWidth = swapAxes ? capture.SourceHeight() : capture.SourceWidth();
  const UINT logicalHeight = swapAxes ? capture.SourceWidth() : capture.SourceHeight();

  UINT rotationMode = 0;
  switch (capture.Rotation()) {
    case DXGI_MODE_ROTATION_ROTATE90: rotationMode = 1; break;
    case DXGI_MODE_ROTATION_ROTATE180: rotationMode = 2; break;
    case DXGI_MODE_ROTATION_ROTATE270: rotationMode = 3; break;
    default: rotationMode = 0; break;
  }

  ID3D11RenderTargetView* rtv = backBufferRtv_.Get();
  context_->OMSetRenderTargets(1, &rtv, nullptr);

  // Clearing the whole target is what produces the black bars; the scaling
  // pass then only touches the letterboxed rectangle.
  const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  context_->ClearRenderTargetView(rtv, black);

  D3D11_VIEWPORT viewport = {};
  ComputeLetterbox(logicalWidth, logicalHeight, &viewport);
  context_->RSSetViewports(1, &viewport);
  context_->RSSetState(rasterizer_.Get());

  D3D11_MAPPED_SUBRESOURCE mapped = {};
  if (SUCCEEDED(context_->Map(mirrorCb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
    auto* constants = static_cast<MirrorConstants*>(mapped.pData);
    constants->rotation = rotationMode;
    constants->toneMap = plan_.toneMapHdrToSdr ? 1u : 0u;
    constants->sdrWhiteNits = plan_.sdrWhiteNits;
    constants->maxLumNits = plan_.maxLuminanceNits;
    context_->Unmap(mirrorCb_.Get(), 0);
  }

  ID3D11ShaderResourceView* srv = capture.DesktopSrv();
  ID3D11SamplerState* sampler = sampler_.Get();
  ID3D11Buffer* mirrorCb = mirrorCb_.Get();

  const float blendFactor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  context_->IASetInputLayout(nullptr);
  context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  context_->VSSetShader(mirrorVs_.Get(), nullptr, 0);
  context_->PSSetShader(mirrorPs_.Get(), nullptr, 0);
  context_->PSSetConstantBuffers(0, 1, &mirrorCb);
  context_->PSSetShaderResources(0, 1, &srv);
  context_->PSSetSamplers(0, 1, &sampler);
  context_->OMSetBlendState(opaqueBlend_.Get(), blendFactor, 0xFFFFFFFF);
  context_->Draw(3, 0);  // Full-screen triangle.

  // Cursor pass. Desktop Duplication reports the pointer separately from the
  // desktop image, so it has to be composited here.
  if (options_.drawCursor && capture.CursorVisible() && capture.HasCursorShape()) {
    if (cursorDirty_ || !cursorSrv_) {
      UploadCursor(capture.CursorShape());
      cursorDirty_ = false;
    }

    if (cursorSrv_ && cursorWidth_ > 0 && cursorHeight_ > 0) {
      const POINT position = capture.CursorPosition();
      const float texW = static_cast<float>(std::max<UINT>(1, capture.SourceWidth()));
      const float texH = static_cast<float>(std::max<UINT>(1, capture.SourceHeight()));

      // Corners in the un-rotated texture's UV space...
      const float u0 = static_cast<float>(position.x) / texW;
      const float v0 = static_cast<float>(position.y) / texH;
      const float u1 = static_cast<float>(position.x + static_cast<LONG>(cursorWidth_)) / texW;
      const float v1 = static_cast<float>(position.y + static_cast<LONG>(cursorHeight_)) / texH;

      // ...mapped back into the upright picture with the inverse of the
      // rotation the pixel shader applies.
      auto toLogical = [rotationMode](float u, float v, float* outU, float* outV) {
        switch (rotationMode) {
          case 1: *outU = 1.0f - v; *outV = u; break;
          case 2: *outU = 1.0f - u; *outV = 1.0f - v; break;
          case 3: *outU = v; *outV = 1.0f - u; break;
          default: *outU = u; *outV = v; break;
        }
      };

      float a0 = 0.0f, b0 = 0.0f, a1 = 0.0f, b1 = 0.0f;
      toLogical(u0, v0, &a0, &b0);
      toLogical(u1, v1, &a1, &b1);

      const float minU = std::min(a0, a1);
      const float maxU = std::max(a0, a1);
      const float minV = std::min(b0, b1);
      const float maxV = std::max(b0, b1);

      if (SUCCEEDED(context_->Map(cursorCb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        auto* constants = static_cast<CursorConstants*>(mapped.pData);
        // UV [0,1] to the viewport's NDC; Y is flipped.
        constants->rect[0] = minU * 2.0f - 1.0f;
        constants->rect[1] = 1.0f - minV * 2.0f;
        constants->rect[2] = maxU * 2.0f - 1.0f;
        constants->rect[3] = 1.0f - maxV * 2.0f;
        context_->Unmap(cursorCb_.Get(), 0);
      }

      ID3D11ShaderResourceView* cursorSrv = cursorSrv_.Get();
      ID3D11Buffer* cursorCb = cursorCb_.Get();
      context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
      context_->VSSetShader(cursorVs_.Get(), nullptr, 0);
      context_->VSSetConstantBuffers(1, 1, &cursorCb);
      context_->PSSetShader(cursorPs_.Get(), nullptr, 0);
      context_->PSSetShaderResources(0, 1, &cursorSrv);
      context_->OMSetBlendState(alphaBlend_.Get(), blendFactor, 0xFFFFFFFF);
      context_->Draw(4, 0);
    }
  }

  // Unbind the source SRV so the next CopyResource into that texture is not
  // blocked by it still being bound as a shader resource.
  ID3D11ShaderResourceView* nullSrv = nullptr;
  context_->PSSetShaderResources(0, 1, &nullSrv);

  if (options_.presentMode == PresentMode::AllowTearing) {
    // ALLOW_TEARING is only legal with sync interval 0.
    *hr = swapChain_->Present(0, DXGI_PRESENT_ALLOW_TEARING);
  } else {
    *hr = swapChain_->Present(1, 0);
  }
  // The token taken in WaitForPresentReady is spent now, whatever Present
  // returned: a failed Present is followed by a rebuild, not by another wait.
  presentCredit_ = false;

  if (*hr == DXGI_STATUS_OCCLUDED) {
    if (!occluded_) {
      DM_INFO(L"Output window is occluded; DXGI is throttling presentation.");
      occluded_ = true;
    }
    return true;
  }
  if (occluded_) {
    DM_INFO(L"Output window is visible again.");
    occluded_ = false;
  }

  return SUCCEEDED(*hr);
}

}  // namespace dm
