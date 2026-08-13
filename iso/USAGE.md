# iso usage

How to build, boot, and drive the iso (kernel + initramfs + DRM desktop).

## Build

Everything lives in `iso/build.sh` (incremental phases; each re-runs cleanly):

    sh iso/build.sh scene        # scene-store apps: iso-drm, iso-demo, iso-terminal
    sudo sh iso/build.sh rootfs  # assemble sysroot (root-owned), apk repos
    sudo sh iso/build.sh initramfs

- Requires sudo for rootfs/initramfs (sysroot files are root-owned).
- Kernel: 6.6.52 at `build/sysroot/boot/vmlinuz-6.6.52`.
- Initramfs: `build/initramfs-6.6.52.cpio.gz` (repo-root `build/`, not `iso/build/`).
- App binaries land in `build/sysroot/usr/bin/{iso-drm,iso-demo,iso-terminal,
  iso-video}` then get packed into the initramfs. Phase `busybox` covers the
  whole toolchain in one go (musl, kernel, busybox, zlib, openssl, apk).

## Boot (verification)

`iso/qemu-proof.sh` boots kernel + initramfs directly, with options:

    sh iso/qemu-proof.sh                                # serial proof mode:
                                                        # udhcpc lease + pkgtest gate
    sh iso/qemu-proof.sh shot autolaunch=iso-terminal   # 45 s boot, screendump to
                                                        # /tmp/qemu-shot.ppm, serial
                                                        # to serial-shot.log
    sh iso/qemu-proof.sh disk=state.img pkgtest=htop    # attach a raw virtio disk,
                                                        # persist=/dev/vda appended
    sh iso/qemu-proof.sh shot cdrom=output/iso..iso     # boot the GRUB ISO (-cdrom),
                                                        # not -kernel/-initrd

QEMU flags used: `-m 512 -netdev user,id=n1 -device e1000,netdev=n1
-console ttyS0`. In shot mode the display is `-vga std` (1280x800) and the
stack is proven down to pixels: terminal body paints `0xFF0C0C0C`, titlebar
`0xFF1A1A1A`, desktop `0xFF1A1A2E`. Note: screendump PPMs store BGR bytes.

GRUB/ISO proof build (extra boot args baked into a tracked proof entry):

    sh iso/build.sh iso iso-terminal /dev/vda   # appends iso/grub-proof.cfg:
                                                # autolaunch=iso-terminal
                                                # persist=/dev/vda, default=4

## Persistence (`persist=`)

Default boot is all-in-RAM (the initramfs IS the rootfs; reboots lose
everything). With `persist=DEV` (e.g. `persist=/dev/vda`, or `persist=auto`
= first of vda/sda/hda) the system becomes persistent:

- First boot: a blank disk is formatted (ext2, read by the ext4 driver) and
  the ramfs rootfs is copied onto it; `/mnt/root/.iso-rootfs-v1` marks it.
- Every boot: the disk root is mounted and `switch_root`'d into — the disk,
  not RAM, is the running system. Installed packages (`apk add`), `/etc`,
  `/home` survive reboots. Proven: `pkgtest=htop` on boot 1, then
  `pkgtest=check:htop` on boot 2 reports `present OK` with no re-copy.

## Kernel cmdline

- `autolaunch=NAME` — start app NAME at boot (forwarded to iso-drm as
  `--autolaunch=NAME`, up to 4). Example: `autolaunch=iso-terminal`.
- `persist=DEV` / `persist=auto` — disk persistence (see above).
- `pkgtest=PKG` — after the NIC gets a lease, run `apk add --no-cache PKG`;
  result printed to serial as `network: pkgtest: ... OK/FAILED`.
- `pkgtest=check:PKG` — verify `command -v PKG` (persistence checks across
  boots); prints `present OK` / `absent FAIL`.

## Boot sequence (what runs)

1. `init` (initramfs) — mounts proc/sysfs/devtmpfs/run/devpts, then either
   execs `/sbin/init` (RAM mode) or formats/mounts the persist disk and
   `switch_root`s into it (see Persistence).
2. `rcS` — mounts proc/sysfs/devtmpfs/run/tmp/devpts, hostname `iso`, lo up.
3. `networking` — first non-lo NIC up, `udhcpc -i <iface> -b -q` (never blocks
   boot), `pkgtest=` gate.
4. `scene-desktop` — starts `iso-drm` (DRM compositor: desktop background,
   panel, start menu, clock) and forwards `autolaunch=` tokens.

Logins on tty1-3 auto-login `user` (no password; root and user accounts exist,
empty shadows, `/etc/shadow` has no hashes). Orchestrate over serial: tty1-3
are only on virtual consoles; use `-serial file:` logs or
`console=ttyS0 loglevel=7` for the boot/desktop log stream.

## Desktop

- Poll-free compositor sessions: the shell is layer 0; every app you launch
  joins as its own layer with its own scene-store session, so a dead app
  repaints as desktop instead of freezing anything.
- Start button (panel-left) toggles the launcher menu; menu items are
  `launcher_apps=` from `/etc/shell.conf` (default
  `iso-terminal,iso-demo,iso-video`). Choosing an item spawns the app, which
  connects over TCP and paints.
- Windows move by dragging the titlebar; resize by dragging the right edge,
  bottom edge, or bottom-right corner (min 96x64; titlebar/close/content
  re-derive as the window resizes).
- `iso-terminal` = first guest app: window with titlebar (0xFF1A1A1A, white
  title text) and 0xFF0C0C0C body, `/ # ` prompt rendered from the scene-store
  bitmap font; it types via evdev keyboard.
- `iso-video` = honest-boundary demo: a synthetic video stream pushed as
  composited textures (client sends only the texture reference over the wire;
  pixels live OS-side in the importer — browser/video/WebGL will arrive the
  same way, effects applied, not semantically owned).
- Theme: `/etc/shell.conf` (`Option=Value`, hex ARGB). Reload-safe: the shell
  re-applies a modified config on each tick; live re-theme of running apps is
  a compositor capability (`set_style`).
- All pixels come from the scene store: every node is a typed, versioned
  semantic object (role/rect/text/style), the compositor only renders it.
  Cross-app search, replay, automation are consumers of that store.

## apk

Official Alpine repos (latest-stable main + community). CAs installed from the
build host at `/etc/apk/ca.pem` (libfetch) and `/etc/ssl/cert.pem` (OpenSSL).
Example on serial or a tty:

    apk add --no-cache htop

## Host-side tests

Windows (w64devkit) and musl-gcc Linux builds both go through:

    make -C scene-store all          # zero warnings (-Wall -Wextra)
    scene-store/build/test_*.exe     # 16 suites, 2,080 checks, 0 failures
                                     # (store, client, compositor, automation,
                                     #  a11y, rewind, shell, app, terminal,
                                     #  sessions, launcher, settings, theme,
                                     #  image, wallpaper, video_app)

Deterministic — compositor and client suites rerun byte-identical output.