@echo off
set PATH=C:\Users\khalu\AppData\Local\Tools\w64devkit\w64devkit\bin;%PATH%
cd /d C:\Users\khalu\Desktop\iso\scene-store
gcc -std=c11 -Wall -Wextra -g -O0 -Iinclude src/iso_preview.c build/scene_fmt.o build/scene_store.o build/scene_transport.o build/scene_client.o build/scene_server.o build/scene_fb.o build/scene_font.o build/scene_font_data.o build/scene_compositor.o build/scene_a11y.o build/scene_shell.o build/scene_wallpaper.o build/scene_image.o -lws2_32 -lgdi32 -o build/iso_preview.exe 2>&1
echo EXITCODE=%ERRORLEVEL%
if %ERRORLEVEL%==0 (
  copy build\wallpaper.bmp build\iso_preview_wallpaper.bmp >nul
  copy build\wallpaper.bmp wallpaper.bmp >nul
)
