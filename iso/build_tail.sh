#!/bin/sh
set -e
cd /workspaces/scene-store-os/iso
_BUILD_SOURCED=1
. ./build.sh
build_scene_store
assemble_rootfs
build_initramfs
build_iso
echo "=== TAIL BUILD COMPLETE ==="
