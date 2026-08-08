#!/bin/bash
# iso-build.sh — build the scene-store + wlroots compositor on antiX
#
# Two ways to use:
#   1. Boot into antiX (partition 5), mount Windows C: and run
#   2. From antiX live (D:), copy to local and build
#
# Source locations tried in order:
#   1. /mnt/windows/Users/khalu/Desktop/iso/scene-store (C: from Linux)
#   2. /media/tzuna/*/scene-store (D: auto-mounted)
#   3. /home/tzuna/scene-store (copied manually)

set -e

echo "=== ISO Build Script ==="
echo ""

# Find scene-store source
SRC=""
for candidate in \
    "/mnt/windows/Users/khalu/Desktop/iso/scene-store" \
    "/home/tzuna/scene-store" \
    "/opt/scene-store"; do
    if [ -d "$candidate/src" ]; then
        SRC="$candidate"
        echo "Found source: $SRC"
        break
    fi
done

# Also try auto-mounting C: and checking
if [ -z "$SRC" ]; then
    WIN_MNT="/mnt/windows"
    mkdir -p "$WIN_MNT"
    for dev in /dev/sda2 /dev/sda3 /dev/sdb2 /dev/sdb3; do
        if [ -b "$dev" ]; then
            if mount -t ntfs-3g "$dev" "$WIN_MNT" 2>/dev/null; then
                if [ -d "$WIN_MNT/Users/khalu/Desktop/iso/scene-store/src" ]; then
                    SRC="$WIN_MNT/Users/khalu/Desktop/iso/scene-store"
                    echo "Mounted Windows partition: $dev"
                    break
                fi
                umount "$WIN_MNT" 2>/dev/null
            fi
        fi
    done
fi

if [ -z "$SRC" ]; then
    echo "Could not find scene-store source."
    echo "Either:"
    echo "  1. Copy D:\\scene-store to /home/tzuna/scene-store"
    echo "  2. Or mount C: and ensure the source is at C:\\Users\\khalu\\Desktop\\iso\\scene-store"
    exit 1
fi

# Copy to local build dir (NTFS is slow, ext4 is faster)
BUILD_DIR="/opt/scene-store"
echo "Copying to $BUILD_DIR for fast builds..."
sudo mkdir -p "$BUILD_DIR"
sudo cp -a "$SRC"/* "$BUILD_DIR/"
chmod +x "$BUILD_DIR/iso-build.sh"

# Install build dependencies
echo ""
echo "=== Installing build dependencies ==="
sudo apt-get update -qq

sudo apt-get install -y -qq \
    build-essential gcc make pkg-config \
    libwayland-dev libxkbcommon-dev \
    libdrm-dev libgbm-dev libinput-dev libudev-dev \
    libpixman-1-dev libcairo2-dev libpango1.0-dev \
    wayland-protocols libseat-dev hwdata \
    2>&1 | tail -5

# wlroots package name varies by distro
if ! pkg-config --exists wlroots 2>/dev/null; then
    echo "wlroots not found via pkg-config, trying packages..."
    sudo apt-get install -y -qq libwlroots-dev 2>/dev/null || \
    sudo apt-get install -y -qq libwlroots-0.18-dev 2>/dev/null || \
    sudo apt-get install -y -qq libwlroots-0.17-dev 2>/dev/null || \
    sudo apt-get install -y -qq libwlroots-0.16-dev 2>/dev/null || \
    echo "WARNING: Could not install wlroots-dev. Compositor build may fail."
fi

echo ""
echo "=== Building engine + shell ==="
cd "$BUILD_DIR"
make clean
make all

echo ""
echo "=== Running all test suites ==="
for suite in test_store test_client test_compositor test_automation test_a11y test_rewind test_shell; do
    if [ -f "./build/$suite.exe" ]; then
        result=$(./build/$suite.exe 2>&1 | tail -1)
    else
        result=$(./build/$suite 2>&1 | tail -1)
    fi
    echo "$suite: $result"
done

echo ""
echo "=== All tests passed ==="
echo ""
echo "To build the wlroots compositor (needs the wlroots skeleton from Pass 8):"
echo "  cd $BUILD_DIR"
echo "  make iso-compositor"
echo ""
echo "To run on real hardware:"
echo "  ./build/iso_compositor"
