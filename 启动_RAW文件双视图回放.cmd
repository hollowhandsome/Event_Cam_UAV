@echo off
setlocal
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\start_raw_playback.ps1" -RawFile "%~1" -PlaybackSpeed "%~2"
set "RESULT=%ERRORLEVEL%"
echo.
pause
exit /b %RESULT%
