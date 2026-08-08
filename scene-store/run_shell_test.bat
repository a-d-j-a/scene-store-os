@echo off
set PATH=C:\Users\khalu\AppData\Local\Tools\w64devkit\w64devkit\bin;%PATH%
cd /d C:\Users\khalu\Desktop\iso\scene-store
echo === Running tests ===
build\test_shell.exe 2>&1
echo === Shell tests done ===
