@echo off
set "PATH=C:\Program Files\Git\cmd;C:\Program Files\GitHub CLI;C:\Program Files\Git\usr\bin;C:\Windows\System32\OpenSSH;%PATH%"
cd /d "C:\Users\khalu\Desktop\iso"
git add -A
git commit -m "fix build.sh: correct versions, musl-gcc toolchain, meson cross file, busybox config"
git push origin master
