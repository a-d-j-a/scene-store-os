#!/bin/sh
# build35b.sh - build remaining phases after musl/kernel/busybox/zlib/openssl
# Run on the codespace: cd /workspaces/scene-store-os/iso && nohup sh build35b.sh &
set -e
cd /workspaces/scene-store-os/iso
_BUILD_SOURCED=1
. ./build.sh

build_wpa_supplicant
build_libdrm
build_wayland_protocols
build_wayland
build_pixman
build_libxkbcommon
build_libevdev
build_mtdev
build_libseat
build_libinput
build_wlroots
build_scene_store
build_iso_wl
assemble_rootfs
build_initramfs
build_iso

echo "=== BUILD COMPLETE ==="
