@echo off
set "PATH=C:\Program Files\Git\cmd;C:\Program Files\GitHub CLI;C:\Program Files\Git\usr\bin;%PATH%"
cd /d "C:\Users\khalu\Desktop\iso"
git add iso/build.sh iso/.devcontainer/devcontainer.json
git commit -m "Fix ISO build script: use meson, all-in-ram rootfs, grub-mkrescue, check_deps"
git push origin master