@echo off
setlocal EnableExtensions
cd /d "%~dp0"
set "APP=%~dp0upper_host\ui\bin\Release\net8.0-windows\EventCameraUdpViewer.UI.exe"
set "CORE=%~dp0upper_host\ui\bin\Release\net8.0-windows\EventCore.dll"
if not exist "%APP%" (
  echo The UDP upper-host application has not been built.
  echo Run the UDP build script first.
  pause
  exit /b 1
)
if not exist "%CORE%" (
  echo EventCore.dll has not been built.
  echo Run the UDP build script first.
  pause
  exit /b 1
)
start "DVSLume UDP upper host" "%APP%"
