@echo off
setlocal
call "%~dp0windows\build.bat" %*
if errorlevel 1 exit /b 1
if not exist "%~dp0build" mkdir "%~dp0build"
if exist "%~dp0windows\build\Zpix.exe" copy /y "%~dp0windows\build\Zpix.exe" "%~dp0build\Zpix.exe" >nul
if exist "%~dp0windows\build\ZpixSetup.exe" copy /y "%~dp0windows\build\ZpixSetup.exe" "%~dp0build\ZpixSetup.exe" >nul
endlocal