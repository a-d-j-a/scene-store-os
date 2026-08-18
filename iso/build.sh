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
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
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
        MUSL_GCC_SHARED="$SYSROOT/bin/musl-gcc-shared"
        [ -x "$MUSL_GCC_SHARED" ] || \
            { printf '#!/bin/sh\nexec gcc --sysroot=%q -I%q/usr/include -I%q/include -L%q/usr/lib -L%q/lib -Wl,--sysroot=%q "$@"\n' \
                "$SYSROOT" "$SYSROOT" "$SYSROOT" "$SYSROOT" "$SYSROOT" "$SYSROOT" > "$MUSL_GCC_SHARED"; chmod +x "$MUSL_GCC_SHARED"; }
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
        autoconf automake libtool ca-certificates \
        scdoc \
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
    fetch "https://zlib.net/fossils/zlib-1.3.1.tar.gz" \
          "$SRC/zlib-1.3.1.tar.gz"
    fetch "https://www.openssl.org/source/openssl-3.0.13.tar.gz" \
          "$SRC/openssl-3.0.13.tar.gz"
    fetch "https://gitlab.alpinelinux.org/alpine/apk-tools/-/archive/v2.14.4/apk-tools-v2.14.4.tar.gz" \
          "$SRC/apk-tools-v2.14.4.tar.gz"

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
    # -L must cover $SYSROOT/usr/lib: musl --prefix=/usr installs libc.a
    # there, and without it ld falls through to the HOST glibc libc.a
    # (seen live: "Using 'getaddrinfo' in statically linked applications…").
    # -Wl,--sysroot prefixes ld's own default dirs with the sysroot too.
    mkdir -p "$SYSROOT/bin"
    cat > "$SYSROOT/bin/musl-gcc" <<WRAPPER
#!/bin/sh
exec gcc --sysroot="$SYSROOT" -I"$SYSROOT/usr/include" -I"$SYSROOT/include" -L"$SYSROOT/usr/lib" -L"$SYSROOT/lib" -Wl,--sysroot="$SYSROOT" -static "\$@"
WRAPPER
    chmod +x "$SYSROOT/bin/musl-gcc"

    # Shared-lib twin (zlib/openssl/apk): the wrapper above hardcodes
    # -static, which breaks -shared links (crtbeginT.o relocation error).
    # Shared twin for -shared builds (zlib/openssl/apk): gcc's default
    # -dynamic-linker is the glibc loader path; pin the musl loader or
    # every shared binary comes out unloadable in our rootfs.
    cat > "$SYSROOT/bin/musl-gcc-shared" <<WRAPPER
#!/bin/sh
exec gcc --sysroot="$SYSROOT" -I"$SYSROOT/usr/include" -I"$SYSROOT/include" -L"$SYSROOT/usr/lib" -L"$SYSROOT/lib" -Wl,--sysroot="$SYSROOT" -Wl,-dynamic-linker,/lib/ld-musl-x86_64.so.1 "\$@"
WRAPPER
    chmod +x "$SYSROOT/bin/musl-gcc-shared"

    [ -x "$SYSROOT/bin/musl-gcc" ] || die "musl-gcc wrapper not created"
    msg "musl done."
}

# ---- phase 3: build kernel -------------------------------------------------
build_kernel() {
    # Config fingerprint: rebuild the kernel only when the config lines
    # in this script change (cached vmlinuz otherwise).
    local CFG_FP="$(grep '^    scripts/config' "$SCRIPT_DIR/build.sh" | sha256sum | cut -d' ' -f1)"
    if [ -f "$SYSROOT/boot/vmlinuz-${KVER}" ] && \
       [ -f "$SYSROOT/boot/.kconfig-fp" ] && \
       [ "$(cat "$SYSROOT/boot/.kconfig-fp")" = "$CFG_FP" ]; then
        msg "kernel cached (config unchanged), skipping"
        return 0
    fi
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

    # Sound: ALSA core + Intel HDA (QEMU hda-duplex and real HDA codecs).
    # iso_play talks the raw kernel PCM UAPI (/dev/snd/pcmC0D0p) with no
    # libasound; SND_HDA_GENERIC covers QEMU's generic codec and most
    # simple real codecs (specific Realtek/etc. drivers stay off to keep
    # the image small — a vendor codec config can be added later).
    scripts/config --enable SND
    scripts/config --enable SND_PCM
    scripts/config --enable SND_PCI
    scripts/config --enable SND_HDA
    scripts/config --enable SND_HDA_INTEL
    scripts/config --enable SND_HDA_GENERIC

    # Filesystems / misc
    scripts/config --enable EXT4_FS
    scripts/config --enable VFAT_FS
    scripts/config --enable OVERLAY_FS
    scripts/config --enable SQUASHFS
    scripts/config --enable SQUASHFS_ZSTD

    # Networking: wired NICs built-in (the ISO loads no modules)
    scripts/config --enable ETHERNET
    scripts/config --enable NET_VENDOR_INTEL
    scripts/config --enable E1000          # QEMU e1000 + many real NICs
    scripts/config --enable E1000E
    scripts/config --enable NET_VENDOR_REALTEK
    scripts/config --enable RTL8139        # QEMU rtl8139 + older real NICs
    scripts/config --enable R8169          # Realtek Gigabit Ethernet
    scripts/config --enable VIRTIO
    scripts/config --enable VIRTIO_PCI
    scripts/config --enable VIRTIO_NET     # QEMU virtio-net
    scripts/config --enable USB_NET_DRIVERS
    scripts/config --enable USB_NET_AX8817X  # USB ethernet (ASIX)
    scripts/config --enable USB_NET_RTL8152  # USB ethernet (Realtek)

    scripts/config --disable SECURITY
    scripts/config --disable DEBUG_INFO

    make olddefconfig
    make -j"$JOBS" || die "kernel build failed"
    make headers_install INSTALL_HDR_PATH="$SYSROOT" || die "headers_install failed"
    make modules_install INSTALL_MOD_PATH="$SYSROOT" || die "modules_install failed"
    mkdir -p "$SYSROOT/boot"
    cp arch/x86/boot/bzImage "$SYSROOT/boot/vmlinuz-${KVER}"
    echo "$CFG_FP" > "$SYSROOT/boot/.kconfig-fp"
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
    # Networking applets (DHCP client, wget for apk bootstrap, ping, nc)
    sed -i 's/# CONFIG_UDHCPC is not set/CONFIG_UDHCPC=y/' .config
    sed -i 's/# CONFIG_WGET is not set/CONFIG_WGET=y/' .config
    sed -i 's/# CONFIG_PING is not set/CONFIG_PING=y/' .config
    sed -i 's/# CONFIG_NC is not set/CONFIG_NC=y/' .config
    # vi needs sigsetjmp, which musl only exposes as a macro under
    # _GNU_SOURCE — disable it (not needed on the ISO) to kill the
    # classic busybox-on-musl link failure for good.
    sed -i 's/^CONFIG_VI=y$/# CONFIG_VI is not set/' .config
    sed -i "s|CONFIG_PREFIX=.*|CONFIG_PREFIX=\"$SYSROOT\"|" .config
    yes "" | make oldconfig
    make CC="$MUSL_GCC" HOSTCC="gcc" EXTRA_CFLAGS="-D_GNU_SOURCE" \
        -j"$JOBS" || die "busybox build failed"
    make CC="$MUSL_GCC" HOSTCC="gcc" EXTRA_CFLAGS="-D_GNU_SOURCE" \
        install || die "busybox install failed"
    cd -
    [ -x "$SYSROOT/bin/busybox" ] || die "busybox not installed"
    msg "busybox done."
}

# ---- phase 4.5: zlib (apk runtime dependency) -------------------------------
build_zlib() {
    msg "=== Phase 4.5: Building zlib ==="
    setup_musl_gcc
    local VER="1.3.1"
    extract "$SRC/zlib-${VER}.tar.gz" "$BUILDDIR/zlib-${VER}"
    cd "$BUILDDIR/zlib-${VER}"
    ./configure --prefix=/usr
    make -j"$JOBS" CC="$MUSL_GCC_SHARED" \
        LDSHARED="$MUSL_GCC_SHARED -shared -Wl,-soname,libz.so.1" \
        || die "zlib build failed"
    make install DESTDIR="$SYSROOT" CC="$MUSL_GCC_SHARED" \
        LDSHARED="$MUSL_GCC_SHARED -shared -Wl,-soname,libz.so.1" \
        || die "zlib install failed"
    cd -
    msg "zlib done."
}

# ---- phase 4.6: openssl (HTTPS for apk repositories) ------------------------
build_openssl() {
    msg "=== Phase 4.6: Building OpenSSL ==="
    setup_musl_gcc
    local VER="3.0.13"
    extract "$SRC/openssl-${VER}.tar.gz" "$BUILDDIR/openssl-${VER}"
    cd "$BUILDDIR/openssl-${VER}"
    ./Configure linux-x86_64 --prefix=/usr --libdir=/usr/lib \
        --openssldir=/etc/ssl shared \
        -I"$SYSROOT/usr/include" -L"$SYSROOT/usr/lib"
    make -j"$JOBS" CC="$MUSL_GCC_SHARED" || die "openssl build failed"
    make install_sw DESTDIR="$SYSROOT" CC="$MUSL_GCC_SHARED" \
        || die "openssl install failed"
    cd -
    msg "openssl done."
}

# ---- phase 4.7: apk-tools (the package manager) -----------------------------
build_apk() {
    msg "=== Phase 4.7: Building apk-tools ==="
    setup_musl_gcc
    local VER="2.14.4"
    extract "$SRC/apk-tools-v${VER}.tar.gz" "$BUILDDIR/apk-tools-v${VER}"
    cd "$BUILDDIR/apk-tools-v${VER}"
    # No autotools: apk-tools 2.14 is plain make + bundled libfetch; the
    # src/Makefile pulls openssl/zlib flags via pkg-config, so pin them
    # to the sysroot explicitly (command-line vars beat := in-make).
    # The root make must drive the build (obj is set per-subdir); the
    # docs need scdoc (installed in install_prereqs), LUA is optional.
    make -j"$JOBS" \
        CC="$MUSL_GCC_SHARED" LUA=no \
        CFLAGS="-O2 -I$SYSROOT/usr/include" \
        LDFLAGS="-L$SYSROOT/usr/lib" \
        OPENSSL_CFLAGS="-I$SYSROOT/usr/include" \
        OPENSSL_LIBS="-L$SYSROOT/usr/lib -lssl -lcrypto" \
        ZLIB_CFLAGS="-I$SYSROOT/usr/include" \
        ZLIB_LIBS="-L$SYSROOT/usr/lib -lz" \
        || die "apk build failed"
    make install DESTDIR="$SYSROOT" \
        CC="$MUSL_GCC_SHARED" LUA=no \
        CFLAGS="-O2 -I$SYSROOT/usr/include" \
        LDFLAGS="-L$SYSROOT/usr/lib" \
        OPENSSL_CFLAGS="-I$SYSROOT/usr/include" \
        OPENSSL_LIBS="-L$SYSROOT/usr/lib -lssl -lcrypto" \
        ZLIB_CFLAGS="-I$SYSROOT/usr/include" \
        ZLIB_LIBS="-L$SYSROOT/usr/lib -lz" \
        || die "apk install failed"
    # Alpine's signing key from the official alpine-keys package, so apk
    # verifies repository signatures out of the box (no --allow-untrusted).
    local IDXURL="https://dl-cdn.alpinelinux.org/alpine/latest-stable/main/x86_64/APKINDEX.tar.gz"
    curl -fsSL "$IDXURL" -o /tmp/apkindex.tar.gz
    mkdir -p /tmp/apkidx
    tar -xzf /tmp/apkindex.tar.gz -C /tmp/apkidx
    local KVER=$(awk '/^P:alpine-keys$/{f=1} f&&/^V:/{print substr($0,3); exit}' /tmp/apkidx/APKINDEX)
    curl -fsSL "https://dl-cdn.alpinelinux.org/alpine/latest-stable/main/x86_64/alpine-keys-${KVER}.apk" \
        -o /tmp/alpine-keys.apk
    mkdir -p /tmp/alpine-keys
    tar -xzf /tmp/alpine-keys.apk -C /tmp/alpine-keys
    mkdir -p "$SYSROOT/etc/apk/keys"
    cp /tmp/alpine-keys/etc/apk/keys/*.rsa.pub "$SYSROOT/etc/apk/keys/"
    rm -rf /tmp/apkidx /tmp/alpine-keys /tmp/apkindex.tar.gz /tmp/alpine-keys.apk
    cd -
    msg "apk done."
}

# ---- phase 5a: build ffmpeg (static musl, minimal + parser) ----------------
# The lock for this configure lives in scene-store/third_party/ffmpeg/
# LICENSE.md. --enable-parser=mpegvideo is REQUIRED: --disable-everything
# does not auto-enable parsers, and without it the mpegvideo decoder
# corrupts every packetized stream (root-caused Aug 2026; proven by
# tests/test_codec.c, 100/100 frames byte-exact).
build_ffmpeg() {
    msg "=== Phase 5a: Building ffmpeg ==="
    setup_musl_gcc
    local FSRC="$SRC/ffmpeg-n7.1"
    local FOUT="$BUILDDIR/ffmpeg/out"
    if [ ! -f "$FSRC/configure" ]; then
        fetch "https://github.com/FFmpeg/FFmpeg/archive/refs/tags/n7.1.tar.gz" \
              "$SRC/ffmpeg-n7.1.tar.gz"
        extract "$SRC/ffmpeg-n7.1.tar.gz" "$FSRC"
    fi
    cd "$FSRC"
    make distclean >/dev/null 2>&1 || true
    ./configure --cc="$MUSL_GCC" \
        --disable-everything --disable-asm --disable-x86asm \
        --disable-network --disable-debug --disable-doc \
        --enable-static --disable-shared --enable-ffmpeg \
        --enable-decoder=mpeg1video,mpeg2video,rawvideo \
        --enable-encoder=mpeg1video,mpeg2video,rawvideo \
        --enable-parser=mpegvideo \
        --enable-demuxer=mpegvideo,mpegps,mpeg,rawvideo \
        --enable-muxer=mpeg1system,mpeg1video,mpeg2video,image2,rawvideo,null \
        --enable-protocol=file --enable-swscale \
        --enable-filter=scale,format --prefix="$FOUT" \
        || die "ffmpeg configure failed"
    make -j"$JOBS" || die "ffmpeg build failed"
    make install || die "ffmpeg install failed"
    cd -
    ls "$FOUT/lib/"libavcodec.a >/dev/null 2>&1 || die "ffmpeg libs missing"
    msg "ffmpeg done ($FOUT)."
}

# ---- phase 5: build scene-store (engine + DRM compositor) ------------------
build_scene_store() {
    msg "=== Phase 5: Building scene-store ==="
    setup_musl_gcc
    local SSRC="$(cd "$(dirname "$0")/.." && pwd)/scene-store"
    [ -d "$SSRC" ] || die "scene-store source not found at $SSRC"
    build_ffmpeg
    cd "$SSRC"
    # Force full rebuild: stale .o files from previous commits cause
    # subtle alpha/rendering bugs (seen live: 87% opacity on themed elements).
    rm -f build/*.o
    make build/iso_drm build/iso_demo build/iso_terminal build/iso_video \
        build/iso_photo build/iso_files build/iso_edit build/iso_play \
        build/iso_install \
        CC="$MUSL_GCC" \
        CFLAGS="-std=c11 -Wall -Wextra -O2 -Iinclude" \
        FFMPEG_DIR="$BUILDDIR/ffmpeg/out" || die "iso_drm build failed"
    mkdir -p "$SYSROOT/usr/bin"
    cp build/iso_drm "$SYSROOT/usr/bin/iso-drm"
    cp build/iso_demo "$SYSROOT/usr/bin/iso-demo"
    cp build/iso_terminal "$SYSROOT/usr/bin/iso-terminal"
    cp build/iso_video "$SYSROOT/usr/bin/iso-video"
    cp build/iso_photo "$SYSROOT/usr/bin/iso-photo"
    cp build/iso_files "$SYSROOT/usr/bin/iso-files"
    cp build/iso_edit "$SYSROOT/usr/bin/iso-edit"
    cp build/iso_play "$SYSROOT/usr/bin/iso-play"
    cp build/iso_install "$SYSROOT/usr/bin/iso-install"
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
if [ -x /etc/init.d/networking ]; then
    /etc/init.d/networking start 2>/dev/null || true
fi
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
    # Forward autolaunch=NAME / videoclip=PATH kernel cmdline tokens to
    # iso-drm so apps and the importer clip can be set at login
    # (headless test path: no input needed).
    AUTOLAUNCH=""
    for tok in $(cat /proc/cmdline); do
        case "$tok" in
            autolaunch=*) AUTOLAUNCH="$AUTOLAUNCH --autolaunch=${tok#autolaunch=}" ;;
            videoclip=*)  AUTOLAUNCH="$AUTOLAUNCH --videoclip=${tok#videoclip=}" ;;
        esac
    done
    exec /usr/bin/iso-drm $AUTOLAUNCH
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
launcher_apps=iso-terminal,iso-files,iso-demo,iso-video,iso-play,iso-install
CONF

    # Package manager: official Alpine repositories + CA trust bundle
    mkdir -p "$R/etc/apk"
    cat > "$R/etc/apk/repositories" <<'REPO'
https://dl-cdn.alpinelinux.org/alpine/latest-stable/main
https://dl-cdn.alpinelinux.org/alpine/latest-stable/community
REPO
    if [ -f /etc/ssl/certs/ca-certificates.crt ]; then
        mkdir -p "$R/etc/ssl/certs"
        cp /etc/ssl/certs/ca-certificates.crt "$R/etc/ssl/certs/"
        # libfetch (apk) is compiled with CA_CERT_FILE=/etc/apk/ca.pem;
        # OpenSSL's compiled default is /etc/ssl/cert.pem. Cover both.
        mkdir -p "$R/etc/apk" "$R/etc/ssl"
        cp /etc/ssl/certs/ca-certificates.crt "$R/etc/apk/ca.pem"
        cp /etc/ssl/certs/ca-certificates.crt "$R/etc/ssl/cert.pem"
    fi

    # GRUB bootloader for the installed-disk mode (iso-install runs
    # grub-install inside the guest; it needs /usr/sbin/grub-install +
    # /usr/lib/grub/i386-pc in the ROOTFS, not the host). apk --root
    # unpacks the grub + grub-bios packages into the rootfs. The install
    # step is best-effort: without network the rootfs builds anyway and
    # the installer reports "grub failed".
    if command -v apk >/dev/null 2>&1; then
        apk add --root "$R" grub grub-bios >/dev/null 2>&1 && \
            msg "grub installed into rootfs" || \
            echo "WARN: apk add --root grub failed (no network?)"
    else
        echo "WARN: no host apk; grub not installed into rootfs"
    fi

    # Overlay (hand-written boot scripts, may override the above)
    cp -a "$(dirname "$0")/overlay/"* "$R/" 2>/dev/null || true
    # cp -a overwrites the heredoc-written scripts above and carries the
    # overlay files' modes — re-assert exec bits here (git filemode can
    # lose +x on Windows checkouts, seen live: "can't run rcS: Permission
    # denied").
    chmod +x "$R/etc/init.d/rcS" "$R/etc/init.d/scene-desktop" "$R/etc/init.d/networking"
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
    # Drop build-only artifacts. /boot is KEPT: the installer copies the
    # live /boot onto the target disk, and a disk boot loads vmlinuz +
    # initramfs from the disk's own /boot partition.
    rm -rf "$TMP/usr/include" "$TMP/usr/lib/pkgconfig" \
           "$TMP/usr/share/man" 2>/dev/null || true
    # Static libs are build-time only; strip the shared ones (saves MB)
    find "$TMP/usr/lib" -name '*.a' -delete 2>/dev/null || true
    find "$TMP/usr/lib" -type f -name 'lib*.so*' \
        -exec strip --strip-unneeded {} \; 2>/dev/null || true

    cat > "$TMP/init" <<'INIT'
#!/bin/sh
# init — boot the ISO in one of two modes:
#   RAM mode (no persist= on the cmdline): the initramfs IS the rootfs,
#   everything lives in RAM, nothing survives reboot.
#   Persist mode (persist=DEV, or persist=auto): DEV (e.g. /dev/vda) holds
#   the real rootfs. First boot: format if blank, copy the ramfs rootfs to
#   it, then switch_root into it. Later boots: switch_root straight in, so
#   installed packages (apk), /etc, /home survive reboots.
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
mount -t proc     proc     /proc
mount -t sysfs    sysfs    /sys
mount -t devtmpfs devtmpfs /dev
mount -t tmpfs    tmpfs    /run
mkdir -p /dev/pts
mount -t devpts   devpts   /dev/pts
echo "ISO Linux booting..."

PERSIST=""
for tok in $(cat /proc/cmdline 2>/dev/null); do
    case "$tok" in
        persist=*) PERSIST="${tok#persist=}" ;;
    esac
done

if [ -n "$PERSIST" ]; then
    case "$PERSIST" in
        auto)
            # Installed disks (iso-install) carry a DOS label: prefer
            # the first partition; whole-disk ext2 persist disks (the
            # classic proof layout) have no partition and fall through.
            for cand in /dev/vda /dev/sda /dev/hda; do
                [ -b "$cand" ] || continue
                PERSIST="$cand"
                for p in "${cand}1" "${cand}2" "${cand}3" "${cand}4"; do
                    [ -b "$p" ] && { PERSIST="$p"; break; }
                done
                break
            done
            ;;
    esac
    if [ -b "$PERSIST" ]; then
        echo "persist: device $PERSIST"
        mkdir -p /mnt/root
        if ! mount -t ext4 "$PERSIST" /mnt/root 2>/dev/null; then
            echo "persist: blank disk — formatting (ext2 fs, ext4 driver)"
            mke2fs -F -q "$PERSIST" 2>/dev/null && \
                mount -t ext4 "$PERSIST" /mnt/root 2>/dev/null || \
                echo "persist: format/mount FAILED"
        fi
        if [ ! -f /mnt/root/.iso-rootfs-v1 ]; then
            echo "persist: first boot — copying rootfs to disk"
            for d in bin sbin usr lib lib64 etc home root var opt; do
                [ -e "/$d" ] || continue
                cp -a "/$d" /mnt/root/
            done
            mkdir -p /mnt/root/dev /mnt/root/proc /mnt/root/sys \
                     /mnt/root/run /mnt/root/tmp /mnt/root/dev/pts
            touch /mnt/root/.iso-rootfs-v1
        fi
        mkdir -p /mnt/root/proc /mnt/root/sys /mnt/root/dev /mnt/root/run
        mount --move /proc /mnt/root/proc
        mount --move /sys  /mnt/root/sys
        mount --move /dev  /mnt/root/dev
        mount --move /run  /mnt/root/run
        echo "persist: switching root to $PERSIST"
        exec switch_root /mnt/root /sbin/init
    fi
    echo "persist: device $PERSIST missing — RAM mode"
fi

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

    # grub.cfg: single source of truth is iso/grub.cfg (tracked in repo).
    # Regex notes: [0-9.]* would swallow the trailing dot of "6.6.52." —
    # use version-then-(.digits)+ so "initramfs-6.6.52.cpio.gz" keeps the dot.
    # Proof build: `build.sh iso <app> <persist-dev>` concatenates the
    # tracked iso/grub-proof.cfg (hardcoded entry with autolaunch= +
    # persist= baked in, default set to it) — no runtime sed of args.
    cp "$SCRIPT_DIR/grub.cfg" "$ISOROOT/boot/grub/grub.cfg"
    if [ -n "${1:-}" ]; then
        cat "$SCRIPT_DIR/grub-proof.cfg" >> "$ISOROOT/boot/grub/grub.cfg"
    fi
    sed -i "s/vmlinuz-[0-9]\+\(\.[0-9]\+\)\+/vmlinuz-${KVER}/g; s/initramfs-[0-9]\+\(\.[0-9]\+\)\+/initramfs-${KVER}/g" \
        "$ISOROOT/boot/grub/grub.cfg"

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
    busybox)   install_prereqs; fetch_sources; build_musl; build_kernel; build_busybox; build_zlib; build_openssl; build_apk ;;
    scene)     build_scene_store ;;
    rootfs)    assemble_rootfs ;;
    initramfs) build_initramfs ;;
    iso)       shift 2>/dev/null || true; build_iso "$1" "$2" ;;
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
        echo "Usage: $0 [all|clean|prereqs|fetch|musl|kernel|busybox|scene|ffmpeg|rootfs|initramfs|iso]"
        exit 1
        ;;
esac
