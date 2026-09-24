@echo off
setlocal
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\start_dual_raw_playback.ps1" -RawFile1 "%~1" -RawFile2 "%~2" -PlaybackSpeed "%~3"
set "RESULT=%ERRORLEVEL%"
echo.
pause
exit /b %RESULT%
