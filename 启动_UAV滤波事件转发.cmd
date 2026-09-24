@echo off
setlocal
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\start_live.ps1" -Mode filtered
set "RESULT=%ERRORLEVEL%"
echo.
pause
exit /b %RESULT%
