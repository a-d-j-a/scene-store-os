cd /workspaces/scene-store-os/iso
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
echo CMD_CYCLE_9_DONE
