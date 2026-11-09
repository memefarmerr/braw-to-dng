@echo off
rem Builds braw_to_dng.exe on Windows using whichever C++ toolchain is available:
rem a portable w64devkit install, or a Visual Studio C++ environment.
setlocal

set "W64DEVKIT=%LOCALAPPDATA%\braw-tools\w64devkit\bin"
if exist "%W64DEVKIT%\g++.exe" goto mingw

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto notoolchain
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH goto notoolchain
if not exist "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" goto notoolchain

where cmake >nul 2>&1
if errorlevel 1 goto nocmake

echo Configuring with Visual Studio C++ tools from "%VSPATH%"
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 goto failed
cmake -B build -A x64
if errorlevel 1 goto failed
cmake --build build --config Release
if errorlevel 1 goto failed
goto done

:mingw
set "PATH=%W64DEVKIT%;%PATH%"
echo Configuring with MinGW-w64 from "%W64DEVKIT%"
cmake -B build -G "MinGW Makefiles" -DCMAKE_CXX_COMPILER=g++ -DCMAKE_MAKE_PROGRAM=mingw32-make
if errorlevel 1 goto failed
cmake --build build
if errorlevel 1 goto failed
goto done

:notoolchain
echo No C++ toolchain was found.
echo Install Visual Studio Build Tools with "Desktop development with C++", or download
echo w64devkit from https://github.com/skeeto/w64devkit/releases and extract it to
echo "%LOCALAPPDATA%\braw-tools\w64devkit".
exit /b 1

:nocmake
echo CMake was not found on PATH. Install it from https://cmake.org/download/
exit /b 1

:failed
echo Build failed.
exit /b 1

:done
echo.
echo Built %CD%\build\braw_to_dng.exe
endlocal
