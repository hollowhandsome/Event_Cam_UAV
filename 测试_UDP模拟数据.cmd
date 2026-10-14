@echo off
setlocal EnableExtensions
cd /d "%~dp0"
where python >nul 2>nul
if not errorlevel 1 (
  python tools\send_test_uevt.py --host 127.0.0.1 --base-port 50000 --slots 4
  exit /b %errorlevel%
)
where py >nul 2>nul
if not errorlevel 1 (
  py -3 tools\send_test_uevt.py --host 127.0.0.1 --base-port 50000 --slots 4
  exit /b %errorlevel%
)
echo Python 3 was not found. Install Python or use the real UEVT sender.
pause
exit /b 1
