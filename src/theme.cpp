#include "theme.h"

// gdiplus.h uses unqualified min and max, which NOMINMAX takes away.
#include <algorithm>
using std::max;
using std::min;

#include <dwmapi.h>
// objidl.h before gdiplus.h, as the GDI+ documentation requires: its headers
// use STDMETHOD and IStream, which WIN32_LEAN_AND_MEAN keeps windows.h from
// pulling in. MinGW happens to get them by another route; MSVC does not.
#include <objidl.h>
#include <gdiplus.h>
#include <uxtheme.h>

namespace dm {
namespace {

ULONG_PTR g_gdiplusToken = 0;

// Present since Windows 10 1809, documented since Windows 11.
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWCP_ROUND
#define DWMWCP_ROUND 2
#endif

// uxtheme.dll ordinal 135. Undocumented, and the only way to make standard
// common controls draw dark; every Windows application that does this uses it.
enum class PreferredAppMode { Default = 0, AllowDark = 1, ForceDark = 2 };
using SetPreferredAppModeFn = PreferredAppMode(WINAPI*)(PreferredAppMode);

COLORREF Blend(COLORREF a, COLORREF b, int percentB) {
  const int inverse = 100 - percentB;
  return RGB((GetRValue(a) * inverse + GetRValue(b) * percentB) / 100,
             (GetGValue(a) * inverse + GetGValue(b) * percentB) / 100,
             (GetBValue(a) * inverse + GetBValue(b) * percentB) / 100);
}

// Rec. 601 luma, which is what decides whether text on the accent should be
// white or black.
int Luma(COLORREF c) {
  return (GetRValue(c) * 299 + GetGValue(c) * 587 + GetBValue(c) * 114) / 1000;
}

bool ReadDwordUnderHkcu(const wchar_t* subKey, const wchar_t* name, DWORD* value) {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, subKey, 0, KEY_READ, &key) != ERROR_SUCCESS) {
    return false;
  }
  DWORD type = 0;
  DWORD bytes = sizeof(DWORD);
  const bool ok = RegQueryValueExW(key, name, nullptr, &type,
                                   reinterpret_cast<LPBYTE>(value), &bytes) ==
                      ERROR_SUCCESS &&
                  type == REG_DWORD;
  RegCloseKey(key);
  return ok;
}

bool SystemUsesDarkMode() {
  DWORD lightTheme = 1;
  if (!ReadDwordUnderHkcu(
          L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
          L"AppsUseLightTheme", &lightTheme)) {
    return false;
  }
  return lightTheme == 0;
}

COLORREF SystemAccentColor() {
  // AccentColorMenu is stored as ABGR, which is the accent Windows uses for
  // controls rather than the colourisation of the title bar.
  DWORD accent = 0;
  if (ReadDwordUnderHkcu(L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent",
                         L"AccentColorMenu", &accent)) {
    return RGB(accent & 0xFF, (accent >> 8) & 0xFF, (accent >> 16) & 0xFF);
  }

  BOOL opaque = FALSE;
  DWORD argb = 0;
  if (SUCCEEDED(DwmGetColorizationColor(&argb, &opaque))) {
    return RGB((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF);
  }
  return RGB(0, 120, 212);
}

}  // namespace

Palette LoadPalette() {
  Palette palette;
  palette.dark = SystemUsesDarkMode();
  palette.accent = SystemAccentColor();

  if (palette.dark) {
    palette.windowBackground = RGB(32, 32, 32);
    palette.cardBackground = RGB(43, 43, 43);
    palette.cardBorder = RGB(56, 56, 56);
    palette.text = RGB(255, 255, 255);
    palette.textSecondary = RGB(170, 170, 170);
    palette.controlBackground = RGB(56, 56, 56);
    palette.controlBorder = RGB(72, 72, 72);
    palette.controlHover = RGB(66, 66, 66);
    palette.disabled = RGB(110, 110, 110);
    // A dark background needs a brighter accent to stay legible.
    if (Luma(palette.accent) < 90) {
      palette.accent = Blend(palette.accent, RGB(255, 255, 255), 30);
    }
  }

  palette.accentHover = palette.dark ? Blend(palette.accent, RGB(255, 255, 255), 12)
                                     : Blend(palette.accent, RGB(255, 255, 255), 10);
  palette.accentPressed = Blend(palette.accent, RGB(0, 0, 0), 12);
  palette.accentText = Luma(palette.accent) > 150 ? RGB(0, 0, 0) : RGB(255, 255, 255);
  return palette;
}

void ApplyWindowChrome(HWND hwnd, bool dark) {
  const BOOL useDark = dark ? TRUE : FALSE;
  DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDark, sizeof(useDark));

  const DWORD corner = DWMWCP_ROUND;
  DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
}

void EnableDarkModeForApp(bool dark) {
  HMODULE uxtheme = LoadLibraryExW(L"uxtheme.dll", nullptr,
                                   LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!uxtheme) return;

  auto setPreferredAppMode = reinterpret_cast<SetPreferredAppModeFn>(
      reinterpret_cast<void*>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(135))));
  if (setPreferredAppMode) {
    setPreferredAppMode(dark ? PreferredAppMode::AllowDark : PreferredAppMode::Default);
  }
  // The module stays loaded on purpose: uxtheme is already in the process and
  // the controls keep calling into it.
}

void ApplyControlTheme(HWND control, bool dark, const wchar_t* darkTheme) {
  SetWindowTheme(control, dark ? darkTheme : L"Explorer", nullptr);
}

bool InitGraphics() {
  Gdiplus::GdiplusStartupInput input;
  return Gdiplus::GdiplusStartup(&g_gdiplusToken, &input, nullptr) == Gdiplus::Ok;
}

void ShutdownGraphics() {
  if (g_gdiplusToken) {
    Gdiplus::GdiplusShutdown(g_gdiplusToken);
    g_gdiplusToken = 0;
  }
}

void FillRoundedRect(HDC dc, const RECT& rect, int radius, COLORREF fill,
                     COLORREF border, int borderWidth) {
  Gdiplus::Graphics graphics(dc);
  graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
  graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

  // Inset by half the pen width so the stroke lands inside the rectangle
  // instead of straddling its edge.
  const Gdiplus::REAL inset = borderWidth / 2.0f;
  const Gdiplus::REAL left = static_cast<Gdiplus::REAL>(rect.left) + inset;
  const Gdiplus::REAL top = static_cast<Gdiplus::REAL>(rect.top) + inset;
  const Gdiplus::REAL width = static_cast<Gdiplus::REAL>(rect.right - rect.left) -
                              borderWidth;
  const Gdiplus::REAL height = static_cast<Gdiplus::REAL>(rect.bottom - rect.top) -
                               borderWidth;
  if (width <= 0 || height <= 0) return;

  const Gdiplus::REAL diameter =
      static_cast<Gdiplus::REAL>(radius) * 2.0f;
  const Gdiplus::REAL d = min(diameter, min(width, height));

  Gdiplus::GraphicsPath path;
  if (d <= 0.0f) {
    path.AddRectangle(Gdiplus::RectF(left, top, width, height));
  } else {
    path.AddArc(left, top, d, d, 180.0f, 90.0f);
    path.AddArc(left + width - d, top, d, d, 270.0f, 90.0f);
    path.AddArc(left + width - d, top + height - d, d, d, 0.0f, 90.0f);
    path.AddArc(left, top + height - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
  }

  Gdiplus::SolidBrush brush(
      Gdiplus::Color(255, GetRValue(fill), GetGValue(fill), GetBValue(fill)));
  graphics.FillPath(&brush, &path);

  if (borderWidth > 0 && border != fill) {
    Gdiplus::Pen pen(
        Gdiplus::Color(255, GetRValue(border), GetGValue(border), GetBValue(border)),
        static_cast<Gdiplus::REAL>(borderWidth));
    graphics.DrawPath(&pen, &path);
  }
}

}  // namespace dm
