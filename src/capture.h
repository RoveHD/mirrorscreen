// DisplayMirror - Desktop Duplication capture of one whole physical output.
//
// We always duplicate an entire output. No window capture, no process lookup,
// no hooking: that is what makes this work across D3D9/11/12, Vulkan and
// OpenGL, and what keeps it clear of anti-cheat.
#pragma once

#include <d3d11.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <wrl/client.h>

#include <cstdint>
#include <vector>

namespace dm {

// Decoded cursor bitmap, ready to be uploaded as a B8G8R8A8 texture.
struct CursorImage {
  std::vector<uint8_t> pixels;  // BGRA, tightly packed
  UINT width = 0;
  UINT height = 0;
  POINT hotspot = {};
};

class DuplicationCapture {
 public:
  enum class Status {
    Frame,     // A new desktop image and/or a pointer update is available.
    Timeout,   // No new frame within the timeout. Not an error.
    Lost,      // DXGI_ERROR_ACCESS_LOST and friends: recreate the duplication.
    Fatal,     // Device removed/reset: the whole D3D device must be rebuilt.
  };

  struct FrameResult {
    Status status = Status::Timeout;
    bool desktopUpdated = false;  // The shared texture holds new content.
    bool pointerMoved = false;
    bool pointerShapeChanged = false;
  };

  ~DuplicationCapture();

  // `device` must belong to the adapter that owns `output`.
  bool Start(ID3D11Device* device, IDXGIOutput* output);
  void Stop();
  bool IsRunning() const { return duplication_ != nullptr; }

  // Blocks for at most `timeoutMs`; never busy-waits. The desktop image is
  // copied into an internally owned texture and the DXGI frame is released
  // immediately, so we never hold the duplication surface across a Present.
  FrameResult AcquireFrame(UINT timeoutMs);

  // Valid after a successful Start().
  ID3D11ShaderResourceView* DesktopSrv() const { return desktopSrv_.Get(); }
  UINT SourceWidth() const { return duplDesc_.ModeDesc.Width; }
  UINT SourceHeight() const { return duplDesc_.ModeDesc.Height; }
  DXGI_FORMAT SourceFormat() const { return duplDesc_.ModeDesc.Format; }
  DXGI_MODE_ROTATION Rotation() const { return duplDesc_.Rotation; }
  bool UsedDuplicateOutput1() const { return usedDuplicateOutput1_; }

  // Cursor state. `CursorVisible()` follows the OS: a game that hides the
  // cursor produces false here and nothing is drawn.
  bool CursorVisible() const { return pointerVisible_; }
  POINT CursorPosition() const { return pointerPosition_; }
  const CursorImage& CursorShape() const { return cursorImage_; }
  bool HasCursorShape() const { return cursorImage_.width > 0 && cursorImage_.height > 0; }

 private:
  bool CreateDesktopTexture();
  bool UpdatePointerShape(UINT requiredSize);

  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
  Microsoft::WRL::ComPtr<IDXGIOutputDuplication> duplication_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> desktopTexture_;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> desktopSrv_;

  DXGI_OUTDUPL_DESC duplDesc_ = {};
  bool usedDuplicateOutput1_ = false;

  std::vector<uint8_t> shapeBuffer_;
  CursorImage cursorImage_;
  POINT pointerPosition_ = {};
  bool pointerVisible_ = false;
  bool firstFrameCopied_ = false;
};

}  // namespace dm
