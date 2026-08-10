#!/bin/bash
set -x
cd /workspaces/scene-store-os
git pull --rebase origin master || exit 1
echo "=== pulling done ==="
rm -f ../scene-store/build/*.o
sudo bash iso/build.sh scene || exit 1
sudo bash iso/build.sh rootfs || exit 1
sudo bash iso/build.sh initramfs || exit 1
sudo bash iso/build.sh iso || exit 1
echo "=== build done ==="
pkill -f qemu-system; sleep 2
rm -f /tmp/serial.log /tmp/scr4.png
qemu-system-x86_64 -m 1024 -vga std -smp 2 -cdrom output/iso-custom-6.6.52.iso -boot d -display none -monitor unix:/tmp/qmon.sock,server,nowait -serial file:/tmp/serial.log -daemonize
sleep 120
echo "screendump /tmp/scr4.png" | socat - unix-connect:/tmp/qmon.sock
cp /tmp/serial.log /tmp/scr4.png /workspaces/scene-store-os/
echo "=== qemu done ==="
