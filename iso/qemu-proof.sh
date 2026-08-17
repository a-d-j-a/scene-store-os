#!/bin/sh
# qemu-proof.sh [shot] [disk=FILE] [cdrom=FILE] [audio=FILE] [extra kernel args...]
# Boot the shipped kernel + initramfs in QEMU with a user-net NIC and
# capture the serial log to serial-qemu.log. Used to prove the
# daily-driver chain: kernel boots -> NIC driver -> udhcpc lease ->
# networking script runs -> pkgtest=PKG apk installs and runs.
#
# Mode "shot": boots with -vga std, waits for the desktop, takes a
# screendump (/tmp/qemu-shot.ppm) via the stdio monitor, then quits.
# disk=FILE: attach FILE as a raw virtio disk and append persist=/dev/vda
#   (proves the persistence path: apk installs survive a reboot).
# cdrom=FILE: boot from FILE as a CD-ROM instead of -kernel/-initrd
#   (proves the GRUB ISO path; kernel args come from grub.cfg).
# audio=FILE: capture guest audio into FILE as WAV via an Intel HDA
#   hda-duplex codec (proves the raw-ALSA path in iso-play).
set -eu
cd "$(dirname "$0")/.."
K=$(pwd)/build/sysroot/boot/vmlinuz-6.6.52
I=$(pwd)/build/initramfs-6.6.52.cpio.gz

SHOT=0
DISK=""
ISO_CD=""
AUDIO=""
EXTRA=""
for a in "$@"; do
    case "$a" in
        shot)     SHOT=1 ;;
        disk=*)   DISK="${a#disk=}" ;;
        cdrom=*)  ISO_CD="${a#cdrom=}" ;;
        audio=*)  AUDIO="${a#audio=}" ;;
        *)        EXTRA="$EXTRA $a" ;;
    esac
done
set -- $EXTRA

AUDIO_ARGS=""
[ -n "$AUDIO" ] && AUDIO_ARGS="-audiodev wav,path=$AUDIO -device intel-hda -device hda-duplex"

APPEND="console=ttyS0 loglevel=7 $*"
[ -n "$DISK" ] && APPEND="$APPEND persist=/dev/vda"
echo "boot: $APPEND"

DRIVE_ARGS=""
[ -n "$DISK" ] && DRIVE_ARGS="-drive file=$DISK,if=virtio,format=raw,media=disk"

if [ "$SHOT" = 1 ]; then
    if [ -n "$ISO_CD" ]; then
        (sleep 80; echo screendump /tmp/qemu-shot.ppm; echo quit) \
            | timeout 120 qemu-system-x86_64 -m 512 \
                -cdrom "$ISO_CD" \
                $DRIVE_ARGS $AUDIO_ARGS \
                -netdev user,id=n1 -device e1000,netdev=n1 \
                -display none -vga std \
                -no-reboot -serial file:/tmp/qemu-shot.log -monitor stdio \
            || true
    else
        (sleep 80; echo screendump /tmp/qemu-shot.ppm; echo quit) \
            | timeout 120 qemu-system-x86_64 -m 512 \
                -kernel "$K" -initrd "$I" \
                -append "$APPEND" \
                $DRIVE_ARGS $AUDIO_ARGS \
                -netdev user,id=n1 -device e1000,netdev=n1 \
                -display none -vga std \
                -no-reboot -serial file:/tmp/qemu-shot.log -monitor stdio \
            || true
    fi
    echo "--- serial tail ---"
    tail -15 /tmp/qemu-shot.log || true
    echo "--- screenshot ---"
    ls -la /tmp/qemu-shot.ppm || true
    cp /tmp/qemu-shot.log serial-shot.log
    return 0 2>/dev/null || exit 0
fi

if [ -n "$ISO_CD" ]; then
    timeout 150 qemu-system-x86_64 -m 512 \
        -cdrom "$ISO_CD" \
        $DRIVE_ARGS $AUDIO_ARGS \
        -netdev user,id=n1 -device e1000,netdev=n1 \
        -nographic -no-reboot -serial file:/tmp/qemu-proof.log || true
else
    timeout 150 qemu-system-x86_64 -m 512 \
        -kernel "$K" -initrd "$I" \
        -append "$APPEND" \
        $DRIVE_ARGS $AUDIO_ARGS \
        -netdev user,id=n1 -device e1000,netdev=n1 \
        -nographic -no-reboot -serial file:/tmp/qemu-proof.log || true
fi
echo "--- serial proof lines ---"
grep -aE 'udhcpc|lease of|pkgtest|installing|OK:|persist:' /tmp/qemu-proof.log | tail -20 || true
cp /tmp/qemu-proof.log serial-qemu.log
echo "log: serial-qemu.log ($(wc -l < serial-qemu.log) lines)"
