@echo off
set "PATH=C:\Program Files\Git\cmd;C:\Program Files\GitHub CLI;C:\Program Files\Git\usr\bin;C:\Windows\System32\OpenSSH;%PATH%"
cd /d "C:\Users\khalu\Desktop\iso"
gh codespace ssh -c "effective-system-966q49pg6q642rj4" -- pkill -2 build.sh