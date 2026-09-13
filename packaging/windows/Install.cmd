@echo off
setlocal
title ISO Recorder

set "SRC=%~dp0"
set "DEST=%ProgramData%\obs-studio\plugins\iso-recorder"

echo.
echo   ISO Recorder
echo   One file per source, while you stream.
echo.
echo   Installing to:
echo     %DEST%
echo.

tasklist /FI "IMAGENAME eq obs64.exe" 2>nul | find /I "obs64.exe" >nul
if not errorlevel 1 goto obsrunning

if not exist "%DEST%\bin\64bit" mkdir "%DEST%\bin\64bit"
if not exist "%DEST%\data\locale" mkdir "%DEST%\data\locale"

copy /y "%SRC%iso-recorder.dll" "%DEST%\bin\64bit\iso-recorder.dll" >nul
if errorlevel 1 goto failed

> "%DEST%\data\locale\en-US.ini" echo iso-recorder="ISO Recorder"

for %%A in ("%SRC%iso-recorder.dll") do set "WANT=%%~zA"
for %%A in ("%DEST%\bin\64bit\iso-recorder.dll") do set "GOT=%%~zA"

if not "%WANT%"=="%GOT%" goto shortwrite

echo   Installed %GOT% bytes.
echo.
goto done

:shortwrite
echo   The file that landed is %GOT% bytes, but the one shipped is %WANT%.
echo   Something stopped the copy part way. Close OBS Studio and run this
echo   again.
echo.
pause
exit /b 1

:obsrunning
echo   OBS Studio is running. Windows will not let this replace a plugin that
echo   is already loaded, so the old version would stay in place and the panel
echo   would look unchanged.
echo.
echo   Close OBS Studio completely, then run this again.
echo.
pause
exit /b 1

:done
if exist "%ProgramFiles%\obs-studio\bin\64bit\obs64.exe" goto foundobs
if exist "%ProgramFiles(x86)%\obs-studio\bin\64bit\obs64.exe" goto foundobs

echo   Note: OBS Studio was not found in the usual place. If yours lives
echo   somewhere else, the panel may not appear.
echo.
goto end

:foundobs
echo   Start OBS Studio, then tick "ISO Recorder" in the Docks menu to show
echo   the panel.
echo.
echo   If OBS was already open when you ran this, close it and open it again -
echo   a running OBS keeps the old plugin until it restarts.
echo.

:end
pause
exit /b 0

:failed
echo.
echo   Could not write to:
echo     %DEST%
echo.
echo   Close OBS Studio if it is open, then close this window, right-click
echo   Install.cmd and choose "Run as administrator", and try again.
echo.
pause
exit /b 1
