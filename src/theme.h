// DisplayMirror - the look of the config window.
//
// A Win32 window does not get the Windows 11 look for free: the title bar
// stays light while the system is dark, the corners stay square, and the
// common controls keep their Windows 7 colours. This pulls the actual system
// settings - light/dark and the accent colour - and hands back the palette and
// the few DWM calls that make a plain window look like it belongs.
#pragma once

#include <windows.h>

namespace dm {

struct Palette {
  bool dark = false;

  COLORREF windowBackground = RGB(243, 243, 243);
  COLORREF cardBackground = RGB(255, 255, 255);
  COLORREF cardBorder = RGB(229, 229, 229);
  COLORREF text = RGB(26, 26, 26);
  COLORREF textSecondary = RGB(96, 96, 96);
  COLORREF controlBackground = RGB(255, 255, 255);
  COLORREF controlBorder = RGB(214, 214, 214);
  COLORREF controlHover = RGB(246, 246, 246);

  COLORREF accent = RGB(0, 120, 212);
  COLORREF accentHover = RGB(26, 138, 226);
  COLORREF accentPressed = RGB(0, 103, 184);
  COLORREF accentText = RGB(255, 255, 255);
  COLORREF disabled = RGB(160, 160, 160);
};

// Reads the current system theme and accent colour.
Palette LoadPalette();

// Dark title bar, rounded corners. Both are no-ops on Windows 10, where the
// attributes are simply not supported, so neither is version-gated by hand.
void ApplyWindowChrome(HWND hwnd, bool dark);

// Lets the common controls render in their dark variant. Uses the undocumented
// uxtheme entry point Windows itself uses for this; missing or changed, the
// controls stay light and nothing else happens.
void EnableDarkModeForApp(bool dark);

// Applies the matching visual style to one control, so combo boxes, edit
// controls and their scroll bars follow the theme. `darkTheme` names the dark
// variant to use: "DarkMode_Explorer" for most things, "DarkMode_CFD" for the
// combo boxes, which have their own.
void ApplyControlTheme(HWND control, bool dark,
                       const wchar_t* darkTheme = L"DarkMode_Explorer");

// Antialiased rounded rectangle. Plain GDI has no antialiasing and a 6-pixel
// radius drawn with it looks like a staircase, which is most of the difference
// between "themed" and "cheap".
void FillRoundedRect(HDC dc, const RECT& rect, int radius, COLORREF fill,
                     COLORREF border, int borderWidth = 1);

// GDI+ has to be running before FillRoundedRect is called.
bool InitGraphics();
void ShutdownGraphics();

// Scales a 96-dpi design coordinate to the window's actual dpi.
inline int Scaled(int value, UINT dpi) {
  return MulDiv(value, static_cast<int>(dpi), 96);
}

}  // namespace dm
