@echo off
set "PATH=C:\Users\khalu\AppData\Local\Tools\w64devkit\w64devkit\bin;%PATH%"
gcc --version >nul 2>&1
if errorlevel 1 (
    echo gcc not found in PATH
    exit /b 1
)
echo gcc found, building...
cd /d "%~dp0"
C:\Users\khalu\AppData\Local\Tools\w64devkit\w64devkit\bin\mingw32-make.exe all 2>&1
if errorlevel 1 (
    echo BUILD FAILED
    exit /b 1
)
echo Build OK, launching preview...
build\iso_preview.exe
