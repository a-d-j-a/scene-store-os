cd /workspaces/scene-store-os/iso
pkill -f qemu-system; sleep 2
rm -f /tmp/serial.log /tmp/scr2.png
sudo bash build.sh iso
qemu-system-x86_64 -m 1024 -vga std -smp 2 -cdrom output/iso-custom-6.6.52.iso -boot d -display none -monitor unix:/tmp/qmon.sock,server,nowait -serial file:/tmp/serial.log -daemonize
sleep 120
echo "screendump /tmp/scr2.png" | socat - unix-connect:/tmp/qmon.sock
cp /tmp/serial.log /tmp/scr2.png /workspaces/scene-store-os/
echo CMD_CYCLE_DONE
