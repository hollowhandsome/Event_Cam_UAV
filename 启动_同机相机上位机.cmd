@echo off
setlocal EnableExtensions
cd /d "%~dp0"
set "APP=%~dp0upper_host\ui\bin\Release\net8.0-windows\EventCameraViewer.UI.exe"
set "CORE=%~dp0upper_host\ui\bin\Release\net8.0-windows\EventCore.dll"
if not exist "%APP%" (
  echo The C# WPF application has not been built.
  echo Run the local upper-host build script first.
  pause
  exit /b 1
)
if not exist "%CORE%" (
  echo EventCore.dll has not been built.
  echo Install MSVC C++ Build Tools, then run the build script.
  pause
  exit /b 1
)
set "PATH=C:\Program Files (x86)\DvsenseDriver\bin;%PATH%"
start "DVSLume local upper host" "%APP%"
