cd /workspaces/scene-store-os
echo "===== CODESPACE STATE ====="
git log --oneline -3
echo "--- sysroot/etc/shell.conf ---"
cat sysroot/etc/shell.conf 2>/dev/null || echo "NO SYSROOT SHELL.CONF"
echo "--- sysroot/usr/bin ---"
ls -la sysroot/usr/bin/ 2>/dev/null
echo "--- build objects (mtimes) ---"
ls -la scene-store/build/*.o 2>/dev/null | awk '{print $6, $7, $8, $9}'
echo "--- scene-store git status ---"
git status --short
echo "===== BUILD ====="
cd iso
sudo bash build.sh scene
sudo bash build.sh rootfs
sudo bash build.sh initramfs
sudo bash build.sh iso
pkill -f qemu-system; sleep 2
rm -f /tmp/serial.log /tmp/scr4.png
qemu-system-x86_64 -m 1024 -vga std -smp 2 -cdrom output/iso-custom-6.6.52.iso -boot d -display none -monitor unix:/tmp/qmon.sock,server,nowait -serial file:/tmp/serial.log -daemonize
sleep 120
echo "screendump /tmp/scr4.png" | socat - unix-connect:/tmp/qmon.sock
cp /tmp/serial.log /tmp/scr4.png /workspaces/scene-store-os/
echo CMD_CYCLE_12_DONE
# cycle-12 re-push nudge
