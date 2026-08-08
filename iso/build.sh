#!/bin/sh
# ISO Build System — fully custom Linux, no Debian base.
# Builds: kernel, musl, busybox, scene-store, wlroots → bootable ISO.
# Run on any Linux with gcc, make, and basic tools.
#   ./build.sh          (full build)
#   ./build.sh clean    (nuke everything)
set -e

# ---- configuration --------------------------------------------------------
JOBS="$(nproc 2>/dev/null || echo 4)"
TOPDIR="$(pwd)/build"
SYSROOT="$TOPDIR/sysroot"
SRC="$TOPDIR/src"
BUILD="$TOPDIR/build"
OUTPUT="$(pwd)/output"

KVER="6.6.52"
MUSLVER="1.2.5"
BUSYBOXVER="1.38.0"
WAYLANDVER="1.22.0"
WLRVER="0.17.4"
PIXMANVER="0.42.2"
LIBDRMVER="2.4.122"
LIBXKBCOMMONVER="1.5.0"
UDEVVER="3.2.14"
LIBSEATVER="0.1.0"
LIBINPUTVER="1.26.0"
MESAVER="23.1.5"

# ---- helpers ---------------------------------------------------------------
msg()  { printf '\033[1;32m>>> %s\033[0m\n' "$*"; }
warn() { printf '\033[1;33m!!! %s\033[0m\n' "$*"; }
die()  { printf '\033[1;31mERR %s\033[0m\n' "$*" >&2; exit 1; }

fetch() {
    local url="$1" dst="$2"
    [ -f "$dst" ] && return 0
    msg "Downloading $(basename "$dst")"
    curl -fL -o "$dst" "$url" || wget -O "$dst" "$url" || die "Cannot download $url"
}

extract() {
    local archive="$1" destdir="$2"
    [ -d "$destdir" ] && return 0
    msg "Extracting $(basename "$archive")"
    mkdir -p "$destdir"
    case "$archive" in
        *.tar.xz) tar -xf "$archive" -C "$destdir" --strip-components=1 ;;
        *.tar.gz) tar -xzf "$archive" -C "$destdir" --strip-components=1 ;;
        *.tar.bz2) tar -xjf "$archive" -C "$destdir" --strip-components=1 ;;
        *) die "Unknown archive format: $archive" ;;
    esac
}

# path to musl-gcc wrapper (created by build_musl)
MUSL_GCC=""
setup_musl_gcc() {
    MUSL_GCC="$SYSROOT/bin/musl-gcc"
    [ -x "$MUSL_GCC" ] || die "musl-gcc not found at $MUSL_GCC"
    export CC="$MUSL_GCC"
    export CFLAGS="--sysroot=$SYSROOT -I$SYSROOT/include"
    export LDFLAGS="--sysroot=$SYSROOT -L$SYSROOT/lib -static"
    export PKG_CONFIG_PATH="$SYSROOT/lib/pkgconfig:$SYSROOT/lib64/pkgconfig"
    export PKG_CONFIG_LIBDIR="$SYSROOT/lib/pkgconfig"
    export PATH="$SYSROOT/bin:$PATH"
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
    fetch "https://gitlab.freedesktop.org/wayland/wayland/-/releases/${WAYLANDVER}/downloads/wayland-${WAYLANDVER}.tar.xz" \
          "$SRC/wayland-${WAYLANDVER}.tar.xz"
    fetch "https://gitlab.freedesktop.org/wlroots/wlroots/-/releases/${WLRVER}/downloads/wlroots-${WLRVER}.tar.gz" \
          "$SRC/wlroots-${WLRVER}.tar.gz"
    fetch "https://www.cairographics.org/releases/pixman-${PIXMANVER}.tar.gz" \
          "$SRC/pixman-${PIXMANVER}.tar.gz"
    fetch "https://dri.freedesktop.org/libdrm/libdrm-${LIBDRMVER}.tar.xz" \
          "$SRC/libdrm-${LIBDRMVER}.tar.xz"
    fetch "https://xkbcommon.org/download/libxkbcommon-${LIBXKBCOMMONVER}.tar.xz" \
          "$SRC/libxkbcommon-${LIBXKBCOMMONVER}.tar.xz"
    fetch "https://github.com/kennylevinsen/seat/releases/download/v${LIBSEATVER}/libseat-${LIBSEATVER}.tar.gz" \
          "$SRC/libseat-${LIBSEATVER}.tar.gz"
    fetch "https://github.com/eudev-project/eudev/releases/download/v${UDEVVER}/eudev-${UDEVVER}.tar.gz" \
          "$SRC/eudev-${UDEVVER}.tar.gz"
    fetch "https://gitlab.freedesktop.org/libinput/libinput/-/releases/${LIBINPUTVER}/downloads/libinput-${LIBINPUTVER}.tar.xz" \
          "$SRC/libinput-${LIBINPUTVER}.tar.xz"
    fetch "https://mesa.freedesktop.org/archive/mesa-${MESAVER}/mesa-${MESAVER}.tar.xz" \
          "$SRC/mesa-${MESAVER}.tar.xz"

    msg "All sources fetched."
}

# ---- phase 2: build musl --------------------------------------------------
build_musl() {
    msg "=== Phase 2: Building musl ==="
    extract "$SRC/musl-${MUSLVER}.tar.gz" "$BUILD/musl-${MUSLVER}"
    mkdir -p "$SYSROOT"
    cd "$BUILD/musl-${MUSLVER}"
    ./configure --prefix=/usr
    make -j"$JOBS" || die "musl build failed"
    # musl-gcc wrapper is created during make, copy it explicitly
    if [ -f "musl-gcc" ]; then
        cp musl-gcc "$SYSROOT/bin/musl-gcc"
        chmod +x "$SYSROOT/bin/musl-gcc"
    fi
    make install DESTDIR="$SYSROOT" || die "musl install failed"
    # verify musl-gcc wrapper exists (installed to usr/bin by musl's make install)
    [ -x "$SYSROOT/usr/bin/musl-gcc" ] || die "musl-gcc wrapper not installed at $SYSROOT/usr/bin/musl-gcc"
    # Also copy to bin for convenience
    cp "$SYSROOT/usr/bin/musl-gcc" "$SYSROOT/bin/musl-gcc" 2>/dev/null || true
    [ -x "$SYSROOT/bin/musl-gcc" ] || die "musl-gcc wrapper not installed at $SYSROOT/bin/musl-gcc"
    cd -
    msg "musl done (musl-gcc at $SYSROOT/bin/musl-gcc)"
}

# ---- phase 3: build busybox ------------------------------------------------
build_busybox() {
    msg "=== Phase 3: Building busybox ==="
    setup_musl_gcc
    extract "$SRC/busybox-${BUSYBOXVER}.tar.bz2" "$BUILD/busybox-${BUSYBOXVER}"
    cd "$BUILD/busybox-${BUSYBOXVER}"
    make mrproper 2>/dev/null || true
    make defconfig
    # enable static build
    sed -i 's/# CONFIG_STATIC is not set/CONFIG_STATIC=y/' .config
    # enable standalone shell for initramfs
    sed -i 's/# CONFIG_FEATURE_SH_STANDALONE is not set/CONFIG_FEATURE_SH_STANDALONE=y/' .config
    sed -i 's/CONFIG_FEATURE_SH_STANDALONE is not set/CONFIG_FEATURE_SH_STANDALONE=y/' .config
    # enable loop mount
    sed -i 's/# CONFIG_FEATURE_MOUNT_LOOP is not set/CONFIG_FEATURE_MOUNT_LOOP=y/' .config
    sed -i 's/CONFIG_FEATURE_MOUNT_LOOP is not set/CONFIG_FEATURE_MOUNT_LOOP=y/' .config
    sed -i 's/# CONFIG_FEATURE_MOUNT_LOOP_CREATE is not set/CONFIG_FEATURE_MOUNT_LOOP_CREATE=y/' .config
    sed -i 's/CONFIG_FEATURE_MOUNT_LOOP_CREATE is not set/CONFIG_FEATURE_MOUNT_LOOP_CREATE=y/' .config
    # set install prefix to sysroot
    sed -i 's|CONFIG_PREFIX=.*|CONFIG_PREFIX="'$SYSROOT'"|' .config
    make -j"$JOBS" || die "busybox build failed"
    make install || die "busybox install failed"
    cd -
    msg "busybox done (installed to $SYSROOT)"
}

# ---- phase 4: build kernel -------------------------------------------------
build_kernel() {
    msg "=== Phase 4: Building kernel ==="
    extract "$SRC/linux-${KVER}.tar.xz" "$BUILD/linux-${KVER}"
    cd "$BUILD/linux-${KVER}"
    make defconfig
    # enable required drivers
    scripts/config --enable DRM
    scripts/config --enable DRM_NOUVEAU
    scripts/config --enable DRM_AMDGPU
    scripts/config --enable DRM_I915
    scripts/config --enable FB
    scripts/config --enable VT
    scripts/config --enable VT_CONSOLE
    scripts/config --enable INPUT_EVDEV
    scripts/config --enable SND_HDA_INTEL
    scripts/config --enable USB_HID
    scripts/config --enable EXT4_FS
    scripts/config --enable VFAT_FS
    scripts/config --enable OVERLAY_FS
    scripts/config --disable SECURITY
    scripts/config --disable DEBUG_INFO
    scripts/config --enable SQUASHFS
    scripts/config --enable SQUASHFS_ZSTD
    make olddefconfig
    make -j"$JOBS" || die "kernel build failed"
    make modules_install INSTALL_MOD_PATH="$SYSROOT"
    mkdir -p "$SYSROOT/boot"
    cp arch/x86/boot/bzImage "$SYSROOT/boot/vmlinuz-${KVER}"
    cd -
    msg "Kernel done."
}

# ---- phase 5: build wayland + deps for wlroots ----------------------------
build_deps() {
    msg "=== Phase 5: Building dependencies ==="
    setup_musl_gcc

    # pixman
    msg "  Building pixman..."
    extract "$SRC/pixman-${PIXMANVER}.tar.gz" "$BUILD/pixman-${PIXMANVER}"
    cd "$BUILD/pixman-${PIXMANVER}"
    ./configure --prefix=/usr --disable-shared --enable-static
    make -j"$JOBS" && make install DESTDIR="$SYSROOT"
    cd -

    # libdrm
    msg "  Building libdrm..."
    extract "$SRC/libdrm-${LIBDRMVER}.tar.xz" "$BUILD/libdrm-${LIBDRMVER}"
    cd "$BUILD/libdrm-${LIBDRMVER}"
    ./configure --prefix=/usr --disable-shared --enable-static
    make -j"$JOBS" && make install DESTDIR="$SYSROOT"
    cd -

    # libxkbcommon
    msg "  Building libxkbcommon..."
    extract "$SRC/libxkbcommon-${LIBXKBCOMMONVER}.tar.xz" "$BUILD/libxkbcommon-${LIBXKBCOMMONVER}"
    cd "$BUILD/libxkbcommon-${LIBXKBCOMMONVER}"
    ./configure --prefix=/usr --disable-shared --enable-static \
        --enable-x11=no --disable-wayland
    make -j"$JOBS" && make install DESTDIR="$SYSROOT"
    cd -

    # wayland (libs only — we hand-compile the protocol)
    msg "  Building wayland..."
    extract "$SRC/wayland-${WAYLANDVER}.tar.xz" "$BUILD/wayland-${WAYLANDVER}"
    cd "$BUILD/wayland-${WAYLANDVER}"
    ./configure --prefix=/usr --disable-shared --enable-static \
        --disable-scanner --disable-documentation
    make -j"$JOBS" && make install DESTDIR="$SYSROOT"
    cd -

    # eudev (device manager)
    msg "  Building eudev..."
    extract "$SRC/eudev-${UDEVVER}.tar.gz" "$BUILD/eudev-${UDEVVER}"
    cd "$BUILD/eudev-${UDEVVER}"
    ./configure --prefix=/usr --disable-shared --enable-static \
        --disable-gudev --disable-introspection \
        --disable-hwdb --disable-manpages
    make -j"$JOBS" && make install DESTDIR="$SYSROOT"
    cd -

    msg "Dependencies done."
}

# ---- phase 6: build wlroots ------------------------------------------------
build_wlroots() {
    msg "=== Phase 6: Building wlroots ==="
    setup_musl_gcc
    extract "$SRC/wlroots-${WLRVER}.tar.gz" "$BUILD/wlroots-${WLRVER}"

    # create meson cross file for musl
    cat > "$BUILD/musl-cross.txt" <<CROSS
[binaries]
c = '$MUSL_GCC'
cpp = '$MUSL_GCC'
ar = 'ar'
strip = 'strip'
pkgconfig = 'pkg-config'

[host_machine]
system = 'linux'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'

[properties]
sys_root = '$SYSROOT'
CFLAGS = ['--sysroot=$SYSROOT']
LDFLAGS = ['--sysroot=$SYSROOT', '-static']
pkg_config_path = ['$SYSROOT/lib/pkgconfig', '$SYSROOT/lib64/pkgconfig']
CROSS
    # meson needs cpp in the cross file
    sed -i "s|^cpp = .*|cpp = '$MUSL_GCC'|" "$BUILD/musl-cross.txt"

    cd "$BUILD/wlroots-${WLRVER}"
    # wlroots uses meson_options.txt / meson.options
    # Override options if the file exists
    if [ -f meson_options.txt ]; then
        sed -i "s|option('examples'.*|option('examples', type: 'boolean', value: false)|" meson_options.txt
        sed -i "s|option('xwayland'.*|option('xwayland', type: 'boolean', value: false)|" meson_options.txt
    fi
    meson setup _build \
        --cross-file "$BUILD/musl-cross.txt" \
        --prefix=/usr \
        -Dexamples=false \
        -Dxwayland=false || \
    meson setup _build \
        --prefix=/usr \
        -Dexamples=false \
        -Dxwayland=false
    ninja -C _build -j"$JOBS" || die "wlroots build failed"
    DESTDIR="$SYSROOT" ninja -C _build install
    cd -
    msg "wlroots done."
}

# ---- phase 7: build scene-store --------------------------------------------
build_scene_store() {
    msg "=== Phase 7: Building scene-store ==="
    setup_musl_gcc
    local SSRC="$(cd "$(dirname "$0")/.." && pwd)/scene-store"
    [ -d "$SSRC" ] || die "scene-store source not found at $SSRC"
    cd "$SSRC"
    make clean 2>/dev/null || true
    # build for Linux (no .exe, no GDI)
    make -j"$JOBS" all \
        CC="$MUSL_GCC" \
        EXTRA_CFLAGS="--sysroot=$SYSROOT -I$SYSROOT/include" \
        EXTRA_LDFLAGS="--sysroot=$SYSROOT -L$SYSROOT/lib -static" \
        EXE_SUFFIX="" \
        PLATFORM=linux \
        || warn "scene-store build had issues (non-fatal for ISO)"
    # install what we can
    mkdir -p "$SYSROOT/usr/bin"
    for bin in iso-compositor iso-preview scene_store scene_client; do
        [ -f "build/${bin}" ] && cp "build/${bin}" "$SYSROOT/usr/bin/$bin"
        [ -f "build/${bin}.exe" ] && cp "build/${bin}.exe" "$SYSROOT/usr/bin/$bin"
    done
    cd -
    msg "scene-store done."
}

# ---- phase 8: assemble rootfs ----------------------------------------------
assemble_rootfs() {
    msg "=== Phase 8: Assembling rootfs ==="
    local R="$SYSROOT"

    mkdir -p "$R"/{proc,sys,dev,run,tmp,var,etc,home/user,mnt,usr/bin,usr/lib,usr/share}
    mkdir -p "$R"/dev/{pts,shm}
    mkdir -p "$R"/etc/init.d
    mkdir -p "$R"/boot/grub
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
    cat > "$R/etc/fstab" <<'EOF'
/dev/sda1   /        ext4   defaults,noatime   0 1
tmpfs       /tmp     tmpfs  defaults,size=256M  0 0
proc        /proc    proc   defaults            0 0
sysfs       /sys     sysfs  defaults            0 0
tmpfs       /run     tmpfs  defaults,size=128M  0 0
devpts      /dev/pts devpts defaults            0 0
EOF
    cat > "$R/etc/profile" <<'PROFILE'
export PATH=/usr/bin:/bin:/sbin:/usr/sbin
export HOME=/home/user
export USER=user
export TERM=xterm-256color
export LANG=en_US.UTF-8
export LC_ALL=en_US.UTF-8
export XDG_RUNTIME_DIR=/run/user/1000
export XDG_SESSION_TYPE=wayland
export WLR_NO_HARDWARE_CURSORS=1
PROFILE
    cat > "$R/home/user/.profile" <<'PROFILE'
. /etc/profile
PROFILE

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
[ -d /sys/kernel/uevent_helpers ] || \
    echo /sbin/udevadm > /proc/sys/kernel/hotplug 2>/dev/null
/sbin/udevd --daemon 2>/dev/null || true
[ -d /sys/class ] && udevadm trigger --action=add 2>/dev/null || true
udevadm settle --timeout=5 2>/dev/null || true
hostname -F /etc/hostname 2>/dev/null || true
ifconfig lo 127.0.0.1 up 2>/dev/null || ip link set lo up 2>/dev/null || true
exec /etc/init.d/scene-desktop
RCS
    chmod +x "$R/etc/init.d/rcS"

    cat > "$R/etc/init.d/scene-desktop" <<'SCENE'
#!/bin/sh
export HOME=/home/user
export USER=user
export XDG_RUNTIME_DIR=/run/user/1000
export XDG_SESSION_TYPE=wayland
export WLR_NO_HARDWARE_CURSORS=1
export WLR_BACKENDS=drm
mkdir -p "$XDG_RUNTIME_DIR"
chown user:user "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
chvt 1 2>/dev/null || true
exec su -c '/usr/bin/iso-compositor' user 2>/dev/null || \
    exec su -c '/usr/bin/iso-preview' user 2>/dev/null || \
    exec /bin/sh
SCENE
    chmod +x "$R/etc/init.d/scene-desktop"

    cp -a "$(dirname "$0")/overlay/"* "$R/" 2>/dev/null || true
    msg "Rootfs assembled."
}

# ---- phase 9: create initramfs ---------------------------------------------
build_initramfs() {
    msg "=== Phase 9: Building initramfs ==="
    local IRD="$SYSROOT/boot/initramfs-${KVER}.cpio.gz"
    local TMP="$TOPDIR/initramfs_tmp"
    rm -rf "$TMP"
    mkdir -p "$TMP"/{bin,sbin,etc,proc,sys,dev,run,tmp,usr/bin,usr/sbin,lib}

    # static busybox for early userspace
    cp "$SYSROOT/bin/busybox" "$TMP/bin/busybox"
    cd "$TMP/bin"
    for cmd in sh mount umount mkdir mknod switch_root modprobe insmod \
               sleep echo cat ls grep sed mkdir pivot_root; do
        ln -sf busybox "$cmd"
    done
    cd -

    cat > "$TMP/init" <<'INIT'
#!/bin/sh
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
mount -t proc     proc     /proc
mount -t sysfs    sysfs    /sys
mount -t devtmpfs devtmpfs /dev
mount -t tmpfs    tmpfs    /run
for f in /lib/modules/*/kernel/drivers/usb/host/*.ko; do
    insmod "$f" 2>/dev/null || true
done
for f in /lib/modules/*/kernel/drivers/ata/*.ko; do
    insmod "$f" 2>/dev/null || true
done
echo "ISO Linux booting..."
echo "Waiting for root device..."
ROOT=""
for i in $(seq 1 30); do
    for dev in /dev/sda /dev/sdb /dev/sda1 /dev/sdb1 /dev/sda2 /dev/sdb2; do
        if mount -t ext4 "$dev" /mnt 2>/dev/null; then
            if [ -d /mnt/usr ] || [ -f /mnt/etc/inittab ]; then
                ROOT="$dev"
                break 2
            fi
            umount /mnt 2>/dev/null
        fi
    done
    sleep 1
done
if [ -z "$ROOT" ]; then
    echo "Root not found. Dropping to shell."
    exec /bin/sh
fi
mkdir -p /mnt/proc /mnt/sys /mnt/dev /mnt/run /mnt/tmp
cp -a /lib/modules /mnt/lib/ 2>/dev/null || true
exec switch_root /mnt /sbin/init
INIT
    chmod +x "$TMP/init"

    mkdir -p "$TMP/lib"
    cp -a "$SYSROOT/lib/modules" "$TMP/lib/" 2>/dev/null || true

    cd "$TMP"
    find . -print0 | cpio --null -ov --format=newc 2>/dev/null | \
        gzip -9 > "$IRD"
    cd -
    rm -rf "$TMP"
    msg "initramfs done: $IRD"
}

# ---- phase 10: build ISO ---------------------------------------------------
build_iso() {
    msg "=== Phase 10: Building ISO ==="
    mkdir -p "$OUTPUT"
    local ISO="$OUTPUT/iso-custom-${KVER}.iso"
    local ISOROOT="$TOPDIR/iso_root"
    rm -rf "$ISOROOT"
    mkdir -p "$ISOROOT/boot/grub"

    cp "$SYSROOT/boot/vmlinuz-${KVER}" "$ISOROOT/boot/"
    cp "$SYSROOT/boot/initramfs-${KVER}.cpio.gz" "$ISOROOT/boot/"

    cat > "$ISOROOT/boot/grub/grub.cfg" <<GRUB
set timeout=3
set default=0

menuentry "ISO Linux" {
    linux /boot/vmlinuz-${KVER} root=/dev/sda1 rw quiet
    initrd /boot/initramfs-${KVER}.cpio.gz
}

menuentry "ISO Linux (RAM)" {
    linux /boot/vmlinuz-${KVER} root=/dev/sda1 rw quiet toram
    initrd /boot/initramfs-${KVER}.cpio.gz
}

menuentry "ISO Linux (safe mode)" {
    linux /boot/vmlinuz-${KVER} root=/dev/sda1 rw nomodeset
    initrd /boot/initramfs-${KVER}.cpio.gz
}
GRUB

    if command -v xorrisofs >/dev/null 2>&1; then
        xorrisofs -o "$ISO" \
            -b boot/grub/grub.cfg \
            -no-emul-boot \
            -boot-load-size 4 \
            -boot-info-table \
            -eltorito-alt-boot \
            -e "boot/vmlinuz-${KVER}" \
            -no-emul-boot \
            -isohybrid-mbr /usr/lib/ISOLINUX/isohdpfx.bin 2>/dev/null || \
        xorrisofs -o "$ISO" -R -J "$ISOROOT"
    elif command -v genisoimage >/dev/null 2>&1; then
        genisoimage -o "$ISO" -R -J -b boot/grub/grub.cfg \
            -c boot/grub/boot.cat "$ISOROOT"
    else
        die "No ISO tool found. Install xorriso or genisoimage."
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
    fetch)    fetch_sources ;;
    kernel)   fetch_sources; build_kernel ;;
    musl)     fetch_sources; build_musl ;;
    busybox)  fetch_sources; build_musl; build_busybox ;;
    deps)     fetch_sources; build_musl; build_deps ;;
    wlroots)  fetch_sources; build_musl; build_deps; build_wlroots ;;
    scene)    build_scene_store ;;
    iso)      build_initramfs; build_iso ;;
    all|"")
        msg "Starting full ISO build..."
        fetch_sources
        build_musl
        build_busybox
        build_kernel
        build_deps
        build_wlroots
        build_scene_store
        assemble_rootfs
        build_initramfs
        build_iso
        msg "=== BUILD COMPLETE ==="
        msg "ISO: $OUTPUT/iso-custom-${KVER}.iso"
        ;;
    *)
        echo "Usage: $0 [all|clean|fetch|kernel|musl|busybox|deps|wlroots|scene|iso]"
        exit 1
        ;;
esac
