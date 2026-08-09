@echo off
set "PATH=C:\Program Files\Git\cmd;C:\Program Files\GitHub CLI;C:\Program Files\Git\usr\bin;%PATH%"
cd /d "C:\Users\khalu\Desktop\iso"
rem Fix mesa version
powershell -Command "(Get-Content iso/build.sh) -replace 'MESAVER=""23.3.3""', 'MESAVER=""23.1.5""' | Set-Content iso/build.sh"
rem Fix mesa URL
powershell -Command "(Get-Content iso/build.sh) -replace 'https://mesa.freedesktop.org/archive/mesa-\${MESAVER}/mesa-\${MESAVER}.tar.xz', 'https://archive.mesa3d.org/mesa-\${MESAVER}/mesa-\${MESAVER}.tar.xz' | Set-Content iso/build.sh"