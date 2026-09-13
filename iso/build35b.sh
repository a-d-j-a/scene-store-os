#!/bin/sh
# build35b.sh - run only the remaining phases (musl/kernel/busybox/zlib/openssl already done)
set -e
cd /workspaces/scene-store-os/iso
. ./build.sh

echo "--- Phase: wpa_supplicant ---"
build_wpa_supplicant

echo "--- Phase: libdrm ---"
build_libdrm

echo "--- Phase: wayland-protocols ---"
build_wayland_protocols

echo "--- Phase: wayland ---"
build_wayland

echo "--- Phase: pixman ---"
build_pixman

echo "--- Phase: libxkbcommon ---"
build_libxkbcommon

echo "--- Phase: libevdev ---"
build_libevdev

echo "--- Phase: mtdev ---"
build_mtdev

echo "--- Phase: libseat ---"
build_libseat

echo "--- Phase: libinput ---"
build_libinput

echo "--- Phase: wlroots ---"
build_wlroots

echo "--- Phase: scene-store ---"
build_scene_store

echo "--- Phase: iso-wl ---"
build_iso_wl

echo "--- Phase: rootfs ---"
assemble_rootfs

echo "--- Phase: initramfs ---"
build_initramfs

echo "--- Phase: iso ---"
build_iso

echo "=== BUILD COMPLETE ==="
