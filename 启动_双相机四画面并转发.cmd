@echo off
setlocal
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\start_dual_live.ps1" -EnableNetwork
set "RESULT=%ERRORLEVEL%"
echo.
pause
exit /b %RESULT%
