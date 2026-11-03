@echo off
rem VidyaGodFS-on-WinFsp check, run on a Windows machine in the user's INTERACTIVE session (vmrun.sh does that):
rem a Sandboxie box started from an SSH logon runs as ANONYMOUS LOGON and never starts its program.
rem Needs C:\vg\vidyagodfs.exe, WinFsp, Sandboxie-Plus, C:\vgcheck\probe.exe (exits 16 with no arguments) and
rem C:\vgcheck\pkg.zip (STORE: pkg/probe.exe, pkg/data.txt). fs_check.sh puts them there and runs this.
rem Prints one PASS/FAIL line per check; the exit code is the number of failed checks.
setlocal EnableDelayedExpansion
set PATH=C:\vg;C:\Program Files (x86)\WinFsp\bin;%PATH%
set SB=C:\Program Files\Sandboxie-Plus
set T=C:\vgcheck\t
set FAILS=0
taskkill /f /im vidyagodfs.exe >nul 2>&1
rem the box is defined before anything names it: Start.exe answers an unknown box with a dialog, and waits
"%SB%\SbieIni.exe" set VGCheck Enabled y
rem as a launch grants it (SandboxLayer::BoxIniCommands): the mountpoint itself; without it the box kills a program
rem at its first file access in the mount. Each boxed run first empties the box: a start while the last program's box
rem still winds down (milliseconds apart, as here; a relaunch is seconds apart) is killed (0x40010004).
"%SB%\SbieIni.exe" set VGCheck OpenFilePath %T%\m
"%SB%\Start.exe" /box:VGCheck /silent /terminate >nul 2>&1
rmdir /s /q %T% 2>nul
mkdir %T%\lower\sub %T%\wl
echo lowerdata> %T%\lower\sub\a.txt
copy /y C:\vgcheck\probe.exe %T%\lower\sub\ >nul
> %T%\spec.json echo {"mountpoint":"%T:\=/%/m","uid":1000,"gid":1000,"readonly":false,"writelayer":"%T:\=/%/wl","layers":[{"type":"dir","source":"%T:\=/%/lower","target":""},{"type":"zip","source":"C:/vgcheck/pkg.zip","target":"z"}]}
rem the full path: the job runs in a folder of its own, and cmd looks there before PATH
start "" /b C:\vg\vidyagodfs.exe %T%\spec.json %T%\m > %T%\fs.out 2> %T%\fs.err
for /l %%i in (1,1,50) do if not exist %T%\m\sub ping -n 1 -w 100 127.0.0.1 >nul
if not exist %T%\m\sub (echo FAIL mount did not appear & type %T%\fs.err & exit /b 99)

dir %T%\m | findstr /r /c:" [1-9][0-9,.]* bytes free" >nul 2>&1
call :check "the mount reports free space" "!ERRORLEVEL!==0"
mkdir %T%\m\newdir 2>nul
call :check "mkdir in the mount" "exist %T%\wl\newdir"
(echo new)> %T%\m\newdir\x.txt 2>nul
call :check "create in a new folder" "exist %T%\wl\newdir\x.txt"
(echo new)> %T%\m\sub\y.txt 2>nul
call :check "create in a lower folder" "exist %T%\wl\sub\y.txt"
(echo more)>> %T%\m\sub\a.txt 2>nul
findstr /c:"more" %T%\wl\sub\a.txt >nul 2>&1
call :check "append copies the lower file up" "!ERRORLEVEL!==0"
%T%\m\sub\probe.exe >nul 2>&1
call :check "a program in a folder layer runs" "!ERRORLEVEL!==16"
"%SB%\Start.exe" /box:VGCheck /silent /terminate
"%SB%\Start.exe" /box:VGCheck /silent /wait %T%\m\sub\probe.exe
call :check "a boxed program runs from the mount" "!ERRORLEVEL!==16"
type %T%\m\Z\PKG\DATA.TXT >nul 2>&1
call :check "a zipped file is found in another case" "!ERRORLEVEL!==0"
(echo new)> %T%\m\z\pkg\new.txt 2>nul
call :check "create in a zip folder" "exist %T%\wl\z\pkg\new.txt"
(echo edited)> %T%\m\z\pkg\data.txt 2>nul
findstr /c:"edited" %T%\wl\z\pkg\data.txt >nul 2>&1
call :check "overwrite copies the zipped file up" "!ERRORLEVEL!==0"
"%SB%\Start.exe" /box:VGCheck /silent /terminate
"%SB%\Start.exe" /box:VGCheck /silent /wait %T%\m\z\pkg\probe.exe
call :check "a boxed program runs from a zip" "!ERRORLEVEL!==16"

taskkill /f /im vidyagodfs.exe >nul 2>&1
"%SB%\Start.exe" /box:VGCheck /silent /terminate >nul 2>&1
echo failed checks: %FAILS%
exit /b %FAILS%

:check
set RC=%ERRORLEVEL%
if %~2 (echo PASS %~1) else (echo FAIL %~1 [exit code %RC%] & set /a FAILS+=1)
exit /b 0
