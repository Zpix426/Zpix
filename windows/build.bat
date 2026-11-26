@echo off
setlocal
pushd "%~dp0"
rem Zpix Windows build script - MinGW-w64 (w64devkit / MSYS2)

set DEFAULT_TC=C:\Users\61Tao\tools\w64devkit
if exist "%DEFAULT_TC%\bin\g++.exe" (
  set PATH=%DEFAULT_TC%\bin;%PATH%
)
where g++.exe >nul 2>nul
if errorlevel 1 (
  echo [ERROR] g++.exe not found in PATH or %DEFAULT_TC%\bin
  popd
  exit /b 1
)

set SRC=src\main.cpp src\util.cpp src\config.cpp src\annotate.cpp src\translate.cpp src\actions.cpp src\settings.cpp src\ui.cpp src\overlay.cpp src\pin.cpp src\ocr_paddle.cpp src\select_translate.cpp src\img_translate.cpp src\img_search.cpp
set LIBS=-lgdi32 -luser32 -lshell32 -lole32 -luuid -lmsimg32 -lgdiplus -lwinhttp -loleaut32 -lshlwapi -ldwmapi -loleacc
set SETUP_LIBS=-lgdi32 -luser32 -lshell32 -lole32 -luuid -lshlwapi -ladvapi32 -lgdiplus -ldwmapi -loleaut32
set FLAGS=-std=c++17 -DUNICODE -D_UNICODE -D_stdcall=__stdcall -Isrc -Ithird_party\ort\include -mwindows

if /i "%1"=="debug" (
  echo [build] Mode: Debug
  set OPT=-g -O0
) else (
  echo [build] Mode: Release
  set OPT=-O2 -s
)

if not exist build mkdir build

windres -c 65001 src\icon.rc -O coff -o build\icon.o
if errorlevel 1 exit /b 1

echo [build] Compiling Zpix.exe...
g++ %FLAGS% %OPT% %SRC% build\icon.o -o build\Zpix.exe %LIBS%
if errorlevel 1 exit /b 1
copy /y src\ocr.ps1 build\ocr.ps1 >nul
copy /y src\setup_payload.rc build\setup_payload.rc >nul
echo [build] OK -^> windows\build\Zpix.exe

if exist build\model\onnxruntime.dll (
  echo [build] Packaging ZpixSetup.exe...
  windres -c 65001 build\setup_payload.rc -O coff -o build\setup_payload.o
  if errorlevel 1 exit /b 1
  g++ -std=c++17 -DUNICODE -D_UNICODE -D_stdcall=__stdcall %OPT% -mwindows src\setup.cpp build\setup_payload.o -o build\ZpixSetup.exe %SETUP_LIBS%
  if errorlevel 1 exit /b 1
  echo [build] OK -^> windows\build\ZpixSetup.exe
) else (
  echo [build] Skipped ZpixSetup.exe ^(build\model not present^)
)
popd
endlocal