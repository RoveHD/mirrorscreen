// DisplayMirror - entry point and configuration window.
//
// A single thread runs everything: the config window's message pump and, while
// mirroring, the capture/present loop. The loop blocks in AcquireNextFrame and
// on the swap chain's waitable object, so an idle desktop costs almost no CPU.
//
// The only work that happens off this thread is the update check, which is a
// blocking network call and posts its result back as a message.
#include <windows.h>

#include <commctrl.h>
#include <shellapi.h>
#include <wtsapi32.h>

#include <string>
#include <vector>

#include "displays.h"
#include "log.h"
#include "mirror.h"
#include "renderer.h"
#include "resource.h"
#include "settings.h"
#include "theme.h"
#include "updater.h"
#include "version.h"

namespace dm {
namespace {

constexpr wchar_t kConfigClass[] = L"DisplayMirrorConfigWindow";

enum ControlId : int {
  kIdSourceCombo = 100,
  kIdTargetCombo,
  kIdCursorCheck,
  kIdTearingCheck,
  kIdAutoRunCheck,
  kIdAutoMirrorCheck,
  kIdUpdateCheckBox,
  kIdStartButton,
  kIdRefreshButton,
  kIdUpdateButton,
  kIdLogBox,
};

// The tray icon's own message and the results posted back by the update
// thread. Kept clear of the control ids above.
constexpr UINT kTrayCallbackMessage = WM_APP + 1;
constexpr UINT kUpdateCheckedMessage = WM_APP + 2;
constexpr UINT kUpdateDownloadedMessage = WM_APP + 3;
constexpr UINT kTrayIconId = 1;

enum TrayCommand : int {
  kIdTrayShow = 200,
  kIdTrayToggle,
  kIdTrayExit,
  kIdTrayUpdate,
};

// Explorer re-broadcasts this after it restarts, and every tray icon has to be
// added again or it is gone until the next logon.
UINT g_taskbarCreatedMessage = 0;

// Ctrl+Alt+M toggles mirroring from anywhere, including from inside a game.
constexpr int kHotkeyToggle = 1;
constexpr UINT kHotkeyModifiers = MOD_CONTROL | MOD_ALT;
constexpr UINT kHotkeyVk = 'M';

// A day between automatic update checks. Often enough to matter, rare enough
// that GitHub's unauthenticated rate limit is never in sight.
constexpr ULONGLONG kUpdateCheckIntervalSeconds = 24 * 60 * 60;

ULONGLONG UnixNow() {
  FILETIME ft = {};
  GetSystemTimeAsFileTime(&ft);
  ULARGE_INTEGER value = {};
  value.LowPart = ft.dwLowDateTime;
  value.HighPart = ft.dwHighDateTime;
  // FILETIME counts 100ns ticks from 1601; 11644473600 seconds to 1970.
  return value.QuadPart / 10000000ULL - 11644473600ULL;
}

// Hover state for the owner-drawn buttons. A button does not get hover
// messages of its own, so each one is subclassed to track the pointer.
struct ButtonVisual {
  bool hovered = false;
  bool primary = false;
};

LRESULT CALLBACK ButtonSubclassProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam,
                                    UINT_PTR id, DWORD_PTR refData) {
  auto* visual = reinterpret_cast<ButtonVisual*>(refData);
  switch (msg) {
    case WM_MOUSEMOVE:
      if (visual && !visual->hovered) {
        visual->hovered = true;
        TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&track);
        InvalidateRect(hwnd, nullptr, TRUE);
      }
      break;
    case WM_MOUSELEAVE:
      if (visual && visual->hovered) {
        visual->hovered = false;
        InvalidateRect(hwnd, nullptr, TRUE);
      }
      break;
    case WM_NCDESTROY:
      RemoveWindowSubclass(hwnd, ButtonSubclassProc, id);
      break;
    default:
      break;
  }
  return DefSubclassProc(hwnd, msg, wparam, lparam);
}

// Everything the update thread hands back. Allocated by the worker, owned by
// the window once the message arrives.
struct UpdateResult {
  UpdateInfo info;
  bool userInitiated = false;
  bool succeeded = false;
  std::wstring error;
};

struct UpdateJob {
  HWND hwnd = nullptr;
  bool userInitiated = false;
  bool download = false;
  UpdateInfo info;
};

DWORD WINAPI UpdateThreadProc(LPVOID parameter) {
  auto* job = static_cast<UpdateJob*>(parameter);
  auto* result = new UpdateResult();
  result->userInitiated = job->userInitiated;

  UINT message = kUpdateCheckedMessage;
  if (job->download) {
    message = kUpdateDownloadedMessage;
    result->info = job->info;
    result->succeeded = DownloadAndRunInstaller(job->info, &result->error);
  } else {
    result->info = CheckForUpdate();
    result->succeeded = result->info.checked;
    result->error = result->info.error;
  }

  // If the window is already gone the result has nowhere to go.
  if (!PostMessageW(job->hwnd, message, 0, reinterpret_cast<LPARAM>(result))) {
    delete result;
  }
  delete job;
  return 0;
}

// The design is laid out in 96-dpi units and scaled to whatever the window is
// actually on. Everything below is one of those units.
namespace layout {
constexpr int kWindowWidth = 720;
constexpr int kWindowHeight = 640;
constexpr int kMargin = 24;
constexpr int kCardPadding = 20;
constexpr int kCardRadius = 8;
constexpr int kButtonRadius = 6;

constexpr int kTitleY = 20;
constexpr int kSubtitleY = 48;

constexpr int kCard1Y = 84;
constexpr int kCard1Height = 168;
constexpr int kSourceLabelY = 104;
constexpr int kSourceComboY = 124;
constexpr int kTargetLabelY = 166;
constexpr int kTargetComboY = 186;
constexpr int kStatusY = 226;

constexpr int kCard2Y = 266;
constexpr int kCard2Height = 118;
constexpr int kOptionRow1Y = 286;
constexpr int kOptionRow2Y = 314;
constexpr int kOptionRow3Y = 342;
constexpr int kOptionCol2X = 368;
constexpr int kOptionWidth = 300;

constexpr int kButtonY = 400;
constexpr int kButtonHeight = 36;

constexpr int kHintY = 452;
constexpr int kLogLabelY = 480;
constexpr int kLogY = 500;
constexpr int kLogHeight = 116;
}  // namespace layout

class ConfigWindow {
 public:
  bool Create(bool startMinimized);
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

  int IndexOfPersistentId(const std::wstring& persistentId) const;
  void SaveCurrentSettings();
  void MaybeAutoStartMirroring();

  // Presentation.
  void ReloadTheme();
  void ReleaseThemeObjects();
  void CreateFonts();
  void Relayout();
  void PaintWindow(HDC dc);
  void DrawButton(const DRAWITEMSTRUCT& item);
  int S(int value) const { return Scaled(value, dpi_); }

  // Tray icon. The window is hidden rather than minimised, so it leaves the
  // taskbar entirely and the icon is the only way back to it.
  bool AddTrayIcon();
  void RemoveTrayIcon();
  void UpdateTrayTooltip();
  void ShowTrayBalloon(const std::wstring& title, const std::wstring& text);
  void HideToTray();
  void ShowFromTray();
  void ShowTrayMenu();

  // Updates.
  void StartUpdateCheck(bool userInitiated);
  void StartUpdateDownload();
  void OnUpdateChecked(UpdateResult* result);
  void OnUpdateDownloaded(UpdateResult* result);
  void OfferUpdate();

  HWND hwnd_ = nullptr;
  HWND sourceCombo_ = nullptr;
  HWND targetCombo_ = nullptr;
  HWND cursorCheck_ = nullptr;
  HWND tearingCheck_ = nullptr;
  HWND autoRunCheck_ = nullptr;
  HWND autoMirrorCheck_ = nullptr;
  HWND updateCheckBox_ = nullptr;
  HWND startButton_ = nullptr;
  HWND refreshButton_ = nullptr;
  HWND updateButton_ = nullptr;
  HWND logBox_ = nullptr;

  UINT dpi_ = 96;
  Palette palette_;
  HFONT titleFont_ = nullptr;
  HFONT bodyFont_ = nullptr;
  HFONT labelFont_ = nullptr;
  HFONT monoFont_ = nullptr;
  HBRUSH cardBrush_ = nullptr;
  HBRUSH windowBrush_ = nullptr;
  HBRUSH controlBrush_ = nullptr;

  ButtonVisual startVisual_;
  ButtonVisual refreshVisual_;
  ButtonVisual updateVisual_;

  std::vector<DisplayInfo> displays_;
  MirrorSession session_;
  Settings settings_;
  std::wstring statusText_ = L"Not mirroring.";

  UpdateInfo pendingUpdate_;
  bool updateInFlight_ = false;

  bool tearingSupported_ = false;
  bool hotkeyRegistered_ = false;
  bool trayIconAdded_ = false;
  HICON trayIcon_ = nullptr;
  bool trayHintShown_ = false;
  // Set when the user stops mirroring by hand, so auto-start does not
  // immediately undo it. Cleared as soon as the saved pair is incomplete
  // again, which is what makes switching the TV off and on re-arm it.
  bool autoStartSuppressed_ = false;
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

// ---------------------------------------------------------------- appearance

void ConfigWindow::ReleaseThemeObjects() {
  if (cardBrush_) DeleteObject(cardBrush_);
  if (windowBrush_) DeleteObject(windowBrush_);
  if (controlBrush_) DeleteObject(controlBrush_);
  cardBrush_ = windowBrush_ = controlBrush_ = nullptr;
}

void ConfigWindow::ReloadTheme() {
  palette_ = LoadPalette();
  ReleaseThemeObjects();
  cardBrush_ = CreateSolidBrush(palette_.cardBackground);
  windowBrush_ = CreateSolidBrush(palette_.windowBackground);
  controlBrush_ = CreateSolidBrush(palette_.controlBackground);

  EnableDarkModeForApp(palette_.dark);
  if (hwnd_) {
    ApplyWindowChrome(hwnd_, palette_.dark);
    // Combo boxes have their own dark theme class; everything else uses the
    // Explorer one.
    ApplyControlTheme(sourceCombo_, palette_.dark, L"DarkMode_CFD");
    ApplyControlTheme(targetCombo_, palette_.dark, L"DarkMode_CFD");
    for (HWND control : {cursorCheck_, tearingCheck_, autoRunCheck_, autoMirrorCheck_,
                         updateCheckBox_, logBox_}) {
      ApplyControlTheme(control, palette_.dark);
    }
    InvalidateRect(hwnd_, nullptr, TRUE);
  }
}

void ConfigWindow::CreateFonts() {
  if (titleFont_) DeleteObject(titleFont_);
  if (bodyFont_) DeleteObject(bodyFont_);
  if (labelFont_) DeleteObject(labelFont_);
  if (monoFont_) DeleteObject(monoFont_);

  auto make = [&](int points, int weight, const wchar_t* face) {
    return CreateFontW(-MulDiv(points, static_cast<int>(dpi_), 72), 0, 0, 0, weight,
                       FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, face);
  };

  // Segoe UI Variable is the Windows 11 face; GDI falls back to Segoe UI on
  // Windows 10, which is the right answer there anyway.
  titleFont_ = make(15, FW_SEMIBOLD, L"Segoe UI Variable Display");
  bodyFont_ = make(9, FW_NORMAL, L"Segoe UI Variable Text");
  labelFont_ = make(8, FW_SEMIBOLD, L"Segoe UI Variable Small");
  monoFont_ = make(8, FW_NORMAL, L"Consolas");
}

void ConfigWindow::Relayout() {
  using namespace layout;
  CreateFonts();

  const int contentWidth = kWindowWidth - 2 * kMargin;
  const int innerX = kMargin + kCardPadding;
  const int innerWidth = contentWidth - 2 * kCardPadding;

  struct Placement {
    HWND control;
    int x, y, w, h;
    HFONT font;
  };
  const Placement placements[] = {
      {sourceCombo_, innerX, kSourceComboY, innerWidth, 300, bodyFont_},
      {targetCombo_, innerX, kTargetComboY, innerWidth, 300, bodyFont_},
      {cursorCheck_, innerX, kOptionRow1Y, kOptionWidth, 24, bodyFont_},
      {tearingCheck_, innerX, kOptionRow2Y, kOptionWidth, 24, bodyFont_},
      {updateCheckBox_, innerX, kOptionRow3Y, kOptionWidth, 24, bodyFont_},
      {autoRunCheck_, kOptionCol2X, kOptionRow1Y, kOptionWidth, 24, bodyFont_},
      {autoMirrorCheck_, kOptionCol2X, kOptionRow2Y, kOptionWidth, 24, bodyFont_},
      {startButton_, kMargin, kButtonY, 190, kButtonHeight, bodyFont_},
      {refreshButton_, kMargin + 202, kButtonY, 160, kButtonHeight, bodyFont_},
      {updateButton_, kMargin + 374, kButtonY, 180, kButtonHeight, bodyFont_},
      {logBox_, kMargin, kLogY, contentWidth, kLogHeight, monoFont_},
  };

  for (const Placement& p : placements) {
    if (!p.control) continue;
    MoveWindow(p.control, S(p.x), S(p.y), S(p.w), S(p.h), TRUE);
    SendMessageW(p.control, WM_SETFONT, reinterpret_cast<WPARAM>(p.font), TRUE);
  }

  // A combo box sizes its closed height from its font, so it has to be told
  // again after a font change or it keeps the old one.
  for (HWND combo : {sourceCombo_, targetCombo_}) {
    if (combo) SendMessageW(combo, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), S(22));
  }
}

void ConfigWindow::PaintWindow(HDC target) {
  using namespace layout;

  RECT client = {};
  GetClientRect(hwnd_, &client);

  // Draw into a bitmap first: the cards and the text would otherwise flash
  // every time a control repaints over them.
  HDC dc = CreateCompatibleDC(target);
  HBITMAP bitmap = CreateCompatibleBitmap(target, client.right, client.bottom);
  HGDIOBJ oldBitmap = SelectObject(dc, bitmap);

  FillRect(dc, &client, windowBrush_);
  SetBkMode(dc, TRANSPARENT);

  auto text = [&](const std::wstring& value, HFONT font, COLORREF colour, int x, int y,
                  int w, UINT format) {
    HGDIOBJ old = SelectObject(dc, font);
    SetTextColor(dc, colour);
    RECT rect = {S(x), S(y), S(x + w), S(y) + S(40)};
    DrawTextW(dc, value.c_str(), -1, &rect, format | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, old);
  };

  text(L"DisplayMirror", titleFont_, palette_.text, kMargin, kTitleY, 400, DT_LEFT);
  text(L"Version " DM_VERSION_STRING, bodyFont_, palette_.textSecondary,
       kWindowWidth - kMargin - 200, kTitleY + 6, 200, DT_RIGHT);
  text(L"Each display keeps its own resolution and refresh rate.", bodyFont_,
       palette_.textSecondary, kMargin, kSubtitleY, 600, DT_LEFT);

  RECT card1 = {S(kMargin), S(kCard1Y), S(kWindowWidth - kMargin),
                S(kCard1Y + kCard1Height)};
  FillRoundedRect(dc, card1, S(kCardRadius), palette_.cardBackground, palette_.cardBorder,
                  1);

  RECT card2 = {S(kMargin), S(kCard2Y), S(kWindowWidth - kMargin),
                S(kCard2Y + kCard2Height)};
  FillRoundedRect(dc, card2, S(kCardRadius), palette_.cardBackground, palette_.cardBorder,
                  1);

  const int innerX = kMargin + kCardPadding;
  text(L"SOURCE DISPLAY · CAPTURED", labelFont_, palette_.textSecondary, innerX,
       kSourceLabelY, 400, DT_LEFT);
  text(L"TARGET DISPLAY · MIRRORED TO", labelFont_, palette_.textSecondary, innerX,
       kTargetLabelY, 400, DT_LEFT);
  text(statusText_, bodyFont_,
       session_.IsRunning() ? palette_.accent : palette_.textSecondary, innerX, kStatusY,
       kWindowWidth - 2 * innerX, DT_LEFT);

  text(L"Ctrl+Alt+M toggles mirroring from anywhere · ESC on the output window "
       L"stops · settings save themselves",
       bodyFont_, palette_.textSecondary, kMargin, kHintY, kWindowWidth - 2 * kMargin,
       DT_LEFT);
  text(L"LOG", labelFont_, palette_.textSecondary, kMargin, kLogLabelY, 200, DT_LEFT);

  BitBlt(target, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
  SelectObject(dc, oldBitmap);
  DeleteObject(bitmap);
  DeleteDC(dc);
}

void ConfigWindow::DrawButton(const DRAWITEMSTRUCT& item) {
  ButtonVisual* visual = nullptr;
  switch (item.CtlID) {
    case kIdStartButton: visual = &startVisual_; break;
    case kIdRefreshButton: visual = &refreshVisual_; break;
    case kIdUpdateButton: visual = &updateVisual_; break;
    default: return;
  }

  const bool disabled = (item.itemState & ODS_DISABLED) != 0;
  const bool pressed = (item.itemState & ODS_SELECTED) != 0;

  COLORREF fill;
  COLORREF border;
  COLORREF textColour;
  if (visual->primary) {
    fill = pressed ? palette_.accentPressed
                   : (visual->hovered ? palette_.accentHover : palette_.accent);
    border = fill;
    textColour = palette_.accentText;
    if (disabled) {
      fill = palette_.dark ? RGB(60, 60, 60) : RGB(224, 224, 224);
      border = fill;
      textColour = palette_.disabled;
    }
  } else {
    fill = pressed ? palette_.controlHover
                   : (visual->hovered ? palette_.controlHover : palette_.controlBackground);
    border = palette_.controlBorder;
    textColour = disabled ? palette_.disabled : palette_.text;
  }

  FillRoundedRect(item.hDC, item.rcItem, S(layout::kButtonRadius), fill, border, 1);

  // A focus ring, drawn just inside the edge so it reads as a ring rather than
  // as a thicker border.
  if ((item.itemState & ODS_FOCUS) && !disabled) {
    RECT ring = item.rcItem;
    InflateRect(&ring, -S(3), -S(3));
    FillRoundedRect(item.hDC, ring, S(layout::kButtonRadius - 2), fill,
                    visual->primary ? palette_.accentText : palette_.accent, 1);
  }

  wchar_t caption[128] = {};
  GetWindowTextW(item.hwndItem, caption, _countof(caption));

  SetBkMode(item.hDC, TRANSPARENT);
  SetTextColor(item.hDC, textColour);
  HGDIOBJ oldFont = SelectObject(item.hDC, bodyFont_);
  RECT textRect = item.rcItem;
  DrawTextW(item.hDC, caption, -1, &textRect,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  SelectObject(item.hDC, oldFont);
}

void ConfigWindow::CreateControls(HWND parent) {
  auto add = [&](const wchar_t* cls, const wchar_t* caption, DWORD style,
                 int id) -> HWND {
    return CreateWindowExW(0, cls, caption, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10,
                           parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                           GetModuleHandleW(nullptr), nullptr);
  };

  sourceCombo_ = add(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, kIdSourceCombo);
  targetCombo_ = add(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, kIdTargetCombo);

  cursorCheck_ = add(L"BUTTON", L"Mirror the mouse cursor", BS_AUTOCHECKBOX,
                     kIdCursorCheck);
  SendMessageW(cursorCheck_, BM_SETCHECK,
               settings_.drawCursor ? BST_CHECKED : BST_UNCHECKED, 0);

  tearingCheck_ = add(L"BUTTON", L"Allow tearing (lowest latency)", BS_AUTOCHECKBOX,
                      kIdTearingCheck);
  // Defaults to on, so it is only ever off because the user turned it off or
  // because this system does not report tearing support.
  SendMessageW(tearingCheck_, BM_SETCHECK,
               (settings_.allowTearing && tearingSupported_) ? BST_CHECKED : BST_UNCHECKED,
               0);
  EnableWindow(tearingCheck_, tearingSupported_);

  updateCheckBox_ = add(L"BUTTON", L"Check for updates on GitHub", BS_AUTOCHECKBOX,
                        kIdUpdateCheckBox);
  SendMessageW(updateCheckBox_, BM_SETCHECK,
               settings_.checkForUpdates ? BST_CHECKED : BST_UNCHECKED, 0);

  autoRunCheck_ = add(L"BUTTON", L"Start with Windows", BS_AUTOCHECKBOX, kIdAutoRunCheck);
  // Read from the Run key rather than from our own settings, so the checkbox
  // cannot disagree with what Windows will actually do.
  SendMessageW(autoRunCheck_, BM_SETCHECK,
               IsStartWithWindowsEnabled() ? BST_CHECKED : BST_UNCHECKED, 0);

  autoMirrorCheck_ = add(L"BUTTON", L"Auto-start when both displays are here",
                         BS_AUTOCHECKBOX, kIdAutoMirrorCheck);
  SendMessageW(autoMirrorCheck_, BM_SETCHECK,
               settings_.autoMirror ? BST_CHECKED : BST_UNCHECKED, 0);

  startButton_ = add(L"BUTTON", L"Start mirroring", BS_OWNERDRAW, kIdStartButton);
  refreshButton_ = add(L"BUTTON", L"Refresh displays", BS_OWNERDRAW, kIdRefreshButton);
  updateButton_ = add(L"BUTTON", L"Check for updates", BS_OWNERDRAW, kIdUpdateButton);

  startVisual_.primary = true;
  SetWindowSubclass(startButton_, ButtonSubclassProc, 1,
                    reinterpret_cast<DWORD_PTR>(&startVisual_));
  SetWindowSubclass(refreshButton_, ButtonSubclassProc, 2,
                    reinterpret_cast<DWORD_PTR>(&refreshVisual_));
  SetWindowSubclass(updateButton_, ButtonSubclassProc, 3,
                    reinterpret_cast<DWORD_PTR>(&updateVisual_));

  logBox_ = add(L"EDIT", L"",
                ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_BORDER,
                kIdLogBox);

  ReloadTheme();
  Relayout();
}

// ------------------------------------------------------------------- displays

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

  // The saved pair wins, then whatever was selected before the refresh, then
  // the defaults: capture the primary display, mirror to the first one that is
  // not it.
  int source = IndexOfPersistentId(settings_.sourceId);
  if (source < 0) source = previousSource;
  if (source < 0 || source >= static_cast<int>(displays_.size())) {
    source = 0;
    for (size_t i = 0; i < displays_.size(); ++i) {
      if (displays_[i].primary) {
        source = static_cast<int>(i);
        break;
      }
    }
  }

  int target = IndexOfPersistentId(settings_.targetId);
  if (target < 0) target = previousTarget;
  if (target < 0 || target >= static_cast<int>(displays_.size()) || target == source) {
    target = (source + 1) % static_cast<int>(displays_.size());
  }

  SendMessageW(sourceCombo_, CB_SETCURSEL, source, 0);
  SendMessageW(targetCombo_, CB_SETCURSEL, target, 0);
  UpdateStartButton();
}

int ConfigWindow::IndexOfPersistentId(const std::wstring& persistentId) const {
  if (persistentId.empty()) return -1;
  for (size_t i = 0; i < displays_.size(); ++i) {
    if (displays_[i].persistentId == persistentId) return static_cast<int>(i);
  }
  return -1;
}

void ConfigWindow::SaveCurrentSettings() {
  const int source = static_cast<int>(SendMessageW(sourceCombo_, CB_GETCURSEL, 0, 0));
  const int target = static_cast<int>(SendMessageW(targetCombo_, CB_GETCURSEL, 0, 0));
  const int count = static_cast<int>(displays_.size());

  // A selection is only overwritten when there is a real display behind it, so
  // unplugging the TV does not wipe the pair the user saved.
  if (source >= 0 && source < count) {
    settings_.sourceId = displays_[static_cast<size_t>(source)].persistentId;
    settings_.sourceName = displays_[static_cast<size_t>(source)].friendlyName;
  }
  if (target >= 0 && target < count) {
    settings_.targetId = displays_[static_cast<size_t>(target)].persistentId;
    settings_.targetName = displays_[static_cast<size_t>(target)].friendlyName;
  }

  settings_.drawCursor = SendMessageW(cursorCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
  settings_.allowTearing =
      tearingSupported_ && SendMessageW(tearingCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
  settings_.autoMirror = SendMessageW(autoMirrorCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
  settings_.checkForUpdates =
      SendMessageW(updateCheckBox_, BM_GETCHECK, 0, 0) == BST_CHECKED;
  settings_.valid = true;
  SaveSettings(settings_);
}

void ConfigWindow::MaybeAutoStartMirroring() {
  const DisplayInfo* source = FindDisplayById(displays_, settings_.sourceId);
  const DisplayInfo* target = FindDisplayById(displays_, settings_.targetId);

  if (!source || !target || source == target) {
    // The pair is incomplete, so re-arm: after the TV comes back on, auto-start
    // should fire again even if the last session was stopped by hand.
    autoStartSuppressed_ = false;
    return;
  }

  if (!settings_.autoMirror || session_.IsRunning() || autoStartSuppressed_) return;

  DM_INFO(L"Both saved displays are connected (%s -> %s); starting automatically.",
          source->friendlyName.c_str(), target->friendlyName.c_str());
  StartMirroring();
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
  EnableWindow(tearingCheck_, tearingSupported_ && !session_.IsRunning());

  if (session_.IsRunning()) {
    const MirrorConfig& config = session_.Config();
    wchar_t buffer[256];
    _snwprintf_s(buffer, _countof(buffer), _TRUNCATE,
                 L"Mirroring %s (%ux%u @ %.0f Hz) to %s (%ux%u @ %.0f Hz).",
                 config.source.friendlyName.c_str(), config.source.width,
                 config.source.height, config.source.RefreshHz(),
                 config.target.friendlyName.c_str(), config.target.width,
                 config.target.height, config.target.RefreshHz());
    statusText_ = buffer;
  } else if (displays_.size() < 2) {
    statusText_ = L"Only one display is attached; there is nothing to mirror onto.";
  } else {
    statusText_ = L"Not mirroring.";
  }

  UpdateTrayTooltip();
  if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

// -------------------------------------------------------------------- session

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

  SaveCurrentSettings();
  UpdateStartButton();
  // Get out of the way entirely, but stay reachable through the tray icon.
  HideToTray();
}

void ConfigWindow::StopMirroring() {
  // Stopping by hand wins over auto-start until the pair is broken and remade.
  autoStartSuppressed_ = true;
  session_.Stop();
  UpdateStartButton();
  // The window is deliberately left as it is. Stopping from the hotkey or the
  // tray menu should not throw a window in front of whatever is on screen; the
  // tray tooltip carries the state instead.
}

void ConfigWindow::ToggleMirroring() {
  if (session_.IsRunning()) {
    StopMirroring();
  } else {
    StartMirroring();
  }
}

// ------------------------------------------------------------------ tray icon

bool ConfigWindow::AddTrayIcon() {
  NOTIFYICONDATAW data = {};
  data.cbSize = sizeof(data);
  data.hWnd = hwnd_;
  data.uID = kTrayIconId;
  data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  data.uCallbackMessage = kTrayCallbackMessage;
  if (!trayIcon_) {
    trayIcon_ = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),
                                              MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                              GetSystemMetrics(SM_CXSMICON),
                                              GetSystemMetrics(SM_CYSMICON), 0));
  }
  data.hIcon = trayIcon_ ? trayIcon_ : LoadIconW(nullptr, IDI_APPLICATION);
  wcscpy_s(data.szTip, L"DisplayMirror");

  trayIconAdded_ = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
  if (!trayIconAdded_) {
    DM_WARN(L"The tray icon could not be added; the window stays in the taskbar.");
    return false;
  }
  UpdateTrayTooltip();
  return true;
}

void ConfigWindow::RemoveTrayIcon() {
  if (!trayIconAdded_) return;
  NOTIFYICONDATAW data = {};
  data.cbSize = sizeof(data);
  data.hWnd = hwnd_;
  data.uID = kTrayIconId;
  Shell_NotifyIconW(NIM_DELETE, &data);
  trayIconAdded_ = false;
  if (trayIcon_) {
    DestroyIcon(trayIcon_);
    trayIcon_ = nullptr;
  }
}

void ConfigWindow::UpdateTrayTooltip() {
  if (!trayIconAdded_) return;

  NOTIFYICONDATAW data = {};
  data.cbSize = sizeof(data);
  data.hWnd = hwnd_;
  data.uID = kTrayIconId;
  data.uFlags = NIF_TIP;

  // The tooltip is the only status this thing shows while it is hidden, so it
  // names the pair rather than just saying "running".
  if (session_.IsRunning()) {
    _snwprintf_s(data.szTip, _countof(data.szTip), _TRUNCATE, L"DisplayMirror - %s to %s",
                 session_.Config().source.friendlyName.c_str(),
                 session_.Config().target.friendlyName.c_str());
  } else {
    wcscpy_s(data.szTip, L"DisplayMirror - not mirroring");
  }
  Shell_NotifyIconW(NIM_MODIFY, &data);
}

void ConfigWindow::ShowTrayBalloon(const std::wstring& title, const std::wstring& text) {
  if (!trayIconAdded_) return;
  NOTIFYICONDATAW data = {};
  data.cbSize = sizeof(data);
  data.hWnd = hwnd_;
  data.uID = kTrayIconId;
  data.uFlags = NIF_INFO;
  data.dwInfoFlags = NIIF_INFO;
  wcsncpy_s(data.szInfoTitle, title.c_str(), _TRUNCATE);
  wcsncpy_s(data.szInfo, text.c_str(), _TRUNCATE);
  Shell_NotifyIconW(NIM_MODIFY, &data);
}

void ConfigWindow::HideToTray() {
  // Hidden, not minimised: a minimised window still owns a taskbar button.
  ShowWindow(hwnd_, SW_HIDE);
  if (!trayIconAdded_) {
    // Without an icon there would be no way back to the window at all.
    ShowWindow(hwnd_, SW_MINIMIZE);
    return;
  }
  if (!trayHintShown_) {
    DM_INFO(L"Running in the notification area. Double-click the tray icon to "
            L"bring this window back, or right-click it for the menu.");
    trayHintShown_ = true;
  }
}

void ConfigWindow::ShowFromTray() {
  ShowWindow(hwnd_, SW_SHOW);
  ShowWindow(hwnd_, SW_RESTORE);
  SetForegroundWindow(hwnd_);
}

void ConfigWindow::ShowTrayMenu() {
  HMENU menu = CreatePopupMenu();
  if (!menu) return;

  AppendMenuW(menu, MF_STRING, kIdTrayShow, L"Show DisplayMirror");
  AppendMenuW(menu, MF_STRING, kIdTrayToggle,
              session_.IsRunning() ? L"Stop mirroring" : L"Start mirroring");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING | (updateInFlight_ ? MF_GRAYED : 0), kIdTrayUpdate,
              L"Check for updates");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kIdTrayExit, L"Exit");
  SetMenuDefaultItem(menu, kIdTrayShow, FALSE);

  POINT cursor = {};
  GetCursorPos(&cursor);

  // Required, or the menu will not dismiss when the user clicks elsewhere.
  SetForegroundWindow(hwnd_);
  TrackPopupMenu(menu, TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, hwnd_, nullptr);
  PostMessageW(hwnd_, WM_NULL, 0, 0);
  DestroyMenu(menu);
}

// -------------------------------------------------------------------- updates

void ConfigWindow::StartUpdateCheck(bool userInitiated) {
  if (updateInFlight_) return;

  auto* job = new UpdateJob();
  job->hwnd = hwnd_;
  job->userInitiated = userInitiated;
  job->download = false;

  HANDLE thread = CreateThread(nullptr, 0, UpdateThreadProc, job, 0, nullptr);
  if (!thread) {
    delete job;
    return;
  }
  CloseHandle(thread);

  updateInFlight_ = true;
  SetWindowTextW(updateButton_, L"Checking…");
  EnableWindow(updateButton_, FALSE);
  settings_.lastUpdateCheck = UnixNow();
  SaveSettings(settings_);
}

void ConfigWindow::StartUpdateDownload() {
  if (updateInFlight_ || !pendingUpdate_.available) return;

  auto* job = new UpdateJob();
  job->hwnd = hwnd_;
  job->download = true;
  job->info = pendingUpdate_;

  HANDLE thread = CreateThread(nullptr, 0, UpdateThreadProc, job, 0, nullptr);
  if (!thread) {
    delete job;
    return;
  }
  CloseHandle(thread);

  updateInFlight_ = true;
  SetWindowTextW(updateButton_, L"Downloading…");
  EnableWindow(updateButton_, FALSE);
}

void ConfigWindow::OfferUpdate() {
  if (!pendingUpdate_.available) return;

  const std::wstring message =
      L"DisplayMirror " + pendingUpdate_.version +
      L" is available. This copy is " DM_VERSION_STRING
      L".\n\nDownload the installer from GitHub and run it now?\n\n"
      L"Mirroring will stop while the update installs.";
  if (MessageBoxW(hwnd_, message.c_str(), L"DisplayMirror update",
                  MB_YESNO | MB_ICONINFORMATION) == IDYES) {
    StartUpdateDownload();
  }
}

void ConfigWindow::OnUpdateChecked(UpdateResult* result) {
  updateInFlight_ = false;
  SetWindowTextW(updateButton_, L"Check for updates");
  EnableWindow(updateButton_, TRUE);

  if (!result->succeeded) {
    DM_WARN(L"Update check failed: %s", result->error.c_str());
    if (result->userInitiated) {
      MessageBoxW(hwnd_,
                  (L"The update check did not succeed.\n\n" + result->error).c_str(),
                  L"DisplayMirror", MB_OK | MB_ICONWARNING);
    }
    return;
  }

  pendingUpdate_ = result->info;
  if (!pendingUpdate_.available) {
    if (result->userInitiated) {
      MessageBoxW(hwnd_, L"DisplayMirror is up to date.", L"DisplayMirror",
                  MB_OK | MB_ICONINFORMATION);
    }
    return;
  }

  // A modal box in front of a game would be worse than the update is urgent,
  // so a hidden window gets a balloon and asks nothing.
  if (IsWindowVisible(hwnd_)) {
    OfferUpdate();
  } else {
    ShowTrayBalloon(L"DisplayMirror " + pendingUpdate_.version + L" is available",
                    L"Click here to install it.");
  }
}

void ConfigWindow::OnUpdateDownloaded(UpdateResult* result) {
  updateInFlight_ = false;
  SetWindowTextW(updateButton_, L"Check for updates");
  EnableWindow(updateButton_, TRUE);

  if (!result->succeeded) {
    DM_ERROR(L"Update download failed: %s", result->error.c_str());
    MessageBoxW(hwnd_, (L"The update could not be downloaded.\n\n" + result->error +
                        L"\n\nThe release page is " + result->info.releaseUrl)
                           .c_str(),
                L"DisplayMirror", MB_OK | MB_ICONWARNING);
  }
  // On success the installer is already running and will close this copy.
}

// ------------------------------------------------------------------- plumbing

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
  // Explorer restarted and every tray icon has to be registered again.
  if (msg == g_taskbarCreatedMessage && g_taskbarCreatedMessage != 0) {
    trayIconAdded_ = false;
    AddTrayIcon();
    return 0;
  }

  if (msg == kTrayCallbackMessage) {
    switch (LOWORD(lparam)) {
      case WM_LBUTTONDBLCLK:
        ShowFromTray();
        return 0;
      case WM_RBUTTONUP:
      case WM_CONTEXTMENU:
        ShowTrayMenu();
        return 0;
      case NIN_BALLOONUSERCLICK:
        ShowFromTray();
        OfferUpdate();
        return 0;
      default:
        return 0;
    }
  }

  if (msg == kUpdateCheckedMessage || msg == kUpdateDownloadedMessage) {
    auto* result = reinterpret_cast<UpdateResult*>(lparam);
    if (msg == kUpdateCheckedMessage) {
      OnUpdateChecked(result);
    } else {
      OnUpdateDownloaded(result);
    }
    delete result;
    return 0;
  }

  switch (msg) {
    case WM_CREATE:
      dpi_ = GetDpiForWindow(hwnd);
      CreateControls(hwnd);
      RefreshDisplays();
      return 0;

    case WM_ERASEBKGND:
      return 1;  // WM_PAINT covers every pixel.

    case WM_PAINT: {
      PAINTSTRUCT ps = {};
      HDC dc = BeginPaint(hwnd, &ps);
      PaintWindow(dc);
      EndPaint(hwnd, &ps);
      return 0;
    }

    case WM_DRAWITEM:
      DrawButton(*reinterpret_cast<DRAWITEMSTRUCT*>(lparam));
      return TRUE;

    // Checkboxes and statics ask their parent for colours; the buttons are
    // owner-drawn and never get here.
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
      auto dc = reinterpret_cast<HDC>(wparam);
      SetTextColor(dc, palette_.text);
      SetBkColor(dc, palette_.cardBackground);
      return reinterpret_cast<LRESULT>(cardBrush_);
    }

    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
      auto dc = reinterpret_cast<HDC>(wparam);
      SetTextColor(dc, palette_.text);
      SetBkColor(dc, palette_.controlBackground);
      return reinterpret_cast<LRESULT>(controlBrush_);
    }

    case WM_SETTINGCHANGE:
      // Sent when the user switches between light and dark while we are up.
      if (lparam && wcscmp(reinterpret_cast<const wchar_t*>(lparam),
                           L"ImmersiveColorSet") == 0) {
        ReloadTheme();
      }
      return 0;

    case WM_DPICHANGED: {
      dpi_ = HIWORD(wparam);
      const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
      SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                   suggested->right - suggested->left, suggested->bottom - suggested->top,
                   SWP_NOZORDER | SWP_NOACTIVATE);
      Relayout();
      InvalidateRect(hwnd, nullptr, TRUE);
      return 0;
    }

    case WM_COMMAND:
      switch (LOWORD(wparam)) {
        case kIdStartButton:
        case kIdTrayToggle:
          ToggleMirroring();
          return 0;
        case kIdTrayShow:
          ShowFromTray();
          return 0;
        case kIdTrayExit:
          DestroyWindow(hwnd);
          return 0;
        case kIdRefreshButton:
          if (!session_.IsRunning()) RefreshDisplays();
          return 0;
        case kIdUpdateButton:
        case kIdTrayUpdate:
          StartUpdateCheck(/*userInitiated=*/true);
          return 0;
        case kIdCursorCheck:
          if (session_.IsRunning()) {
            session_.SetDrawCursor(
                SendMessageW(cursorCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED);
          }
          SaveCurrentSettings();
          return 0;
        case kIdTearingCheck:
          // Takes effect on the next start: the flag is baked into the swap
          // chain, so changing it mid-session would mean rebuilding it.
          if (session_.IsRunning()) {
            DM_INFO(L"Tearing mode changed; it applies the next time mirroring "
                    L"starts.");
          }
          SaveCurrentSettings();
          return 0;
        case kIdUpdateCheckBox:
          SaveCurrentSettings();
          return 0;
        case kIdAutoMirrorCheck:
          SaveCurrentSettings();
          // Ticking the box with both displays already connected should act
          // immediately rather than waiting for the next display change.
          if (!session_.IsRunning()) {
            autoStartSuppressed_ = false;
            MaybeAutoStartMirroring();
          }
          return 0;
        case kIdAutoRunCheck: {
          const bool wanted =
              SendMessageW(autoRunCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
          if (!SetStartWithWindows(wanted)) {
            // The registry write failed, so put the checkbox back rather than
            // leaving it claiming something that is not true.
            SendMessageW(autoRunCheck_, BM_SETCHECK,
                         wanted ? BST_UNCHECKED : BST_CHECKED, 0);
          }
          return 0;
        }
        case kIdSourceCombo:
        case kIdTargetCombo:
          if (HIWORD(wparam) == CBN_SELCHANGE) {
            UpdateStartButton();
            SaveCurrentSettings();
          }
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
        // This is the hot-plug path: the TV was switched on, or came back from
        // standby, and the saved pair may now be complete.
        MaybeAutoStartMirroring();
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

    case WM_SYSCOMMAND:
      // Minimising hides the window instead: a minimised window still owns a
      // taskbar button, and the point of the tray icon is to not have one.
      if ((wparam & 0xFFF0) == SC_MINIMIZE) {
        HideToTray();
        return 0;
      }
      break;

    case WM_CLOSE:
      // Closing hides too. This thing is meant to sit there waiting for the TV
      // to come on, so the X button must not end that; Exit in the tray menu
      // is what quits. With no tray icon there would be nothing to come back
      // from, so then it really does close.
      if (trayIconAdded_) {
        HideToTray();
        return 0;
      }
      DestroyWindow(hwnd);
      return 0;

    case WM_DESTROY:
      RemoveTrayIcon();
      session_.Stop();
      WTSUnRegisterSessionNotification(hwnd);
      if (hotkeyRegistered_) UnregisterHotKey(hwnd, kHotkeyToggle);
      if (titleFont_) DeleteObject(titleFont_);
      if (bodyFont_) DeleteObject(bodyFont_);
      if (labelFont_) DeleteObject(labelFont_);
      if (monoFont_) DeleteObject(monoFont_);
      ReleaseThemeObjects();
      PostQuitMessage(0);
      return 0;

    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

bool ConfigWindow::Create(bool startMinimized) {
  tearingSupported_ = Renderer::SystemSupportsTearing();
  // Before the window exists: WM_CREATE builds the controls from these.
  settings_ = LoadSettings();
  // No installer, so the Run entry is just this file's path. If the exe was
  // moved since it was enabled, repair the entry now rather than let Windows
  // fail silently at the next logon.
  RefreshStartWithWindowsPath();

  palette_ = LoadPalette();
  EnableDarkModeForApp(palette_.dark);

  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = &ConfigWindow::WndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = nullptr;  // WM_PAINT owns every pixel.
  wc.lpszClassName = kConfigClass;
  wc.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APPICON));
  if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
  wc.hIconSm = wc.hIcon;
  if (!RegisterClassExW(&wc)) return false;

  const UINT dpi = GetDpiForSystem();
  RECT rect = {0, 0, Scaled(layout::kWindowWidth, dpi),
               Scaled(layout::kWindowHeight, dpi)};
  const DWORD style = (WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX) |
                      WS_CLIPCHILDREN;
  AdjustWindowRectExForDpi(&rect, style, FALSE, 0, dpi);

  hwnd_ = CreateWindowExW(0, kConfigClass, L"DisplayMirror", style, CW_USEDEFAULT,
                          CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top,
                          nullptr, nullptr, GetModuleHandleW(nullptr), this);
  if (!hwnd_) return false;

  ApplyWindowChrome(hwnd_, palette_.dark);

  // Without this subscription WM_WTSSESSION_CHANGE is never delivered.
  WTSRegisterSessionNotification(hwnd_, NOTIFY_FOR_THIS_SESSION);

  if (g_taskbarCreatedMessage == 0) {
    g_taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
  }
  AddTrayIcon();

  hotkeyRegistered_ = RegisterHotKey(hwnd_, kHotkeyToggle, kHotkeyModifiers, kHotkeyVk) != FALSE;
  if (!hotkeyRegistered_) {
    DM_WARN(L"Could not register the Ctrl+Alt+M hotkey; another application owns it.");
  }

  DM_INFO(L"Tearing support: %s", tearingSupported_ ? L"yes" : L"no");

  if (startMinimized && trayIconAdded_) {
    // Started from the Run key: no window, no taskbar button, just the icon.
    DM_INFO(L"Started minimised to the notification area.");
    trayHintShown_ = true;
  } else {
    ShowWindow(hwnd_, startMinimized ? SW_SHOWMINNOACTIVE : SW_SHOW);
    UpdateWindow(hwnd_);
  }

  // Only now, with the window up and the log pane able to show what happens.
  MaybeAutoStartMirroring();

  if (settings_.checkForUpdates &&
      UnixNow() - settings_.lastUpdateCheck > kUpdateCheckIntervalSeconds) {
    StartUpdateCheck(/*userInitiated=*/false);
  }
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
        ShowFromTray();
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
  dm::InitGraphics();

  // Started from the Run key, the window would otherwise appear in front of
  // whatever the user is doing at logon.
  const wchar_t* commandLine = GetCommandLineW();
  const bool startMinimized =
      commandLine != nullptr && wcsstr(commandLine, L"--minimized") != nullptr;

  dm::ConfigWindow window;
  dm::g_window = &window;
  dm::LogInit(&dm::ConfigWindow::LogSinkThunk);
  DM_INFO(L"DisplayMirror " DM_VERSION_STRING L" starting.");

  int exitCode = 1;
  if (window.Create(startMinimized)) {
    exitCode = window.Run();
  } else {
    MessageBoxW(nullptr, L"The DisplayMirror window could not be created.",
                L"DisplayMirror", MB_OK | MB_ICONERROR);
  }

  DM_INFO(L"DisplayMirror exiting.");
  dm::g_window = nullptr;
  dm::LogShutdown();
  dm::ShutdownGraphics();
  return exitCode;
}
