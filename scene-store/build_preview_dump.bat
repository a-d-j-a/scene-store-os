@echo off
set PATH=C:\Users\khalu\AppData\Local\Tools\w64devkit\w64devkit\bin;%PATH%
cd /d C:\Users\khalu\Desktop\iso\scene-store
mingw32-make.exe build/preview_dump.exe 2>&1
echo EXITCODE=%ERRORLEVEL%
