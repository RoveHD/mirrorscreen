; DisplayMirror installer.
;
; Per-user by design: the program stores its settings in HKCU, puts its
; autostart entry in the HKCU Run key, and writes its log next to its own
; executable. Installing into Program Files would need elevation for the
; install and then leave the log unwritable, so it goes into the user's own
; Programs directory and never asks for admin.
;
; Build with:  makensis -DAPP_EXE=<path to DisplayMirror.exe> DisplayMirror.nsi

Unicode true

!define APP_NAME    "DisplayMirror"
; Overridden by the release workflow from the tag; keep the default in step
; with src/version.h.
!ifndef APP_VERSION
  !define APP_VERSION "1.0.0"
!endif
!define APP_EXE_NAME "DisplayMirror.exe"
!define UNINSTALL_KEY \
    "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_NAME}"
!define RUN_KEY "Software\Microsoft\Windows\CurrentVersion\Run"
; The config window's class, used to notice a running copy before overwriting
; its executable.
!define WINDOW_CLASS "DisplayMirrorConfigWindow"

!ifndef APP_EXE
  !define APP_EXE "..\build\Release\${APP_EXE_NAME}"
!endif
!ifndef OUT_FILE
  !define OUT_FILE "DisplayMirror-Setup.exe"
!endif

Name "${APP_NAME}"
OutFile "${OUT_FILE}"
InstallDir "$LOCALAPPDATA\Programs\${APP_NAME}"
InstallDirRegKey HKCU "Software\${APP_NAME}" "InstallDir"
RequestExecutionLevel user
SetCompressor /SOLID lzma
ShowInstDetails hide
ShowUninstDetails hide

!include "MUI2.nsh"
!include "WinMessages.nsh"

!define MUI_ICON "..\src\app.ico"
!define MUI_UNICON "..\src\app.ico"
!define MUI_FINISHPAGE_RUN "$INSTDIR\${APP_EXE_NAME}"
!define MUI_FINISHPAGE_RUN_TEXT "Start ${APP_NAME} now"

!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

; A running copy holds its own executable open, so it has to go first.
!macro CloseRunningCopy UN
Function ${UN}CloseRunningCopy
  FindWindow $0 "${WINDOW_CLASS}" ""
  StrCmp $0 0 done
    MessageBox MB_OKCANCEL|MB_ICONEXCLAMATION \
      "${APP_NAME} is running and has to be closed to continue." \
      IDOK closeit
      Abort
    closeit:
      ; WM_CLOSE only hides the window, so ask it to quit through the tray
      ; menu's Exit command (WM_COMMAND with the Exit id) instead.
      SendMessage $0 ${WM_COMMAND} 202 0 /TIMEOUT=3000
      Sleep 1500
  done:
FunctionEnd
!macroend
!insertmacro CloseRunningCopy ""
!insertmacro CloseRunningCopy "un."

Section "Install"
  Call CloseRunningCopy

  SetOutPath "$INSTDIR"
  File "${APP_EXE}"
  File "..\README.md"

  WriteUninstaller "$INSTDIR\Uninstall.exe"

  CreateDirectory "$SMPROGRAMS\${APP_NAME}"
  CreateShortcut "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk" \
      "$INSTDIR\${APP_EXE_NAME}" "" "$INSTDIR\${APP_EXE_NAME}" 0

  WriteRegStr HKCU "Software\${APP_NAME}" "InstallDir" "$INSTDIR"

  ; Add/Remove Programs.
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayName" "${APP_NAME}"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayVersion" "${APP_VERSION}"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayIcon" "$INSTDIR\${APP_EXE_NAME}"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "UninstallString" "$\"$INSTDIR\Uninstall.exe$\""
  WriteRegStr HKCU "${UNINSTALL_KEY}" "QuietUninstallString" \
      "$\"$INSTDIR\Uninstall.exe$\" /S"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "Publisher" "${APP_NAME}"
  WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoRepair" 1

  ; Installing to a fixed location is exactly what makes "start with Windows"
  ; reliable, since the Run entry is an absolute path to this file. The program
  ; repairs a stale entry itself on the next start, but there is no reason to
  ; leave one pointing at the copy the user just replaced.
  ReadRegStr $0 HKCU "${RUN_KEY}" "${APP_NAME}"
  StrCmp $0 "" noautostart
    WriteRegStr HKCU "${RUN_KEY}" "${APP_NAME}" \
        "$\"$INSTDIR\${APP_EXE_NAME}$\" --minimized"
  noautostart:
SectionEnd

Section "Uninstall"
  Call un.CloseRunningCopy

  ; The autostart entry has to go, or Windows keeps trying to launch a file
  ; that is no longer there.
  DeleteRegValue HKCU "${RUN_KEY}" "${APP_NAME}"

  Delete "$INSTDIR\${APP_EXE_NAME}"
  Delete "$INSTDIR\README.md"
  Delete "$INSTDIR\DisplayMirror.log"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"

  Delete "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk"
  RMDir "$SMPROGRAMS\${APP_NAME}"

  DeleteRegKey HKCU "${UNINSTALL_KEY}"
  ; The saved display pair and checkboxes. Removing the program should not
  ; leave its settings behind.
  DeleteRegKey HKCU "Software\${APP_NAME}"
SectionEnd
