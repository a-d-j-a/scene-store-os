# FFmpeg (libavcodec/libavformat/libswscale/libavutil) vendored build

Static minimal build of FFmpeg 7.1 (`n7.1` tag, commit lineage 2024-07),
compiled in w64devkit (mingw-w64 gcc) for the Windows test harness, and
reproduced with the identical configure for the ISO's musl static build
(see `iso/build.sh` `build_ffmpeg`).

## Configure (locked; must match the ISO build exactly)

    ./configure --cc=gcc --target-os=mingw32 --arch=x86_64 \
      --disable-everything --disable-asm --disable-x86asm \
      --disable-network --disable-debug --disable-doc \
      --enable-static --disable-shared --enable-ffmpeg \
      --enable-decoder=mpeg1video,mpeg2video,rawvideo \
      --enable-encoder=mpeg1video,mpeg2video,rawvideo \
      --enable-parser=mpegvideo \
      --enable-demuxer=mpegvideo,mpegps,mpeg,rawvideo \
      --enable-muxer=mpeg1system,mpeg1video,mpeg2video,image2,rawvideo,null \
      --enable-protocol=file --enable-swscale \
      --enable-filter=scale,format --prefix=build/out

(On Linux the `--cc/--target-os/--arch` flags are omitted; everything
else is identical.)

## Why this exact configure

- `--disable-everything` + explicit enables keeps the static closure
  tiny (libavcodec.a ~1.3 MB, four libs ~3.4 MB total) — the ISO stays
  small and boots fast; only the two MPEG-1/2 decoders, the container
  demuxers, raw video, and the scale filter are compiled in.
- `--enable-parser=mpegvideo` is REQUIRED: `--disable-everything` does
  not auto-enable parsers, and without the mpegvideo parser the
  decoder corrupts every packetized stream (first pictures duplicated,
  all AC coefficients lost, "ac-tex damaged"). This defect was
  root-caused in August 2026 by differential testing: the identical
  stream decodes 100/100 frames with stock full-config ffmpeg (Ubuntu
  6.1.1 and a BtbN full Windows build) and 100/100 with this minimal
  build + the parser; it fails with the minimal build without it.
  A static-content control clip round-tripped cleanly either way,
  which is why the defect first looked like a toolchain problem.
- No x86 asm (no yasm in the toolchain): pure-C decode is exact for
  our deterministic clips; the `-O0`/`-O3` and asm-on/off comparisons
  all behaved identically — the parser was the whole story.
- `-lbcrypt` is required on Windows at link time (avutil random seed);
  Linux links `-lm -lz -pthread` as needed.

## Adoption justification (per the project's create-our-own rule)

The OS owns video decode at the seam where media enters the scene:
iso-video pushes only a texture REFERENCE on the v0 wire; the pixels
are decoded HERE, host-side, once per frame. MPEG-1/2 (ISO/IEC 13818
etc.) is a mature international standard whose trustworthy
implementation is the product of decades of security hardening —
rebuilding a conformant decoder at equivalent trust is not achievable
by an OS project whose point is the semantic scene. This is the same
class of boundary as a kernel driver or a codec engine, where the rule
itself says adoption is the honest choice (and the honest-boundary
line is stated, never hidden: video arrives as composited textures,
effects applied, not semantically owned).

ffmpeg is chosen because it is the reference implementation, LGPL
(no licensing drag on the ISO; `FFMPEG-LICENSE.md` + `COPYING.LGPLv2.1`
vendored), and statically linkable — no dynamic loader, no runtime
library surprises on the ISO. The build is our own locked configure
above (vendored verbatim source, no modifications), verified by the
deterministic decode suite (`tests/test_codec.c`): the fixture
`tests/fixtures/demo.mpg` decodes 100/100 frames, byte-identical
across repeated decodes, with the test deriving every expected pixel
from the same generator that made the clip. The codec runs ONLY
host-side in the OS (scene_codec.c, then iso_drm's importer), never
rides the wire, never runs inside an app process.

Nothing else was adopted; the wire format, scene store, compositor,
effects, shell, and all first-party apps remain in-house.
