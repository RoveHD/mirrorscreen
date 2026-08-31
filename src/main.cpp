// DisplayMirror - entry point and configuration window.
//
// A single thread runs everything: the config window's message pump and, while
// mirroring, the capture/present loop. The loop blocks in AcquireNextFrame and
// on the swap chain's waitable object, so an idle desktop costs almost no CPU.
#include <windows.h>

#include <commctrl.h>
#include <wtsapi32.h>

#include <string>
#include <vector>

#include "displays.h"
#include "log.h"
#include "mirror.h"
#include "renderer.h"

namespace dm {
namespace {

constexpr wchar_t kConfigClass[] = L"DisplayMirrorConfigWindow";

enum ControlId : int {
  kIdSourceLabel = 100,
  kIdSourceCombo,
  kIdTargetLabel,
  kIdTargetCombo,
  kIdCursorCheck,
  kIdTearingCheck,
  kIdStartButton,
  kIdRefreshButton,
  kIdLogBox,
};

// Ctrl+Alt+M toggles mirroring from anywhere, including from inside a game.
constexpr int kHotkeyToggle = 1;
constexpr UINT kHotkeyModifiers = MOD_CONTROL | MOD_ALT;
constexpr UINT kHotkeyVk = 'M';

class ConfigWindow {
 public:
  bool Create();
  int Run();

  // Routes log lines into the window's log pane. Installed before the window
  // exists, so it has to tolerate a null g_window.
  static void LogSinkThunk(LogLevel level, const std::wstring& line);

 private:
  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
  LRESULT Handle(HWND, UINT, WPARAM, LPARAM);

  void CreateControls(HWND parent);
  void RefreshDisplays();
  void ToggleMirroring();
  void StartMirroring();
  void StopMirroring();
  void UpdateStartButton();
  void AppendLog(const std::wstring& line);

  HWND hwnd_ = nullptr;
  HWND sourceCombo_ = nullptr;
  HWND targetCombo_ = nullptr;
  HWND cursorCheck_ = nullptr;
  HWND tearingCheck_ = nullptr;
  HWND startButton_ = nullptr;
  HWND logBox_ = nullptr;
  HFONT font_ = nullptr;

  std::vector<DisplayInfo> displays_;
  MirrorSession session_;
  bool tearingSupported_ = false;
  bool hotkeyRegistered_ = false;
};

ConfigWindow* g_window = nullptr;

void ConfigWindow::LogSinkThunk(LogLevel, const std::wstring& line) {
  if (g_window) g_window->AppendLog(line);
}

void ConfigWindow::AppendLog(const std::wstring& line) {
  if (!logBox_) return;

  // Keep the box bounded: trim the oldest third once it grows past ~32k chars.
  const int length = GetWindowTextLengthW(logBox_);
  if (length > 32000) {
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(logBox_, text.data(), length + 1);
    text.resize(static_cast<size_t>(length));
    const size_t cut = text.find(L'\n', text.size() / 3);
    text = (cut == std::wstring::npos) ? std::wstring() : text.substr(cut + 1);
    SetWindowTextW(logBox_, text.c_str());
  }

  const std::wstring appended = line + L"\r\n";
  const int end = GetWindowTextLengthW(logBox_);
  SendMessageW(logBox_, EM_SETSEL, end, end);
  SendMessageW(logBox_, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(appended.c_str()));
  SendMessageW(logBox_, EM_SCROLLCARET, 0, 0);
}

void ConfigWindow::CreateControls(HWND parent) {
  font_ = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                      DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

  auto add = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w,
                 int h, int id) -> HWND {
    HWND control = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h,
                                   parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                   GetModuleHandleW(nullptr), nullptr);
    if (control && font_) {
      SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    }
    return control;
  };

  add(L"STATIC", L"Source display (captured):", 0, 12, 12, 200, 18, kIdSourceLabel);
  sourceCombo_ = add(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 12, 32, 640, 300,
                     kIdSourceCombo);

  add(L"STATIC", L"Target display (mirrored to):", 0, 12, 68, 200, 18, kIdTargetLabel);
  targetCombo_ = add(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 12, 88, 640, 300,
                     kIdTargetCombo);

  cursorCheck_ = add(L"BUTTON", L"Mirror the mouse cursor", BS_AUTOCHECKBOX, 12, 126, 200,
                     22, kIdCursorCheck);
  SendMessageW(cursorCheck_, BM_SETCHECK, BST_CHECKED, 0);

  tearingCheck_ = add(L"BUTTON", L"Allow tearing (lowest latency, may tear)",
                      BS_AUTOCHECKBOX, 220, 126, 280, 22, kIdTearingCheck);
  EnableWindow(tearingCheck_, tearingSupported_);

  startButton_ = add(L"BUTTON", L"Start mirroring", BS_DEFPUSHBUTTON, 12, 158, 160, 30,
                     kIdStartButton);
  add(L"BUTTON", L"Refresh displays", 0, 180, 158, 140, 30, kIdRefreshButton);
  add(L"STATIC", L"Ctrl+Alt+M toggles · ESC on the output window stops", 0, 332, 165,
      320, 20, 0);

  logBox_ = add(L"EDIT", L"",
                ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_BORDER, 12,
                198, 640, 200, kIdLogBox);
}

void ConfigWindow::RefreshDisplays() {
  displays_ = EnumerateDisplays();
  LogDisplays(displays_);

  const int previousSource = static_cast<int>(SendMessageW(sourceCombo_, CB_GETCURSEL, 0, 0));
  const int previousTarget = static_cast<int>(SendMessageW(targetCombo_, CB_GETCURSEL, 0, 0));

  SendMessageW(sourceCombo_, CB_RESETCONTENT, 0, 0);
  SendMessageW(targetCombo_, CB_RESETCONTENT, 0, 0);

  for (const DisplayInfo& display : displays_) {
    const std::wstring text = display.Describe();
    SendMessageW(sourceCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
    SendMessageW(targetCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
  }

  if (displays_.empty()) {
    UpdateStartButton();
    return;
  }

  // Sensible defaults: capture the primary display, mirror to the first display
  // that is not it.
  int source = previousSource;
  if (source < 0 || source >= static_cast<int>(displays_.size())) {
    source = 0;
    for (size_t i = 0; i < displays_.size(); ++i) {
      if (displays_[i].primary) {
        source = static_cast<int>(i);
        break;
      }
    }
  }

  int target = previousTarget;
  if (target < 0 || target >= static_cast<int>(displays_.size()) || target == source) {
    target = (source + 1) % static_cast<int>(displays_.size());
  }

  SendMessageW(sourceCombo_, CB_SETCURSEL, source, 0);
  SendMessageW(targetCombo_, CB_SETCURSEL, target, 0);
  UpdateStartButton();
}

void ConfigWindow::UpdateStartButton() {
  const int source = static_cast<int>(SendMessageW(sourceCombo_, CB_GETCURSEL, 0, 0));
  const int target = static_cast<int>(SendMessageW(targetCombo_, CB_GETCURSEL, 0, 0));

  SetWindowTextW(startButton_,
                 session_.IsRunning() ? L"Stop mirroring" : L"Start mirroring");

  const bool selectable = source >= 0 && target >= 0 && source != target &&
                          displays_.size() >= 2;
  EnableWindow(startButton_, session_.IsRunning() || selectable);
  EnableWindow(sourceCombo_, !session_.IsRunning());
  EnableWindow(targetCombo_, !session_.IsRunning());

  // Mirroring across two GPUs works, but the frame is composited by DWM on its
  // way to the other adapter, so it can never reach independent flip. Tearing
  // mode would only remove the pacing, not the vblank; grey it out and say why.
  bool sameAdapter = true;
  if (selectable) {
    const LUID& a = displays_[static_cast<size_t>(source)].adapterLuid;
    const LUID& b = displays_[static_cast<size_t>(target)].adapterLuid;
    sameAdapter = a.LowPart == b.LowPart && a.HighPart == b.HighPart;
  }
  EnableWindow(tearingCheck_, tearingSupported_ && sameAdapter && !session_.IsRunning());
  SetWindowTextW(tearingCheck_, sameAdapter
                                    ? L"Allow tearing (lowest latency, may tear)"
                                    : L"Allow tearing (not available across GPUs)");
}

void ConfigWindow::StartMirroring() {
  const int source = static_cast<int>(SendMessageW(sourceCombo_, CB_GETCURSEL, 0, 0));
  const int target = static_cast<int>(SendMessageW(targetCombo_, CB_GETCURSEL, 0, 0));

  if (source < 0 || target < 0 || source >= static_cast<int>(displays_.size()) ||
      target >= static_cast<int>(displays_.size())) {
    MessageBoxW(hwnd_, L"Select a source and a target display first.", L"DisplayMirror",
                MB_OK | MB_ICONINFORMATION);
    return;
  }
  if (source == target) {
    MessageBoxW(hwnd_, L"Source and target must be different displays.", L"DisplayMirror",
                MB_OK | MB_ICONWARNING);
    return;
  }

  MirrorConfig config;
  config.source = displays_[static_cast<size_t>(source)];
  config.target = displays_[static_cast<size_t>(target)];
  config.drawCursor = SendMessageW(cursorCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
  config.presentMode =
      (tearingSupported_ && SendMessageW(tearingCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED)
          ? PresentMode::AllowTearing
          : PresentMode::VSyncWaitable;

  StartError error = StartError::None;
  std::wstring message;
  if (!session_.Start(config, &error, &message)) {
    MessageBoxW(hwnd_, message.c_str(), L"DisplayMirror", MB_OK | MB_ICONERROR);
    UpdateStartButton();
    return;
  }

  UpdateStartButton();
  // Get out of the way, but stay reachable in the taskbar.
  ShowWindow(hwnd_, SW_MINIMIZE);
}

void ConfigWindow::StopMirroring() {
  session_.Stop();
  UpdateStartButton();
  ShowWindow(hwnd_, SW_RESTORE);
  SetForegroundWindow(hwnd_);
}

void ConfigWindow::ToggleMirroring() {
  if (session_.IsRunning()) {
    StopMirroring();
  } else {
    StartMirroring();
  }
}

LRESULT CALLBACK ConfigWindow::WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  auto* self = reinterpret_cast<ConfigWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (msg == WM_NCCREATE) {
    auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    self = static_cast<ConfigWindow*>(create->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    self->hwnd_ = hwnd;
  }
  if (self) return self->Handle(hwnd, msg, wparam, lparam);
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

LRESULT ConfigWindow::Handle(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  switch (msg) {
    case WM_CREATE:
      CreateControls(hwnd);
      RefreshDisplays();
      return 0;

    case WM_COMMAND:
      switch (LOWORD(wparam)) {
        case kIdStartButton:
          ToggleMirroring();
          return 0;
        case kIdRefreshButton:
          if (!session_.IsRunning()) RefreshDisplays();
          return 0;
        case kIdCursorCheck:
          if (session_.IsRunning()) {
            session_.SetDrawCursor(
                SendMessageW(cursorCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED);
          }
          return 0;
        case kIdSourceCombo:
        case kIdTargetCombo:
          if (HIWORD(wparam) == CBN_SELCHANGE) UpdateStartButton();
          return 0;
        default:
          break;
      }
      break;

    case WM_HOTKEY:
      if (wparam == kHotkeyToggle) {
        ToggleMirroring();
        return 0;
      }
      break;

    case WM_DISPLAYCHANGE:
      // Fired for resolution changes, monitors appearing and disappearing, and
      // for the TV being switched off and on again.
      if (session_.IsRunning()) {
        session_.OnDisplayChange();
      } else {
        RefreshDisplays();
      }
      return 0;

    case WM_POWERBROADCAST:
      if (wparam == PBT_APMRESUMEAUTOMATIC || wparam == PBT_APMRESUMESUSPEND) {
        // After wake the whole graphics stack may have been re-created.
        DM_INFO(L"System resumed from sleep.");
        if (session_.IsRunning()) session_.OnDisplayChange();
      }
      return TRUE;

    case WM_WTSSESSION_CHANGE:
      // Lock/unlock and fast user switching invalidate duplication; the session
      // recovers on its own, this just explains the gap in the log.
      if (wparam == WTS_SESSION_LOCK) DM_INFO(L"Session locked.");
      if (wparam == WTS_SESSION_UNLOCK) DM_INFO(L"Session unlocked.");
      return 0;

    case WM_CLOSE:
      DestroyWindow(hwnd);
      return 0;

    case WM_DESTROY:
      session_.Stop();
      WTSUnRegisterSessionNotification(hwnd);
      if (hotkeyRegistered_) UnregisterHotKey(hwnd, kHotkeyToggle);
      if (font_) {
        DeleteObject(font_);
        font_ = nullptr;
      }
      PostQuitMessage(0);
      return 0;

    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

bool ConfigWindow::Create() {
  tearingSupported_ = Renderer::SystemSupportsTearing();

  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = &ConfigWindow::WndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
  wc.lpszClassName = kConfigClass;
  wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
  if (!RegisterClassExW(&wc)) return false;

  RECT rect = {0, 0, 668, 412};
  AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME, FALSE);

  hwnd_ = CreateWindowExW(0, kConfigClass, L"DisplayMirror", 
                          (WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX),
                          CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left,
                          rect.bottom - rect.top, nullptr, nullptr,
                          GetModuleHandleW(nullptr), this);
  if (!hwnd_) return false;

  // Without this subscription WM_WTSSESSION_CHANGE is never delivered.
  WTSRegisterSessionNotification(hwnd_, NOTIFY_FOR_THIS_SESSION);

  hotkeyRegistered_ = RegisterHotKey(hwnd_, kHotkeyToggle, kHotkeyModifiers, kHotkeyVk) != FALSE;
  if (!hotkeyRegistered_) {
    DM_WARN(L"Could not register the Ctrl+Alt+M hotkey; another application owns it.");
  }

  DM_INFO(L"Tearing support: %s", tearingSupported_ ? L"yes" : L"no");

  ShowWindow(hwnd_, SW_SHOW);
  UpdateWindow(hwnd_);
  return true;
}

int ConfigWindow::Run() {
  MSG msg = {};
  for (;;) {
    if (session_.IsRunning()) {
      // Drain pending messages without blocking, then do one frame. The frame
      // itself blocks (on the swap chain and on AcquireNextFrame), which is
      // what keeps the loop off the CPU without a sleep.
      while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) return static_cast<int>(msg.wParam);
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }
      if (!session_.IsRunning()) continue;

      if (session_.EscapeRequested()) {
        DM_INFO(L"ESC pressed on the output window.");
        StopMirroring();
        continue;
      }

      if (!session_.Tick()) {
        UpdateStartButton();
        ShowWindow(hwnd_, SW_RESTORE);
      }
    } else {
      // Idle: block in GetMessage so the process uses no CPU at all.
      const BOOL result = GetMessageW(&msg, nullptr, 0, 0);
      if (result == 0) return static_cast<int>(msg.wParam);
      if (result == -1) return 1;
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
}

}  // namespace
}  // namespace dm

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
  // Per-monitor DPI awareness matters here: without it Windows would hand us
  // virtualised display coordinates and the output window would not line up
  // with the target monitor's real pixels.
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

  INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_STANDARD_CLASSES};
  InitCommonControlsEx(&icc);

  dm::ConfigWindow window;
  dm::g_window = &window;
  dm::LogInit(&dm::ConfigWindow::LogSinkThunk);
  DM_INFO(L"DisplayMirror starting.");

  int exitCode = 1;
  if (window.Create()) {
    exitCode = window.Run();
  } else {
    MessageBoxW(nullptr, L"The DisplayMirror window could not be created.",
                L"DisplayMirror", MB_OK | MB_ICONERROR);
  }

  DM_INFO(L"DisplayMirror exiting.");
  dm::g_window = nullptr;
  dm::LogShutdown();
  return exitCode;
}
