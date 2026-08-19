#!/bin/sh
# install-proof.sh MODE - QEMU proofs for the installer + lock/chrome chain.
#
# Modes:
#   shot     - ISO boot (kernel+initrd, autolaunch=iso-terminal): screendump
#              of the settled desktop (window chrome pixels), Super+L lock
#              screendump (0xFF0A0A14 backdrop), Enter unlock screendump,
#              clean powerdown. Three PPMs: dshot.ppm, lshot.ppm, ushot.ppm
#   install  - ISO boot with persist=/dev/vda (state) + /dev/vdb (target)
#              + autolaunch=iso-install installto=/dev/vdb: the installer
#              partitions/formats/copies/installs grub on vdb (serial proves
#              every step), screendump at the end, clean powerdown. The
#              installed disk is then checked HOST-side (mount partition 1,
#              verify /boot/grub/grub.cfg + /usr/bin/iso-terminal).
#   diskboot - boot the INSTALLED vdb.img as an IDE disk, BIOS -> grub MBR
#              -> kernel + initramfs from the disk's /boot -> persist=auto
#              switches root into /dev/sda1 -> desktop + iso-terminal.
#              Screendump pixel proof of the full standalone chain.
#   q35      - ISO boot on the q35 machine with virtio disks + a virtio-blk
#              persist disk (modern machine path).
#
# Outputs: serial shot logs + PPMs under iso/../proof/ (build dir parent).
set -eu
cd "$(dirname "$0")/.."
K=$(pwd)/build/sysroot/boot/vmlinuz-6.6.52
I=$(pwd)/build/initramfs-6.6.52.cpio.gz
P=$(pwd)/proof
mkdir -p "$P"

MODE="${1:-shot}"
SLEEP_BOOT="${SLEEP_BOOT:-75}"   # seconds until the desktop settles
MON=""

case "$MODE" in
shot)
    MON="(sleep $SLEEP_BOOT; echo screendump $P/dshot.ppm;
          sleep 2; echo sendkey meta_l; sleep 1; echo sendkey l;
          sleep 6; echo screendump $P/lshot.ppm;
          sleep 1; echo sendkey ret;
          sleep 6; echo screendump $P/ushot.ppm;
          sleep 2; echo powerdown)"
    eval "$MON" | timeout 200 qemu-system-x86_64 -m 512 \
        -kernel "$K" -initrd "$I" \
        -append "console=ttyS0 loglevel=7 autolaunch=iso-terminal" \
        -netdev user,id=n1 -device e1000,netdev=n1 \
        -display none -vga std -no-reboot -serial file:$P/shot-serial.log \
        -monitor stdio || true
    cp "$P/shot-serial.log" serial-shot.log
    echo "== shot done: dshot/lshot/ushot.ppm + shot-serial.log"
    ;;
install)
    rm -f "$P/target.vdb" "$P/state.vda"
    truncate -s 4G "$P/target.vdb"
    truncate -s 4G "$P/state.vda"
    (sleep 640; echo screendump $P/inst.ppm; sleep 2; echo powerdown) | \
        timeout 700 qemu-system-x86_64 -m 512 \
        -kernel "$K" -initrd "$I" \
        -append "console=ttyS0 loglevel=7 autolaunch=iso-install installto=/dev/vdb persist=/dev/vda" \
        -drive file=$P/state.vda,if=virtio,format=raw,media=disk \
        -drive file=$P/target.vdb,if=virtio,format=raw,media=disk \
        -netdev user,id=n1 -device e1000,netdev=n1 \
        -display none -vga std -no-reboot -serial file:$P/inst-serial.log \
        -monitor stdio || true
    cp "$P/inst-serial.log" serial-inst.log
    echo "== install proof lines =="
    grep -aE 'iso-install:|persist:|app .*joined|installto' "$P/inst-serial.log" | tail -30
    ;;
diskboot)
    MON="(sleep $SLEEP_BOOT; echo screendump $P/dboot.ppm; sleep 2; echo powerdown)"
    eval "$MON" | timeout 200 qemu-system-x86_64 -m 512 \
        -drive file=$P/target.vdb,if=ide,format=raw,media=disk \
        -boot order=c \
        -netdev user,id=n1 -device e1000,netdev=n1 \
        -display none -vga std -no-reboot -serial file:$P/dboot-serial.log \
        -monitor stdio || true
    cp "$P/dboot-serial.log" serial-dboot.log
    echo "== diskboot proof lines =="
    grep -aE 'ISO Linux|persist:|login|scene|gio|vmlinuz|initrd|Grub|grub|error' "$P/dboot-serial.log" | tail -25
    ;;
q35)
    rm -f "$P/q35.vda"
    truncate -s 4G "$P/q35.vda"
    (sleep $SLEEP_BOOT; echo screendump $P/q35shot.ppm; sleep 2; echo powerdown) | \
        timeout 200 qemu-system-x86_64 -m 512 -machine q35 \
        -kernel "$K" -initrd "$I" \
        -append "console=ttyS0 loglevel=7 autolaunch=iso-terminal persist=/dev/vda" \
        -drive file=$P/q35.vda,if=virtio,format=raw,media=disk \
        -netdev user,id=n1 -device e1000,netdev=n1 \
        -display none -vga std -no-reboot -serial file:$P/q35-serial.log \
        -monitor stdio || true
    cp "$P/q35-serial.log" serial-q35.log
    echo "== q35 proof lines =="
    grep -aE 'persist:|app .*joined|welcome|scene' "$P/q35-serial.log" | tail -15
    ;;
*)
    echo "usage: install-proof.sh [shot|install|diskboot|q35]"
    exit 1
    ;;
esac