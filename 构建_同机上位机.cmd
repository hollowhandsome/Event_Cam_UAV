@echo off
setlocal EnableExtensions
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSROOT="
if exist "%VSWHERE%" (
  for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%I"
)
if not defined VSROOT (
  echo Microsoft MSVC C++ Build Tools were not found.
  echo Install Visual Studio with Desktop development with C++.
  pause
  exit /b 1
)

set "VCPKG_TOOLCHAIN=%VSROOT%\VC\vcpkg\scripts\buildsystems\vcpkg.cmake"
if not exist "%VCPKG_TOOLCHAIN%" (
  echo Visual Studio vcpkg component was not found.
  echo Modify Visual Studio and install the vcpkg package manager component.
  pause
  exit /b 1
)

call "%VSROOT%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64
if errorlevel 1 (
  echo Failed to initialize the x64 MSVC build environment.
  pause
  exit /b 1
)

if not exist "C:\Program Files (x86)\DvsenseDriver\lib\DvsenseDriver.lib" (
  echo DVSense SDK was not found in the default installation directory.
  pause
  exit /b 1
)

set "BUILD_DRIVE="
for %%D in (R S T U V W X Y Z) do if not exist %%D:\ (
  set "BUILD_DRIVE=%%D:"
  goto :drive_found
)
:drive_found
if not defined BUILD_DRIVE (
  echo No free drive letter is available for the Unicode-path build workaround.
  pause
  exit /b 1
)
subst %BUILD_DRIVE% "%CD%"
if errorlevel 1 (
  echo Failed to create the temporary build drive.
  pause
  exit /b 1
)

echo Building the C# WPF user interface...
dotnet build "upper_host\ui\EventCameraViewer.UI.csproj" -c Release
if errorlevel 1 goto :failed

echo Configuring the C++ camera core...
cmake -S "%BUILD_DRIVE%\upper_host\native" -B "%BUILD_DRIVE%\upper_host\native\build-msvc-opencv" -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="%VCPKG_TOOLCHAIN%" -DVCPKG_TARGET_TRIPLET=x64-windows -DVCPKG_INSTALLED_DIR="%BUILD_DRIVE%/upper_host/native/vcpkg_installed" -DEVENTCORE_OUTPUT_DIR="%BUILD_DRIVE%/upper_host/ui/bin/Release/net8.0-windows"
if errorlevel 1 goto :failed

echo Building the C++ camera core...
cmake --build "%BUILD_DRIVE%\upper_host\native\build-msvc-opencv"
if errorlevel 1 goto :failed

subst %BUILD_DRIVE% /d
echo.
echo Build completed successfully.
pause
exit /b 0

:failed
if defined BUILD_DRIVE subst %BUILD_DRIVE% /d
echo.
echo Build failed. Review the error messages above.
pause
exit /b 1
