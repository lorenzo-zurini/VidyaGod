; ---------------------------------------------------------------------------
; vidyagod.nsi — NSIS installer for the Windows build of VidyaGod.
;
; Packages the portable dist/ produced by tools/windeploy.sh into a single Setup.exe: installs the app,
; creates Start-Menu + Desktop shortcuts, writes an uninstaller, and handles the external prerequisites
; that can't live inside the app folder:
;   * WinFsp  (REQUIRED — the union filesystem's kernel driver). If a WinFsp MSI is staged at
;              <DISTDIR>\prereq\winfsp.msi it is installed silently when WinFsp is absent; otherwise the
;              user is pointed at winfsp.dev.
;   * Sandboxie-Plus (OPTIONAL — host isolation). Only mentioned; a launch degrades to unsandboxed
;              without it.
;
; Build (on Windows, from the repo root):
;   makensis /DDISTDIR=<path-to-dist> /DOUTFILE=<path-to-VidyaGodSetup.exe> tools\vidyagod.nsi
; ---------------------------------------------------------------------------

!ifndef DISTDIR
  !error "define DISTDIR (the windeploy.sh output dir) with /DDISTDIR=..."
!endif
!ifndef OUTFILE
  !define OUTFILE "VidyaGodSetup.exe"
!endif

!include "MUI2.nsh"

Name "VidyaGod"
OutFile "${OUTFILE}"
Unicode True
InstallDir "$PROGRAMFILES64\VidyaGod"
InstallDirRegKey HKLM "Software\VidyaGod" "InstallDir"
RequestExecutionLevel admin

!define MUI_ABORTWARNING
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Section "VidyaGod (required)" SecApp
  SectionIn RO
  SetOutPath "$INSTDIR"
  ; The whole portable distribution (VidyaGod.exe, vidyagodfs.exe, libvgipfs.dll, Qt runtime + plugins,
  ; all MinGW-w64 runtime DLLs).
  File /r "${DISTDIR}\*.*"

  ; --- WinFsp prerequisite (the FS kernel driver) ---
  ; Present if its user-mode DLL exists. If absent, install the bundled MSI silently when available.
  IfFileExists "$PROGRAMFILES32\WinFsp\bin\winfsp-x64.dll" winfsp_done 0
    IfFileExists "$INSTDIR\prereq\winfsp.msi" 0 winfsp_missing
      DetailPrint "Installing WinFsp (required kernel driver)..."
      ExecWait 'msiexec /i "$INSTDIR\prereq\winfsp.msi" /qn'
      Goto winfsp_done
    winfsp_missing:
      MessageBox MB_ICONEXCLAMATION "WinFsp is required and was not found. Install it from https://winfsp.dev before launching VidyaGod."
  winfsp_done:

  ; Shortcuts
  CreateDirectory "$SMPROGRAMS\VidyaGod"
  CreateShortcut  "$SMPROGRAMS\VidyaGod\VidyaGod.lnk" "$INSTDIR\VidyaGod.exe"
  CreateShortcut  "$DESKTOP\VidyaGod.lnk"             "$INSTDIR\VidyaGod.exe"

  ; Registry (install dir + Add/Remove Programs)
  WriteRegStr HKLM "Software\VidyaGod" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VidyaGod" "DisplayName" "VidyaGod"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VidyaGod" "UninstallString" "$INSTDIR\uninstall.exe"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VidyaGod" "DisplayIcon" "$INSTDIR\VidyaGod.exe"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VidyaGod" "Publisher" "VidyaGod"
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VidyaGod" "NoModify" 1
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VidyaGod" "NoRepair" 1
  WriteUninstaller "$INSTDIR\uninstall.exe"

  ; Sandboxie (game isolation) is an OPTIONAL external dependency — install Sandboxie-Plus separately.
  ; Informational only; VidyaGod runs unsandboxed if it is absent.
  IfFileExists "$PROGRAMFILES64\Sandboxie-Plus\Start.exe" sbie_done 0
    DetailPrint "Note: install Sandboxie-Plus (https://sandboxie-plus.com) to enable game isolation; without it launches run unsandboxed."
  sbie_done:
SectionEnd

Section "Uninstall"
  Delete "$SMPROGRAMS\VidyaGod\VidyaGod.lnk"
  RMDir  "$SMPROGRAMS\VidyaGod"
  Delete "$DESKTOP\VidyaGod.lnk"
  RMDir /r "$INSTDIR"
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VidyaGod"
  DeleteRegKey HKLM "Software\VidyaGod"
SectionEnd
