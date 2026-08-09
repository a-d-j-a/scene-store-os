@echo off
set PATH=C:\Program Files\Git\cmd;C:\Program Files\GitHub CLI;C:\Program Files\Git\usr\bin;%PATH%
cd /d C:\Users\khalu\Desktop\iso
git push origin master
echo Push completed with status %ERRORLEVEL%