@echo off
taskkill /F /IM iso_preview.exe 2>nul
del C:\Users\khalu\Desktop\iso\scene-store\build\dbg.txt 2>nul
cd /d C:\Users\khalu\Desktop\iso\scene-store\build
start "" iso_preview.exe
ping -n 4 127.0.0.1 >nul
taskkill /F /IM iso_preview.exe 2>nul
