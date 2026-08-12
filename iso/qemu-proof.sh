#!/bin/sh
# qemu-proof.sh [shot] [extra kernel args...]
# Boot the shipped kernel + initramfs in QEMU with a user-net NIC and
# capture the serial log to serial-qemu.log. Used to prove the
# daily-driver chain: kernel boots -> NIC driver -> udhcpc lease ->
# networking script runs -> pkgtest=PKG apk installs and runs.
#
# Mode "shot": boots with -vga std, waits for the desktop, takes a
# screendump (/tmp/qemu-shot.ppm) via the stdio monitor, then quits.
set -eu
cd "$(dirname "$0")/.."
K=$(pwd)/build/sysroot/boot/vmlinuz-6.6.52
I=$(pwd)/build/initramfs-6.6.52.cpio.gz

SHOT=0
case "${1:-}" in
    shot) SHOT=1; shift ;;
esac

APPEND="console=ttyS0 loglevel=7 $*"
echo "boot: $APPEND"

if [ "$SHOT" = 1 ]; then
    (sleep 45; echo screendump /tmp/qemu-shot.ppm; echo quit) \
        | timeout 100 qemu-system-x86_64 -m 512 \
            -kernel "$K" -initrd "$I" \
            -append "$APPEND" \
            -netdev user,id=n1 -device e1000,netdev=n1 \
            -display none -vga std \
            -no-reboot -serial file:/tmp/qemu-shot.log -monitor stdio \
        || true
    echo "--- serial tail ---"
    tail -15 /tmp/qemu-shot.log || true
    echo "--- screenshot ---"
    ls -la /tmp/qemu-shot.ppm || true
    cp /tmp/qemu-shot.log serial-shot.log
    return 0 2>/dev/null || exit 0
fi

timeout 150 qemu-system-x86_64 -m 512 \
    -kernel "$K" -initrd "$I" \
    -append "$APPEND" \
    -netdev user,id=n1 -device e1000,netdev=n1 \
    -nographic -no-reboot -serial file:/tmp/qemu-proof.log || true
echo "--- serial proof lines ---"
grep -aE 'udhcpc|lease of|pkgtest|installing|OK:' /tmp/qemu-proof.log | tail -20 || true
cp /tmp/qemu-proof.log serial-qemu.log
echo "log: serial-qemu.log ($(wc -l < serial-qemu.log) lines)"
