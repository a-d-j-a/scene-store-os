@echo off
set PATH=C:\Users\khalu\AppData\Local\Tools\w64devkit\w64devkit\bin;%PATH%
cd /d C:\Users\khalu\Desktop\iso\scene-store
mingw32-make.exe clean 2>&1
mingw32-make.exe all 2>&1
echo ALL_EXIT=%ERRORLEVEL%
mingw32-make.exe build/preview_dump.exe 2>&1
echo DUMP_EXIT=%ERRORLEVEL%
mingw32-make.exe build/iso_preview.exe 2>&1
echo PREVIEW_EXIT=%ERRORLEVEL%
if exist wallpaper.bmp copy wallpaper.bmp build\wallpaper.bmp >nul 2>&1
