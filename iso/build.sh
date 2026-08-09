#!/bin/sh
# ISO Build System — fully custom Linux, no Debian base.
# Builds: musl, kernel, busybox, scene-store (DRM/KMS compositor) → bootable ISO.
# Run on any Linux with gcc and make (e.g. Ubuntu Codespace).
#   ./build.sh          (full build)
#   ./build.sh clean    (nuke everything)
#
# Design notes (why the list is short):
#   - The desktop compositor (iso-drm) talks to the kernel directly via
#     linux/drm.h UAPI ioctls + evdev. No libdrm, no wayland, no wlroots,
#     no mesa, no meson cross-toolchain — that whole class of cross-build
#     failure is gone. Wayland/wlroots stay an alternative backend in the
#     scene-store source tree, not a build dependency.
#   - All-in-RAM boot: the initramfs IS the root filesystem (full copy of
#     the sysroot), so the desktop comes up without touching a disk.
set -e

# ---- configuration --------------------------------------------------------
JOBS="$(nproc 2>/dev/null || echo 4)"
TOPDIR="$(pwd)/build"
SYSROOT="$TOPDIR/sysroot"
SRC="$TOPDIR/src"
BUILDDIR="$TOPDIR/builddir"
OUTPUT="$(pwd)/output"

KVER="6.6.52"
MUSLVER="1.2.5"
BUSYBOXVER="1.38.0"

INITRD="$TOPDIR/initramfs-${KVER}.cpio.gz"

# ---- helpers ---------------------------------------------------------------
msg()  { printf '\033[1;32m>>> %s\033[0m\n' "$*"; }
warn() { printf '\033[1;33m!!! %s\033[0m\n' "$*"; }
die()  { printf '\033[1;31mERR %s\033[0m\n' "$*" >&2; exit 1; }

fetch() {
    local url="$1" dst="$2"
    [ -f "$dst" ] && return 0
    msg "Downloading $(basename "$dst")"
    curl -fL --retry 3 -o "$dst" "$url" || \
    wget -O "$dst" "$url" || \
    die "Cannot download $url"
}

extract() {
    local archive="$1" destdir="$2"
    [ -d "$destdir" ] && return 0
    msg "Extracting $(basename "$archive")"
    mkdir -p "$destdir"
    case "$archive" in
        *.tar.xz)  tar -xf "$archive" -C "$destdir" --strip-components=1 ;;
        *.tar.gz)  tar -xzf "$archive" -C "$destdir" --strip-components=1 ;;
        *.tar.bz2) tar -xjf "$archive" -C "$destdir" --strip-components=1 ;;
        *) die "Unknown archive format: $archive" ;;
    esac
}

# ---- musl toolchain --------------------------------------------------------
MUSL_GCC=""
setup_musl_gcc() {
    if [ -x "$SYSROOT/bin/musl-gcc" ]; then
        MUSL_GCC="$SYSROOT/bin/musl-gcc"
    else
        die "musl-gcc not found in $SYSROOT — run: $0 musl"
    fi
}

# ---- phase 0: install host prerequisites -----------------------------------
install_prereqs() {
    msg "=== Phase 0: Installing host prerequisites ==="
    apt-get update -qq 2>/dev/null || true
    apt-get install -y -qq \
        gcc make flex bison bc libelf-dev \
        cpio gzip xz-utils \
        xorriso grub-pc-bin grub-common mtools \
        2>/dev/null || true
    msg "Host prerequisites installed."
}

# ---- phase 1: download sources --------------------------------------------
fetch_sources() {
    msg "=== Phase 1: Fetching sources ==="
    mkdir -p "$SRC"

    fetch "https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-${KVER}.tar.xz" \
          "$SRC/linux-${KVER}.tar.xz"
    fetch "https://musl.libc.org/releases/musl-${MUSLVER}.tar.gz" \
          "$SRC/musl-${MUSLVER}.tar.gz"
    fetch "https://busybox.net/downloads/busybox-${BUSYBOXVER}.tar.bz2" \
          "$SRC/busybox-${BUSYBOXVER}.tar.bz2"

    msg "All sources fetched."
}

# ---- phase 2: build musl ---------------------------------------------------
build_musl() {
    msg "=== Phase 2: Building musl ==="
    extract "$SRC/musl-${MUSLVER}.tar.gz" "$BUILDDIR/musl-${MUSLVER}"
    mkdir -p "$SYSROOT"
    cd "$BUILDDIR/musl-${MUSLVER}"
    ./configure --prefix=/usr
    make -j"$JOBS" || die "musl build failed"
    make install DESTDIR="$SYSROOT" || die "musl install failed"
    cd -

    # musl-gcc wrapper: clean, idempotent, no specs dependency.
    # Kernel headers are installed by headers_install to $SYSROOT/include
    # (a later phase); the -I path just needs to exist at compile time.
    mkdir -p "$SYSROOT/bin"
    cat > "$SYSROOT/bin/musl-gcc" <<WRAPPER
#!/bin/sh
exec gcc --sysroot="$SYSROOT" -I"$SYSROOT/usr/include" -I"$SYSROOT/include" -L"$SYSROOT/lib" -static "\$@"
WRAPPER
    chmod +x "$SYSROOT/bin/musl-gcc"

    [ -x "$SYSROOT/bin/musl-gcc" ] || die "musl-gcc wrapper not created"
    msg "musl done."
}

# ---- phase 3: build kernel -------------------------------------------------
build_kernel() {
    msg "=== Phase 3: Building kernel ==="
    extract "$SRC/linux-${KVER}.tar.xz" "$BUILDDIR/linux-${KVER}"
    cd "$BUILDDIR/linux-${KVER}"
    make defconfig

    # Video output
    scripts/config --enable DRM
    scripts/config --enable DRM_NOUVEAU
    scripts/config --enable DRM_AMDGPU
    scripts/config --enable DRM_I915
    scripts/config --enable DRM_BOCHS        # QEMU std/vga test display
    scripts/config --enable DRM_VIRTIO_GPU   # QEMU virtio display
    scripts/config --enable FB
    scripts/config --enable FB_SIMPLE
    scripts/config --enable DRM_FBDEV_EMULATION
    scripts/config --enable FRAMEBUFFER_CONSOLE
    scripts/config --enable VT
    scripts/config --enable VT_CONSOLE

    # Devices: devtmpfs, devpts, tmpfs, unix/inet
    scripts/config --enable DEVTMPFS
    scripts/config --enable DEVPTS_FS
    scripts/config --enable TMPFS
    scripts/config --enable UNIX
    scripts/config --enable INET

    # Storage: SATA / SCSI / NVMe / USB mass storage
    scripts/config --enable BLK_DEV_SD
    scripts/config --enable SCSI
    scripts/config --enable ATA
    scripts/config --enable ATA_SFF
    scripts/config --enable SATA_AHCI
    scripts/config --enable ATA_PIIX
    scripts/config --enable NVME_CORE
    scripts/config --enable BLK_DEV_NVME
    scripts/config --enable USB_STORAGE

    # Input: evdev, PS/2 keyboard+mouse, USB HID
    scripts/config --enable INPUT_EVDEV
    scripts/config --enable INPUT_KEYBOARD
    scripts/config --enable KEYBOARD_ATKBD
    scripts/config --enable INPUT_MOUSE
    scripts/config --enable MOUSE_PS2
    scripts/config --enable HID
    scripts/config --enable HID_GENERIC
    scripts/config --enable USB_HID
    scripts/config --enable SERIO
    scripts/config --enable SERIO_I8042

    # Filesystems / misc
    scripts/config --enable EXT4_FS
    scripts/config --enable VFAT_FS
    scripts/config --enable OVERLAY_FS
    scripts/config --enable SQUASHFS
    scripts/config --enable SQUASHFS_ZSTD
    scripts/config --disable SECURITY
    scripts/config --disable DEBUG_INFO

    make olddefconfig
    make -j"$JOBS" || die "kernel build failed"
    make headers_install INSTALL_HDR_PATH="$SYSROOT" || die "headers_install failed"
    make modules_install INSTALL_MOD_PATH="$SYSROOT" || die "modules_install failed"
    mkdir -p "$SYSROOT/boot"
    cp arch/x86/boot/bzImage "$SYSROOT/boot/vmlinuz-${KVER}"
    cd -
    msg "Kernel done."
}

# ---- phase 4: build busybox ------------------------------------------------
build_busybox() {
    msg "=== Phase 4: Building busybox ==="
    setup_musl_gcc
    extract "$SRC/busybox-${BUSYBOXVER}.tar.bz2" "$BUILDDIR/busybox-${BUSYBOXVER}"
    cd "$BUILDDIR/busybox-${BUSYBOXVER}"
    make mrproper 2>/dev/null || true
    make defconfig
    sed -i 's/# CONFIG_STATIC is not set/CONFIG_STATIC=y/' .config
    sed -i 's/# CONFIG_FEATURE_SH_STANDALONE is not set/CONFIG_FEATURE_SH_STANDALONE=y/' .config
    sed -i 's/# CONFIG_FEATURE_MOUNT_LOOP is not set/CONFIG_FEATURE_MOUNT_LOOP=y/' .config
    sed -i 's/# CONFIG_FEATURE_MOUNT_LOOP_CREATE is not set/CONFIG_FEATURE_MOUNT_LOOP_CREATE=y/' .config
    sed -i "s|CONFIG_PREFIX=.*|CONFIG_PREFIX=\"$SYSROOT\"|" .config
    yes "" | make oldconfig
    make CC="$MUSL_GCC" HOSTCC="gcc" -j"$JOBS" || die "busybox build failed"
    make CC="$MUSL_GCC" HOSTCC="gcc" install || die "busybox install failed"
    cd -
    [ -x "$SYSROOT/bin/busybox" ] || die "busybox not installed"
    msg "busybox done."
}

# ---- phase 5: build scene-store (engine + DRM compositor) ------------------
build_scene_store() {
    msg "=== Phase 5: Building scene-store ==="
    setup_musl_gcc
    local SSRC="$(cd "$(dirname "$0")/.." && pwd)/scene-store"
    [ -d "$SSRC" ] || die "scene-store source not found at $SSRC"
    cd "$SSRC"
    make build/iso_drm CC="$MUSL_GCC" \
        CFLAGS="-std=c11 -Wall -Wextra -O2 -Iinclude" || die "iso_drm build failed"
    mkdir -p "$SYSROOT/usr/bin"
    cp build/iso_drm "$SYSROOT/usr/bin/iso-drm"
    cd -
    msg "scene-store done."
}

# ---- phase 6: assemble rootfs ----------------------------------------------
assemble_rootfs() {
    msg "=== Phase 6: Assembling rootfs ==="
    local R="$SYSROOT"

    mkdir -p "$R"/{proc,sys,dev,run,tmp,var,etc,home/user,mnt,usr/bin,usr/lib,usr/share}
    mkdir -p "$R"/dev/{pts,shm}
    mkdir -p "$R"/etc/init.d
    mkdir -p "$R"/root

    cat > "$R/etc/hostname" <<'EOF'
iso
EOF
    cat > "$R/etc/hosts" <<'EOF'
127.0.0.1   localhost iso
::1         localhost iso
EOF
    cat > "$R/etc/passwd" <<'EOF'
root:x:0:0:root:/root:/bin/sh
user:x:1000:1000:user:/home/user:/bin/sh
EOF
    cat > "$R/etc/group" <<'EOF'
root:x:0:
user:x:1000:
audio:x:29:
video:x:44:
input:x:999:
tty:x:5:
EOF
    cat > "$R/etc/shadow" <<'EOF'
root::0:99999:7:::
user::0:99999:7:::
EOF
    cat > "$R/etc/resolv.conf" <<'EOF'
nameserver 8.8.8.8
nameserver 1.1.1.1
EOF
    cat > "$R/etc/inittab" <<'INITTAB'
::sysinit:/etc/init.d/rcS
tty1::respawn:/bin/login -f user
tty2::respawn:/bin/login -f user
tty3::respawn:/bin/login -f user
::ctrlaltdel:/sbin/reboot
::shutdown:/bin/umount -a -r
INITTAB

    cat > "$R/etc/init.d/rcS" <<'RCS'
#!/bin/sh
export PATH=/usr/bin:/bin:/sbin:/usr/sbin
mount -t proc     proc     /proc
mount -t sysfs    sysfs    /sys
mount -t devtmpfs devtmpfs /dev
mount -t tmpfs    tmpfs    /run
mount -t tmpfs    tmpfs    /tmp
mkdir -p /dev/pts /dev/shm
mount -t devpts   devpts   /dev/pts
hostname -F /etc/hostname 2>/dev/null || true
ip link set lo up 2>/dev/null || true
ip addr add 127.0.0.1/8 dev lo 2>/dev/null || true
mkdir -p /run/user/0 /run/user/1000
chown 0:0 /run/user/0
chown 1000:1000 /run/user/1000
chmod 700 /run/user/0 /run/user/1000
echo "ISO Linux starting scene desktop..."
exec /etc/init.d/scene-desktop
RCS
    chmod +x "$R/etc/init.d/rcS"

    cat > "$R/etc/init.d/scene-desktop" <<'SCENE'
#!/bin/sh
export HOME=/home/user
export USER=user
export PATH=/usr/bin:/bin:/sbin:/usr/sbin
export TERM=xterm-256color
chvt 7 2>/dev/null || chvt 1 2>/dev/null || true
if [ -x /usr/bin/iso-drm ]; then
    echo "Starting iso-drm compositor..."
    exec /usr/bin/iso-drm
fi
echo "No compositor found. Dropping to shell."
exec /bin/sh
SCENE
    chmod +x "$R/etc/init.d/scene-desktop"

    # Default shell theme
    cat > "$R/etc/shell.conf" <<'CONF'
bg_color=0xFF1A1A2E
panel_color=0xFF16213E
panel_height=32
panel_radius=4
button_color=0xFF2A2A4E
button_border=0xFF555555
button_text=0xFFE8E8E8
hover_color=0xFF2A2A4E
label_text=0xFFE8E8E8
menu_color=0xFF1F1F3A
menu_border=0xFF444466
menu_item_color=0xFF2A2A4E
menu_item_text=0xFFE8E8E8
clock_12h=0
CONF

    # Overlay (hand-written boot scripts, may override the above)
    cp -a "$(dirname "$0")/overlay/"* "$R/" 2>/dev/null || true
    msg "Rootfs assembled."
}

# ---- phase 7: create initramfs (the all-in-RAM rootfs) ---------------------
build_initramfs() {
    msg "=== Phase 7: Building initramfs ==="
    local TMP="$TOPDIR/initramfs_tmp"
    rm -rf "$TMP"
    mkdir -p "$TMP"

    # Full rootfs copy: busybox install + musl + kernel modules + /etc + /usr
    cp -a "$SYSROOT/." "$TMP/"
    # Drop build-only artifacts
    rm -rf "$TMP/usr/include" "$TMP/boot" "$TMP/usr/lib/pkgconfig" \
           "$TMP/usr/share/man" 2>/dev/null || true

    cat > "$TMP/init" <<'INIT'
#!/bin/sh
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
mount -t proc     proc     /proc
mount -t sysfs    sysfs    /sys
mount -t devtmpfs devtmpfs /dev
mount -t tmpfs    tmpfs    /run
mkdir -p /dev/pts
mount -t devpts   devpts   /dev/pts
echo "ISO Linux booting..."
exec /sbin/init
INIT
    chmod +x "$TMP/init"

    cd "$TMP"
    find . -print0 | cpio --null -ov --format=newc 2>/dev/null | gzip -9 > "$INITRD"
    cd -
    rm -rf "$TMP"
    msg "initramfs done: $INITRD"
}

# ---- phase 8: build ISO -----------------------------------------------------
build_iso() {
    msg "=== Phase 8: Building ISO ==="
    mkdir -p "$OUTPUT"
    local ISO="$OUTPUT/iso-custom-${KVER}.iso"
    local ISOROOT="$TOPDIR/iso_root"
    rm -rf "$ISOROOT"
    mkdir -p "$ISOROOT/boot/grub"

    cp "$SYSROOT/boot/vmlinuz-${KVER}" "$ISOROOT/boot/"
    cp "$INITRD" "$ISOROOT/boot/"

    cat > "$ISOROOT/boot/grub/grub.cfg" <<GRUB
set timeout=3
set default=0

menuentry "ISO Linux (All-in-RAM)" {
    linux /boot/vmlinuz-${KVER} rw quiet console=tty1
    initrd /boot/initramfs-${KVER}.cpio.gz
}

menuentry "ISO Linux (Safe Mode)" {
    linux /boot/vmlinuz-${KVER} rw nomodeset console=tty1
    initrd /boot/initramfs-${KVER}.cpio.gz
}
GRUB

    if command -v grub-mkrescue >/dev/null 2>&1; then
        grub-mkrescue -o "$ISO" "$ISOROOT" || die "grub-mkrescue failed"
    else
        die "No ISO creation tool found. Install xorriso + grub-pc-bin."
    fi

    msg "ISO created: $ISO"
    ls -lh "$ISO"
}

# ---- main -------------------------------------------------------------------
case "${1:-}" in
    clean)
        msg "Cleaning build tree..."
        rm -rf "$TOPDIR" "$OUTPUT"
        msg "Done."
        exit 0
        ;;
    prereqs)   install_prereqs ;;
    fetch)     fetch_sources ;;
    musl)      install_prereqs; fetch_sources; build_musl ;;
    kernel)    install_prereqs; fetch_sources; build_musl; build_kernel ;;
    busybox)   install_prereqs; fetch_sources; build_musl; build_kernel; build_busybox ;;
    scene)     build_scene_store ;;
    rootfs)    assemble_rootfs ;;
    initramfs) build_initramfs ;;
    iso)       build_iso ;;
    all|"")
        msg "Starting full ISO build..."
        install_prereqs
        fetch_sources
        build_musl
        build_kernel
        build_busybox
        build_scene_store
        assemble_rootfs
        build_initramfs
        build_iso
        msg "=== BUILD COMPLETE ==="
        msg "ISO: $OUTPUT/iso-custom-${KVER}.iso"
        ;;
    *)
        echo "Usage: $0 [all|clean|prereqs|fetch|musl|kernel|busybox|scene|rootfs|initramfs|iso]"
        exit 1
        ;;
esac
