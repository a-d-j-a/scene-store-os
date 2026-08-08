@echo off
set PATH=C:\Users\khalu\AppData\Local\Tools\w64devkit\w64devkit\bin;%PATH%
cd /d C:\Users\khalu\Desktop\iso\scene-store
gcc -O2 -o build/ppm_info.exe tools/ppm_info.c 2>&1 && build\ppm_info.exe preview.ppm 2>&1
