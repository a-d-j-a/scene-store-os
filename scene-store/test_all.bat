@echo off
set "PATH=C:\Users\khalu\AppData\Local\Tools\w64devkit\w64devkit\bin;%PATH%"
cd /d "C:\Users\khalu\Desktop\iso\scene-store"
set EXE=
echo === store ===
build\test_store.exe
echo === client ===
build\test_client.exe
echo === compositor ===
build\test_compositor.exe
echo === automation ===
build\test_automation.exe
echo === a11y ===
build\test_a11y.exe
echo === rewind ===
build\test_rewind.exe
echo === shell ===
build\test_shell.exe
echo === app ===
build\test_app.exe
echo === terminal ===
build\test_terminal.exe
echo === settings ===
build\test_settings.exe
echo === theme ===
build\test_theme.exe
echo === image ===
build\test_image.exe
echo === wallpaper ===
build\test_wallpaper.exe
