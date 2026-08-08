@echo off
set PATH=C:\Users\khalu\AppData\Local\Tools\w64devkit\w64devkit\bin;%PATH%
cd /d C:\Users\khalu\Desktop\iso\scene-store
echo === test_store ===
build\test_store.exe 2>&1
echo === test_client ===
build\test_client.exe 2>&1
echo === test_compositor ===
build\test_compositor.exe 2>&1
echo === test_automation ===
build\test_automation.exe 2>&1
echo === test_a11y ===
build\test_a11y.exe 2>&1
echo === test_rewind ===
build\test_rewind.exe 2>&1
echo === test_shell ===
build\test_shell.exe 2>&1
echo === test_app ===
build\test_app.exe 2>&1
echo === test_terminal ===
build\test_terminal.exe 2>&1
echo === test_settings ===
build\test_settings.exe 2>&1
echo === test_theme ===
build\test_theme.exe 2>&1
echo === test_image ===
build\test_image.exe 2>&1
echo === test_wallpaper ===
build\test_wallpaper.exe 2>&1
echo === DONE ===
