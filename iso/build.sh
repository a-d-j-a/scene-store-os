#!/bin/sh
# ISO Build System — fully custom Linux, no Debian base.
# Builds: musl, kernel, busybox, wlroots, scene-store → bootable ISO.
# Run on any Linux with gcc and make (e.g. Ubuntu Codespace).
#   ./build.sh          (full build)
#   ./build.sh clean    (nuke everything)
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
WAYLANDVER="1.22.0"
WLRVER="0.17.4"
PIXMANVER="0.42.2"
LIBDRMVER="2.4.122"
LIBXKBCOMMONVER="1.5.0"
UDEVVER="3.2.14"

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
        *.tar.xz) tar -xf "$archive" -C "$destdir" --strip-components=1 ;;
        *.tar.gz) tar -xzf "$archive" -C "$destdir" --strip-components=1 ;;
        *.tar.bz2) tar -xjf "$archive" -C "$destdir" --strip-components=1 ;;
        *) die "Unknown archive format: $archive" ;;
    esac
}

MUSL_GCC=""
setup_musl_gcc() {
    # musl-gcc can be at bin/ or usr/bin/ depending on install
    if [ -x "$SYSROOT/bin/musl-gcc" ]; then
        MUSL_GCC="$SYSROOT/bin/musl-gcc"
    elif [ -x "$SYSROOT/usr/bin/musl-gcc" ]; then
        MUSL_GCC="$SYSROOT/usr/bin/musl-gcc"
    else
        die "musl-gcc not found in $SYSROOT"
    fi

    export CC="$MUSL_GCC"
    export CFLAGS="--sysroot=$SYSROOT -I$SYSROOT/include"
    export LDFLAGS="--sysroot=$SYSROOT -L$SYSROOT/lib -static"
    export PKG_CONFIG_PATH="$SYSROOT/lib/pkgconfig:$SYSROOT/lib64/pkgconfig"
    export PKG_CONFIG_LIBDIR="$SYSROOT/lib/pkgconfig"
    export PATH="$SYSROOT/bin:$SYSROOT/usr/bin:$PATH"

    mkdir -p "$BUILDDIR"
    cat > "$BUILDDIR/musl-cross.txt" <<CROSS
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
}

# ---- phase 0: install host prerequisites -----------------------------------
install_prereqs() {
    msg "=== Phase 0: Installing host prerequisites ==="
    apt-get update -qq 2>/dev/null || true
    apt-get install -y -qq \
        flex bison bc libelf-dev \
        meson ninja-build pkg-config \
        xorriso grub-pc-bin grub-common mtools \
        cpio gzip xz-utils \
        libseat-dev libinput-dev libudev-dev \
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
    fetch "https://github.com/eudev-project/eudev/releases/download/v${UDEVVER}/eudev-${UDEVVER}.tar.gz" \
          "$SRC/eudev-${UDEVVER}.tar.gz"

    msg "All sources fetched."
}

# ---- phase 2: build musl --------------------------------------------------
build_musl() {
    msg "=== Phase 2: Building musl ==="
    extract "$SRC/musl-${MUSLVER}.tar.gz" "$BUILDDIR/musl-${MUSLVER}"
    mkdir -p "$SYSROOT"
    cd "$BUILDDIR/musl-${MUSLVER}"
    ./configure --prefix=/usr
    make -j"$JOBS" || die "musl build failed"
    make install DESTDIR="$SYSROOT" || die "musl install failed"
    cd -

    # Ensure musl-gcc wrapper is accessible at $SYSROOT/bin
    mkdir -p "$SYSROOT/bin"
    if [ -x "$SYSROOT/usr/bin/musl-gcc" ] && [ ! -x "$SYSROOT/bin/musl-gcc" ]; then
        cp "$SYSROOT/usr/bin/musl-gcc" "$SYSROOT/bin/musl-gcc"
        chmod +x "$SYSROOT/bin/musl-gcc"
    fi

    # Fix the specs path in the musl-gcc wrapper — it hardcodes /usr/lib/musl-gcc.specs
    # but we installed to $SYSROOT/usr/lib/
    if [ -f "$SYSROOT/bin/musl-gcc" ]; then
        sed -i "s|/usr/lib/musl-gcc.specs|$SYSROOT/usr/lib/musl-gcc.specs|g" "$SYSROOT/bin/musl-gcc"
    fi
    if [ -f "$SYSROOT/usr/bin/musl-gcc" ]; then
        sed -i "s|/usr/lib/musl-gcc.specs|$SYSROOT/usr/lib/musl-gcc.specs|g" "$SYSROOT/usr/bin/musl-gcc"
    fi

    # Verify
    if [ -x "$SYSROOT/bin/musl-gcc" ] || [ -x "$SYSROOT/usr/bin/musl-gcc" ]; then
        msg "musl done."
    else
        die "musl-gcc wrapper not found after install"
    fi
}

# ---- phase 3: build kernel -------------------------------------------------
build_kernel() {
    msg "=== Phase 3: Building kernel ==="
    extract "$SRC/linux-${KVER}.tar.xz" "$BUILDDIR/linux-${KVER}"
    cd "$BUILDDIR/linux-${KVER}"
    make defconfig
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
    make headers_install INSTALL_HDR_PATH="$SYSROOT"
    make modules_install INSTALL_MOD_PATH="$SYSROOT"
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
    sed -i 's/CONFIG_FEATURE_SH_STANDALONE=y/# CONFIG_FEATURE_SH_STANDALONE is not set/' .config
    sed -i 's/CONFIG_FEATURE_MOUNT_LOOP is not set/CONFIG_FEATURE_MOUNT_LOOP=y/' .config
    sed -i 's/CONFIG_FEATURE_MOUNT_LOOP_CREATE is not set/CONFIG_FEATURE_MOUNT_LOOP_CREATE=y/' .config
    sed -i "s|CONFIG_PREFIX=.*|CONFIG_PREFIX=\"$SYSROOT\"|" .config
    yes "" | make oldconfig
    make CC="$MUSL_GCC" HOSTCC="gcc" -j"$JOBS" || die "busybox build failed"
    make CC="$MUSL_GCC" HOSTCC="gcc" install || die "busybox install failed"
    cd -
    msg "busybox done."
}

# ---- phase 5: build dependencies for wlroots ------------------------------
build_deps() {
    msg "=== Phase 5: Building dependencies ==="
    setup_musl_gcc
    local CROSS="$BUILDDIR/musl-cross.txt"

    msg "  pixman..."
    extract "$SRC/pixman-${PIXMANVER}.tar.gz" "$BUILDDIR/pixman-${PIXMANVER}"
    cd "$BUILDDIR/pixman-${PIXMANVER}"
    ./configure --prefix=/usr --host=x86_64-linux-musl \
        --disable-shared --enable-static
    make -j"$JOBS" && make install DESTDIR="$SYSROOT"
    cd -

    msg "  libdrm..."
    extract "$SRC/libdrm-${LIBDRMVER}.tar.xz" "$BUILDDIR/libdrm-${LIBDRMVER}"
    cd "$BUILDDIR/libdrm-${LIBDRMVER}"
    ./configure --prefix=/usr --host=x86_64-linux-musl \
        --disable-shared --enable-static
    make -j"$JOBS" && make install DESTDIR="$SYSROOT"
    cd -

    msg "  libxkbcommon..."
    extract "$SRC/libxkbcommon-${LIBXKBCOMMONVER}.tar.xz" "$BUILDDIR/libxkbcommon-${LIBXKBCOMMONVER}"
    cd "$BUILDDIR/libxkbcommon-${LIBXKBCOMMONVER}"
    ./configure --prefix=/usr --host=x86_64-linux-musl \
        --disable-shared --enable-static \
        --enable-x11=no --disable-wayland --disable-docs
    make -j"$JOBS" && make install DESTDIR="$SYSROOT"
    cd -

    msg "  wayland..."
    extract "$SRC/wayland-${WAYLANDVER}.tar.xz" "$BUILDDIR/wayland-${WAYLANDVER}"
    cd "$BUILDDIR/wayland-${WAYLANDVER}"
    ./configure --prefix=/usr --host=x86_64-linux-musl \
        --disable-shared --enable-static \
        --disable-scanner --disable-documentation
    make -j"$JOBS" && make install DESTDIR="$SYSROOT"
    cd -

    msg "  eudev..."
    extract "$SRC/eudev-${UDEVVER}.tar.gz" "$BUILDDIR/eudev-${UDEVVER}"
    cd "$BUILDDIR/eudev-${UDEVVER}"
    ./configure --prefix=/usr --host=x86_64-linux-musl \
        --disable-shared --enable-static \
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
    extract "$SRC/wlroots-${WLRVER}.tar.gz" "$BUILDDIR/wlroots-${WLRVER}"
    cd "$BUILDDIR/wlroots-${WLRVER}"

    meson setup _build \
        --cross-file "$BUILDDIR/musl-cross.txt" \
        --prefix=/usr \
        -Dexamples=false \
        -Dxwayland=false \
        -Dbackends=drm \
        -Drenderers=gles2 || die "wlroots meson setup failed"
    ninja -C _build -j"$JOBS" || die "wlroots build failed"
    DESTDIR="$SYSROOT" ninja -C _build install
    cd -
    msg "wlroots done."
}

# ---- phase 7: build scene-store --------------------------------------------
build_scene_store() {
    msg "=== Phase 7: Building scene-store ==="
    local SSRC="$(cd "$(dirname "$0")/.." && pwd)/scene-store"
    if [ ! -d "$SSRC" ]; then
        warn "scene-store source not found at $SSRC — skipping"
        return 0
    fi
    cd "$SSRC"
    make clean 2>/dev/null || true
    # Try to build Linux binaries; don't fail the whole build if scene-store has issues
    if make -j"$JOBS" all EXE_SUFFIX="" 2>/dev/null; then
        msg "scene-store built successfully"
    else
        warn "scene-store build had issues (non-fatal — ISO will use initramfs only)"
    fi
    mkdir -p "$SYSROOT/usr/bin"
    for bin in iso-compositor iso-preview scene_store scene_client; do
        [ -f "build/${bin}" ] && cp "build/${bin}" "$SYSROOT/usr/bin/$bin" 2>/dev/null || true
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
/sbin/udevd --daemon 2>/dev/null || true
udevadm trigger --action=add 2>/dev/null || true
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
    local IRD="$BUILD/initramfs-${KVER}.cpio.gz"
    rm -rf "$BUILD/initramfs_tmp"
    local TMP="$BUILD/initramfs_tmp"
    mkdir -p "$TMP"/{bin,sbin,etc,proc,sys,dev,run,tmp,usr/bin,usr/sbin,lib}

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
exec /sbin/init
INIT
    chmod +x "$TMP/init"

    mkdir -p "$TMP/lib"
    cp -a "$SYSROOT/lib/modules" "$TMP/lib/" 2>/dev/null || true

    cd "$TMP"
    find . -print0 | cpio --null -ov --format=newc 2>/dev/null | gzip -9 > "$IRD"
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
    cp "$BUILD/initramfs-${KVER}.cpio.gz" "$ISOROOT/boot/"

    cat > "$ISOROOT/boot/grub/grub.cfg" <<GRUB
set timeout=3
set default=0

menuentry "ISO Linux (All-in-RAM)" {
    linux /boot/vmlinuz-${KVER} rw quiet
    initrd /boot/initramfs-${KVER}.cpio.gz
}

menuentry "ISO Linux (Safe Mode)" {
    linux /boot/vmlinuz-${KVER} rw nomodeset
    initrd /boot/initramfs-${KVER}.cpio.gz
}
GRUB

    if command -v grub-mkrescue >/dev/null 2>&1; then
        grub-mkrescue -o "$ISO" "$ISOROOT" || die "grub-mkrescue failed"
    elif command -v xorrisofs >/dev/null 2>&1; then
        xorrisofs -o "$ISO" -R -J "$ISOROOT" || die "xorrisofs failed"
    elif command -v genisoimage >/dev/null 2>&1; then
        genisoimage -o "$ISO" -R -J "$ISOROOT" || die "genisoimage failed"
    else
        die "No ISO creation tool found. Install grub-mkrescue, xorriso, or genisoimage."
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
    prereqs)  install_prereqs ;;
    fetch)    fetch_sources ;;
    musl)     install_prereqs; fetch_sources; build_musl ;;
    kernel)   install_prereqs; fetch_sources; build_musl; build_kernel ;;
    busybox)  install_prereqs; fetch_sources; build_musl; build_kernel; build_busybox ;;
    deps)     install_prereqs; fetch_sources; build_musl; build_kernel; build_busybox; build_deps ;;
    wlroots)  install_prereqs; fetch_sources; build_musl; build_kernel; build_busybox; build_deps; build_wlroots ;;
    scene)    build_scene_store ;;
    iso)      build_initramfs; build_iso ;;
    all|"")
        msg "Starting full ISO build..."
        install_prereqs
        fetch_sources
        build_musl
        build_kernel
        build_busybox
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
        echo "Usage: $0 [all|clean|prereqs|fetch|musl|kernel|busybox|deps|wlroots|scene|iso]"
        exit 1
        ;;
esac
