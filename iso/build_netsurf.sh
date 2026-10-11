#!/bin/sh
set -e
IDE=/workspaces/scene-store-os/iso
cd "$IDE"
_BUILD_SOURCED=1
. ./build.sh
setup_musl_gcc
JOBS=$(nproc 2>/dev/null || echo 4)
SYSROOT="$IDE/build/sysroot"
SRCX="$IDE/src"
CFLAGS_MUSL="-O2 -fcommon -I$SYSROOT/usr/include -I$SYSROOT/include"
LDFLAGS_MUSL="-L$SYSROOT/usr/lib -L$SYSROOT/lib"
PC_PATH="$SYSROOT/usr/lib/pkgconfig:$SYSROOT/lib/pkgconfig:$SYSROOT/lib/x86_64-linux-gnu/pkgconfig"

msg "=== Netsurf 3.11 (framebuffer/wld) from $SRCX ==="
cd "$SRCX"
if [ ! -d netsurf-all-3.11 ]; then
  [ -f "$SRCX/netsurf-all-3.11.tar.gz" ] || de_fetch netsurf-orig 2>/dev/null || true
  tar xzf "$SRCX/netsurf-all-3.11.tar.gz"
fi
cd "$SRCX/netsurf-all-3.11"
make clean 2>/dev/null || true
make -j"$JOBS" V=1 \
  CC="$MUSL_GCC" HOST_CC=gcc \
  TARGET=framebuffer \
  NETSURF_FB_FRONTEND=wld \
  NETSURF_FB_FONTLIB=internal \
  NETSURF_FB_BPP=32 \
  NETSURF_FB_DEPTH=32 \
  NETSURF_USE_DUKTAPE=NO \
  NETSURF_USE_JPEG=AUTO \
  NETSURF_USE_CURL=YES \
  NETSURF_USE_OPENSSL=YES \
  NETSURF_USE_PNG=YES \
  NETSURF_USE_BMP=YES \
  NETSURF_USE_GIF=YES \
  NETSURF_USE_MNG=AUTO \
  NETSURF_USE_XML=AUTO \
  NETSURF_USER_CFLAGS="$CFLAGS_MUSL" \
  NETSURF_USER_LDFLAGS="$LDFLAGS_MUSL" \
  PKG_CONFIG_PATH="$PC_PATH:$PWD/inst-framebuffer/lib/pkgconfig" \
  2>&1 | tail -40
RC=${PIPESTATUS[0]}
echo "=== netsurf make exit $RC ==="
ls -la "$SRCX/netsurf-all-3.11/build/framebuffer/" 2>/dev/null || echo "no build/framebuffer dir"
msg "=== NETSURF BUILD PHASE COMPLETE (rc=$RC) ==="
