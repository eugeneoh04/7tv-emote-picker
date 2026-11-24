@echo off
rem Builds "7TV Emote Picker.exe" with Visual Studio's C++ compiler (the free Build Tools are enough).
rem   build.cmd        the picker (run it afterwards: it replaces the copy that's running)
rem   build.cmd test   also build\render_test.exe, which draws the picker offscreen into PNGs
setlocal enableextensions
cd /d "%~dp0"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VS="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS (
  echo Can't find Visual Studio's C++ build tools.
  exit /b 1
)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 || exit /b 1
if not exist build\obj mkdir build\obj
if not exist build\test mkdir build\test

set CFLAGS=/nologo /std:c++20 /O2 /GL /EHsc /W4 /permissive- /MT /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN /Ithird_party\libwebp\src
set LIBS=build\webp.lib d2d1.lib d3d11.lib dxgi.lib dwrite.lib windowscodecs.lib winhttp.lib dwmapi.lib imm32.lib user32.lib ole32.lib

rem libwebp's decoder (third_party\libwebp, for the WebP previews) is compiled once into
rem build\webp.lib. Delete that file to compile it again.
if not exist build\webp.lib (
  if not exist build\webp mkdir build\webp
  cl /nologo /c /MP /O2 /MT /W0 /DNDEBUG /D_CRT_SECURE_NO_WARNINGS /DWIN32_LEAN_AND_MEAN /Ithird_party\libwebp /Fobuild\webp\ third_party\libwebp\src\dec\*.c third_party\libwebp\src\demux\*.c third_party\libwebp\src\dsp\*.c third_party\libwebp\src\utils\*.c || exit /b 1
  lib /nologo /out:build\webp.lib build\webp\*.obj || exit /b 1
)

rc /nologo /fo build\app.res src\app.rc || exit /b 1
cl %CFLAGS% /Fobuild\obj\ src\*.cpp build\app.res /Fe:build\picker.exe /link /LTCG /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTINPUT:src\app.manifest %LIBS% || exit /b 1

rem A running picker's file can't be overwritten, but it can be renamed out of the way.
del /q build\old-*.exe 2>nul
if exist "7TV Emote Picker.exe" move /y "7TV Emote Picker.exe" "build\old-%RANDOM%.exe" >nul
copy /y build\picker.exe "7TV Emote Picker.exe" >nul || exit /b 1
echo Built "7TV Emote Picker.exe"

if /i not "%~1"=="test" exit /b 0
cl %CFLAGS% /Fobuild\test\ /Isrc tests\render_test.cpp src\app_*.cpp src\net.cpp src\images.cpp src\clipboard.cpp src\textbox.cpp /Fe:build\render_test.exe /link /LTCG /SUBSYSTEM:CONSOLE %LIBS% gdi32.lib shell32.lib || exit /b 1
echo Built build\render_test.exe
