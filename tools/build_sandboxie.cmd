@echo off
REM ===========================================================================
REM build_sandboxie.cmd — build the VENDORED Sandboxie (external/Sandboxie submodule) into the minimal
REM binary set VidyaGod bundles for its Windows isolation backend:
REM     Start.exe  SbieSvc.exe  SbieDll.dll  SbieDrv.sys  SbieIni.exe  KmdUtil.exe
REM
REM This is a NATIVE MSVC/WDK build — it does NOT use the MinGW toolchain the rest of VidyaGod builds
REM with (Sandboxie ships MSVC .sln/.vcxproj and a kernel driver). PREREQUISITES:
REM   * Visual Studio 2022 (or Build Tools) with the "Desktop development with C++" workload
REM   * Windows Driver Kit (WDK) matching your SDK  — required for the SbieDrv.sys kernel driver
REM   * To DISTRIBUTE: an EV/attestation code-signing certificate to sign SbieDrv.sys (a kernel driver
REM     will not load on x64 Windows otherwise). For LOCAL testing only: bcdedit /set testsigning on.
REM Run from a "x64 Native Tools Command Prompt for VS 2022" so msbuild + the toolchain are on PATH.
REM
REM   usage:  tools\build_sandboxie.cmd [x64|ARM64]      (default: x64)
REM   output: staging\Sandboxie\   (tools\windeploy.sh copies this into dist\Sandboxie\ beside the exe)
REM ===========================================================================
setlocal
set ARCH=%1
if "%ARCH%"=="" set ARCH=x64
set ROOT=%~dp0..
set SBIE=%ROOT%\external\Sandboxie\Sandboxie
set OUT=%ROOT%\staging\Sandboxie

where msbuild >nul 2>nul || (echo ERROR: msbuild not found. Open a "x64 Native Tools Command Prompt for VS 2022". & exit /b 1)
if not exist "%SBIE%\Sandbox.sln" (echo ERROR: submodule missing. Run: git submodule update --init external/Sandboxie & exit /b 1)

echo == Building Sandboxie core (user-mode: Start / SbieSvc / SbieDll / SbieIni) ==
msbuild "%SBIE%\Sandbox.sln"    /m /p:Configuration=Release /p:Platform=%ARCH% || exit /b 1
echo == Building Sandboxie kernel driver (SbieDrv.sys — requires the WDK) ==
msbuild "%SBIE%\SandboxDrv.sln" /m /p:Configuration=Release /p:Platform=%ARCH% || exit /b 1

echo == Staging the minimal binary set -^> %OUT% ==
if not exist "%OUT%" mkdir "%OUT%"
set BIN=%SBIE%\Bin\%ARCH%\SbieRelease
for %%F in (Start.exe SbieSvc.exe SbieDll.dll SbieDrv.sys SbieIni.exe KmdUtil.exe) do (
    if exist "%BIN%\%%F" (copy /y "%BIN%\%%F" "%OUT%\" >nul) else (echo WARNING: %BIN%\%%F not built)
)
echo.
echo Done -^> %OUT%
echo   * SIGN SbieDrv.sys before distributing (kernel driver signing requirement).
echo   * The VidyaGod installer (tools\vidyagod.nsi) registers + starts the SbieSvc service from the
echo     bundled Sandboxie folder, which loads the driver; sandboxlayer_win.cpp then drives Start.exe.
endlocal
