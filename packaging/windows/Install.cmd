@echo off
setlocal
title ISO Recorder 0.1.0

set "SRC=%~dp0"
set "DEST=%ProgramData%\obs-studio\plugins\iso-recorder"

echo.
echo   ISO Recorder 0.1.0
echo   One file per source, while you stream.
echo.
echo   Installing to:
echo     %DEST%
echo.

if not exist "%DEST%\bin\64bit" mkdir "%DEST%\bin\64bit"
if not exist "%DEST%\data\locale" mkdir "%DEST%\data\locale"
if errorlevel 1 goto failed

copy /y "%SRC%iso-recorder.dll" "%DEST%\bin\64bit\iso-recorder.dll" >nul
if errorlevel 1 goto failed

> "%DEST%\data\locale\en-US.ini" echo iso-recorder="ISO Recorder"

echo   Installed.
echo.
echo   Start OBS Studio, then tick "ISO Recorder" in the Docks menu to show
echo   the panel.
echo.

if exist "%ProgramFiles%\obs-studio\bin\64bit\obs64.exe" goto foundobs
if exist "%ProgramFiles(x86)%\obs-studio\bin\64bit\obs64.exe" goto foundobs

echo   Note: OBS Studio was not found in the usual place. If yours lives
echo   somewhere else, the panel may not appear.
echo.
goto done

:foundobs
echo   OBS Studio found. Restart it if it was already running.
echo.

:done
pause
exit /b 0

:failed
echo.
echo   Could not write to:
echo     %DEST%
echo.
echo   Close this window, right-click Install.cmd and choose
echo   "Run as administrator", then try again.
echo.
pause
exit /b 1
