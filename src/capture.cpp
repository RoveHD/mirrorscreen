#include "capture.h"

#include <algorithm>

#include "log.h"

using Microsoft::WRL::ComPtr;

namespace dm {
namespace {

// Formats we are willing to receive from a full-screen application, in the
// order we prefer them. DuplicateOutput1 hands back the application's own
// scan-out format instead of forcing a conversion to 32-bit BGRA, which keeps
// 10-bit and HDR content intact. B8G8R8A8_UNORM must be in the list because it
// is the ordinary desktop format.
constexpr DXGI_FORMAT kSupportedFormats[] = {
    DXGI_FORMAT_B8G8R8A8_UNORM,
    DXGI_FORMAT_R10G10B10A2_UNORM,
    DXGI_FORMAT_R16G16B16A16_FLOAT,
};

// Distinguishes "the device itself is gone, rebuild everything" from the much
// more common "this duplication object is dead, build a new one" (access lost,
// session disconnected, a mode change), which is the caller's default.
bool IsDeviceFatal(HRESULT hr) {
  return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET ||
         hr == DXGI_ERROR_DEVICE_HUNG || hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
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

// Decodes a 1bpp AND mask followed by a 1bpp XOR mask into BGRA.
//
// The true semantics are per-pixel raster ops against the screen; we cannot do
// that in a single blend pass, so the inverting case (AND=1, XOR=1) is
// approximated as opaque white. That is the same approximation every
// duplication-based mirror makes and is invisible for normal cursors.
bool DecodeMonochrome(const uint8_t* data, size_t dataSize,
                      const DXGI_OUTDUPL_POINTER_SHAPE_INFO& info, CursorImage& out) {
  const UINT height = info.Height / 2;  // AND mask and XOR mask stacked.
  if (height == 0 || static_cast<size_t>(info.Pitch) * info.Height > dataSize) return false;
  out.width = info.Width;
  out.height = height;
  out.pixels.assign(static_cast<size_t>(info.Width) * height * 4, 0);

  const uint8_t* andMask = data;
  const uint8_t* xorMask = data + static_cast<size_t>(info.Pitch) * height;

  for (UINT y = 0; y < height; ++y) {
    for (UINT x = 0; x < info.Width; ++x) {
      const UINT byte = x / 8;
      const uint8_t bit = static_cast<uint8_t>(0x80 >> (x % 8));
      const bool andBit = (andMask[static_cast<size_t>(y) * info.Pitch + byte] & bit) != 0;
      const bool xorBit = (xorMask[static_cast<size_t>(y) * info.Pitch + byte] & bit) != 0;

      uint8_t* px = &out.pixels[(static_cast<size_t>(y) * info.Width + x) * 4];
      if (!andBit) {
        // Opaque: black when XOR is clear, white when it is set.
        const uint8_t value = xorBit ? 0xFF : 0x00;
        px[0] = px[1] = px[2] = value;
        px[3] = 0xFF;
      } else if (xorBit) {
        px[0] = px[1] = px[2] = 0xFF;  // Would be "invert screen".
        px[3] = 0xFF;
      }
      // AND=1, XOR=0 stays fully transparent.
    }
  }
  return true;
}

bool DecodeColor(const uint8_t* data, size_t dataSize,
                 const DXGI_OUTDUPL_POINTER_SHAPE_INFO& info, CursorImage& out) {
  if (static_cast<size_t>(info.Pitch) * (info.Height - 1) +
          static_cast<size_t>(info.Width) * 4 > dataSize) {
    return false;
  }
  out.width = info.Width;
  out.height = info.Height;
  out.pixels.assign(static_cast<size_t>(info.Width) * info.Height * 4, 0);
  for (UINT y = 0; y < info.Height; ++y) {
    memcpy(&out.pixels[static_cast<size_t>(y) * info.Width * 4],
           data + static_cast<size_t>(y) * info.Pitch,
           static_cast<size_t>(info.Width) * 4);
  }
  return true;
}

// Masked colour: alpha 0 means "replace the screen pixel", alpha 0xFF means
// "XOR with the screen pixel". No other alpha values are legal. We render both
// as opaque, which is exact for the common case and an approximation for the
// rare XOR cursors.
bool DecodeMaskedColor(const uint8_t* data, size_t dataSize,
                       const DXGI_OUTDUPL_POINTER_SHAPE_INFO& info, CursorImage& out) {
  if (!DecodeColor(data, dataSize, info, out)) return false;
  for (size_t i = 0; i < out.pixels.size(); i += 4) out.pixels[i + 3] = 0xFF;
  return true;
}

}  // namespace

DuplicationCapture::~DuplicationCapture() { Stop(); }

bool DuplicationCapture::Start(ID3D11Device* device, IDXGIOutput* output) {
  Stop();

  device_ = device;
  device_->GetImmediateContext(&context_);

  ComPtr<IDXGIOutput5> output5;
  HRESULT hr = output->QueryInterface(IID_PPV_ARGS(&output5));
  if (SUCCEEDED(hr)) {
    hr = output5->DuplicateOutput1(device, 0, _countof(kSupportedFormats),
                                   kSupportedFormats, &duplication_);
    if (SUCCEEDED(hr)) {
      usedDuplicateOutput1_ = true;
    } else {
      DM_WARN(L"DuplicateOutput1 failed (%s), falling back to DuplicateOutput",
              HrToString(hr).c_str());
    }
  }

  if (!duplication_) {
    ComPtr<IDXGIOutput1> output1;
    hr = output->QueryInterface(IID_PPV_ARGS(&output1));
    if (FAILED(hr)) {
      DM_ERROR(L"Output does not support IDXGIOutput1: %s", HrToString(hr).c_str());
      Stop();
      return false;
    }
    hr = output1->DuplicateOutput(device, &duplication_);
    usedDuplicateOutput1_ = false;
  }

  if (FAILED(hr) || !duplication_) {
    // These three are the ones a user can actually act on, so name them.
    if (hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE) {
      DM_ERROR(L"Desktop Duplication unavailable: the per-session limit of "
               L"concurrent duplications is reached. Close other capture tools.");
    } else if (hr == E_ACCESSDENIED) {
      DM_ERROR(L"Access denied duplicating the output. This happens on the secure "
               L"desktop (UAC prompt, lock screen); it resolves by itself.");
    } else if (hr == DXGI_ERROR_UNSUPPORTED) {
      DM_ERROR(L"Desktop Duplication is not supported for the current desktop mode.");
    } else {
      DM_ERROR(L"DuplicateOutput failed: %s", HrToString(hr).c_str());
    }
    Stop();
    return false;
  }

  duplication_->GetDesc(&duplDesc_);
  if (!CreateDesktopTexture()) {
    Stop();
    return false;
  }

  DM_INFO(L"Capture started: %ux%u, format %s, rotation %u, %s%s",
          duplDesc_.ModeDesc.Width, duplDesc_.ModeDesc.Height,
          FormatName(duplDesc_.ModeDesc.Format),
          static_cast<unsigned>(duplDesc_.Rotation),
          usedDuplicateOutput1_ ? L"DuplicateOutput1" : L"DuplicateOutput",
          duplDesc_.DesktopImageInSystemMemory ? L", desktop image in system memory" : L"");
  return true;
}

void DuplicationCapture::Stop() {
  if (duplication_) {
    // Best effort: if we still hold a frame, ReleaseFrame is required before
    // the object goes away. A duplicate release is harmless.
    duplication_->ReleaseFrame();
    duplication_.Reset();
  }
  desktopSrv_.Reset();
  desktopTexture_.Reset();
  context_.Reset();
  device_.Reset();
  duplDesc_ = {};
  pointerVisible_ = false;
  cursorImage_ = {};
  shapeBuffer_.clear();
  firstFrameCopied_ = false;
}

bool DuplicationCapture::CreateDesktopTexture() {
  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = duplDesc_.ModeDesc.Width;
  desc.Height = duplDesc_.ModeDesc.Height;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = duplDesc_.ModeDesc.Format;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

  HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &desktopTexture_);
  if (FAILED(hr)) {
    DM_ERROR(L"CreateTexture2D for the desktop copy failed: %s", HrToString(hr).c_str());
    return false;
  }

  hr = device_->CreateShaderResourceView(desktopTexture_.Get(), nullptr, &desktopSrv_);
  if (FAILED(hr)) {
    DM_ERROR(L"CreateShaderResourceView for the desktop copy failed: %s",
             HrToString(hr).c_str());
    return false;
  }
  return true;
}

DuplicationCapture::FrameResult DuplicationCapture::AcquireFrame(UINT timeoutMs) {
  FrameResult result;
  if (!duplication_) {
    result.status = Status::Lost;
    return result;
  }

  DXGI_OUTDUPL_FRAME_INFO info = {};
  ComPtr<IDXGIResource> resource;
  const HRESULT hr = duplication_->AcquireNextFrame(timeoutMs, &info, &resource);

  if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
    // Nothing changed on the source display. This is the normal idle path and
    // is where the loop spends its time when the desktop is static.
    result.status = Status::Timeout;
    return result;
  }
  if (FAILED(hr)) {
    result.status = IsDeviceFatal(hr) ? Status::Fatal : Status::Lost;
    DM_WARN(L"AcquireNextFrame failed: %s", HrToString(hr).c_str());
    return result;
  }

  // Everything below must reach ReleaseFrame, so keep it branch-light.
  //
  // LastPresentTime is zero when only the pointer moved. The one exception is
  // the very first acquire after a (re)start: the duplication hands us the
  // current desktop image and we must take it, or a static desktop would leave
  // the target black until something happens to move on the source.
  if (info.LastPresentTime.QuadPart != 0 || !firstFrameCopied_) {
    ComPtr<ID3D11Texture2D> acquired;
    if (SUCCEEDED(resource.As(&acquired))) {
      // Straight GPU-to-GPU copy. The frame is released immediately after, so
      // the duplication surface is never held across our Present.
      context_->CopyResource(desktopTexture_.Get(), acquired.Get());
      result.desktopUpdated = true;
      firstFrameCopied_ = true;
    }
  }

  if (info.LastMouseUpdateTime.QuadPart != 0) {
    pointerVisible_ = info.PointerPosition.Visible != 0;
    pointerPosition_ = info.PointerPosition.Position;
    result.pointerMoved = true;
  }

  if (info.PointerShapeBufferSize != 0) {
    if (UpdatePointerShape(info.PointerShapeBufferSize)) result.pointerShapeChanged = true;
  }

  const HRESULT releaseHr = duplication_->ReleaseFrame();
  if (FAILED(releaseHr)) {
    result.status = IsDeviceFatal(releaseHr) ? Status::Fatal : Status::Lost;
    DM_WARN(L"ReleaseFrame failed: %s", HrToString(releaseHr).c_str());
    return result;
  }

  result.status = Status::Frame;
  return result;
}

bool DuplicationCapture::UpdatePointerShape(UINT requiredSize) {
  if (shapeBuffer_.size() < requiredSize) shapeBuffer_.resize(requiredSize);

  UINT written = 0;
  DXGI_OUTDUPL_POINTER_SHAPE_INFO info = {};
  HRESULT hr = duplication_->GetFramePointerShape(
      static_cast<UINT>(shapeBuffer_.size()), shapeBuffer_.data(), &written, &info);
  if (hr == DXGI_ERROR_MORE_DATA) {
    shapeBuffer_.resize(written ? written : shapeBuffer_.size() * 2);
    hr = duplication_->GetFramePointerShape(static_cast<UINT>(shapeBuffer_.size()),
                                            shapeBuffer_.data(), &written, &info);
  }
  if (FAILED(hr)) {
    DM_WARN(L"GetFramePointerShape failed: %s", HrToString(hr).c_str());
    return false;
  }

  if (info.Width == 0 || info.Height == 0) return false;

  // `written` is the authoritative size of the shape data; the buffer itself
  // may be larger from a previous, bigger cursor.
  const size_t dataSize = (std::min)(static_cast<size_t>(written), shapeBuffer_.size());

  bool decoded = false;
  switch (info.Type) {
    case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME:
      decoded = DecodeMonochrome(shapeBuffer_.data(), dataSize, info, cursorImage_);
      break;
    case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR:
      decoded = DecodeColor(shapeBuffer_.data(), dataSize, info, cursorImage_);
      break;
    case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR:
      decoded = DecodeMaskedColor(shapeBuffer_.data(), dataSize, info, cursorImage_);
      break;
    default:
      DM_WARN(L"Unknown pointer shape type %u", info.Type);
      return false;
  }
  if (!decoded) {
    DM_WARN(L"Pointer shape data was smaller than its descriptor claims; ignoring.");
    return false;
  }

  cursorImage_.hotspot.x = static_cast<LONG>(info.HotSpot.x);
  cursorImage_.hotspot.y = static_cast<LONG>(info.HotSpot.y);
  return true;
}

}  // namespace dm
