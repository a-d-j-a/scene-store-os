#!/bin/sh
# qemu-proof.sh [extra kernel args...]
# Boot the shipped kernel + initramfs in QEMU with a user-net NIC and
# capture the serial log to serial-qemu.log. Used to prove the
# daily-driver chain: kernel boots -> NIC driver -> udhcpc lease ->
# networking script runs -> pkgtest=PKG apk installs and runs.
set -eu
cd "$(dirname "$0")/.."
K=$(pwd)/build/sysroot/boot/vmlinuz-6.6.52
I=$(pwd)/build/initramfs-6.6.52.cpio.gz
APPEND="console=ttyS0 loglevel=7 $*"
echo "boot: $APPEND"
timeout 150 qemu-system-x86_64 -m 512 \
    -kernel "$K" -initrd "$I" \
    -append "$APPEND" \
    -netdev user,id=n1 -device e1000,netdev=n1 \
    -nographic -no-reboot -serial file:/tmp/qemu-proof.log || true
echo "--- serial proof lines ---"
grep -aE 'udhcpc|lease of|pkgtest|installing|OK:' /tmp/qemu-proof.log | tail -20 || true
cp /tmp/qemu-proof.log serial-qemu.log
echo "log: serial-qemu.log ($(wc -l < serial-qemu.log) lines)"
