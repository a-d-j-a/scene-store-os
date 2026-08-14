================================================================================
SEMANTIC SCENE ENGINE - FULL HANDOFF - POST PASS 16 + DAILY-DRIVER MILESTONE
Written: 2026-08-13, by the agent working the "daily-driver" milestone.
Purpose: carries EVERYTHING needed to continue in a brand new chat, cold, with
zero history. Read this file first, then AGENTS.md (rules + research state) and
scene-store-spec.md (locked wire format, v0) before touching anything.
Status as of this document: daily-driver milestone COMPLETE (networking,
apk package manager, GRUB ISO boot, working terminal app — ALL PROVEN in
QEMU). The one active bug (terminal never paints) was root-caused, fixed,
and verified at wire level and in a QEMU screendump (see §6/§16.2).
Post-milestone (2026-08-13, same day): persistence (persist=DEV switch_root
mode — apk installs survive reboots, two-boot proven), GRUB -cdrom boot
proof with terminal visible + pixels verified, shell window resize by
pointer, iso-video texture-streaming demo (honest boundary). See §16.3.
Remaining before shipping: update the AGENTS.md ledger; then the release
gap list is: real-hardware bring-up, storage/file manager, power
management, and wlroots integration (skeleton exists).
Debug instrumentation is stripped (eddb960) and re-verified on the QEMU
shot with a clean serial log.
================================================================================

--------------------------------------------------------------------------------
1. THE PROJECT (never lose sight)
--------------------------------------------------------------------------------
An OS as fast and light as antiX/Puppy (boots+runs on that hardware) but not
1999-looking and deeply customizable. Modern look + deep customization come from
the OS layer itself: a "semantic scene" datastore that OWNS the meaning of every
native app's visible UI (regions, roles, labels, values, text, geometry, focus)
per frame, versioned, with an append-only op log. Drawing is downstream. The
novel asset is UNDERSTANDING, not rendering (Plan9/NeWS/BeOS=rasters; Flatland/
wlroots=composition only; AT-SPI/UIA/AX/Newton=a11y mirrors, no store/version/
replay). Capabilities falling out: cross-app live search, cross-app UI
automation, deterministic whole-desktop replay/testing, re-theme of running
apps, ghost-crash recovery, zero-cost capture. Honest boundary: browser/video/
WebGL arrive as composited textures (effects applied, not semantically owned).
CONSTRUCTION ORDER: scene store first; compositor, effects, search, automation,
a11y, rewind are consumers. Wire format is LOCKED before code.

HARD RULES (from AGENTS.md, binding): take the given time (early finish = fail);
never settle (dig one layer deeper; verify from primary sources); no asking what
was already answered; no compromises (full-fledged desktop); CREATE OUR OWN
unless absolutely necessary (justify adoption in writing); never hallucinate
(verify or mark unverified; never claim done when not); use parallel sub-agents;
max focus.

--------------------------------------------------------------------------------
2. RESEARCH / VERIFICATION STATE (primary sources, not summaries)
--------------------------------------------------------------------------------
Pass 1-3 confirmed NO SHIPPED SYSTEM owns a stored, versioned, replayable
semantic scene; every existing semantic source is a derived unversioned mirror,
an opaque passthrough, or a hardcoded stub — verified at source-code level:
- Plan 9 draw(3) alive today; Fuchsia Flatland = retained composition only,
  VDC-hardware-delegated, no semantics; uber_struct.h stateless; epoch is a
  renderer structural-change hint only.
- Windows UIA (Vista 2007) + macOS AX trees = read-mostly a11y mirrors, no
  versioning/replay/store. Windows Timeline retired; Recall = screenshots;
  App Content Search = opt-in file index.
- Linux AT-SPI = pull-model D-Bus a11y mirror, chatty IPC, no screen coords,
  opt-in toolkit support. atspi-accessible.c = client-side pull cache
  (cached_properties bitmask, ATSPI_MAX_CHILDREN partial caching, blocking
  D-Bus on miss). registryd/registry.c = OS-side "desktop frame" is hardcoded
  stubs: role 14 TODO, extents 1024x768, GetSize literal "TODO - Get the
  screen size", GetItems empty, GetAccessibleAtPoint null, Contains FALSE.
  gtkatspicache.c = point-in-time snapshot, ITEM_SIGNATURE has no geometry,
  AddAccessible/RemoveAccessible unsequenced.
- Chromium ax_tree.{cc,h} = current-state id→node map; Unserialize = one atomic
  AXTreeUpdate, fatal-error semantics, caches invalidated every update; no op
  log/versioning/replay; per-app in-process.
- GNOME Newton (Campbell 2023-2024, STF-funded) = closest neighbor, explicitly
  pass-through only: "the compositor doesn't process accessibility tree
  updates; it only passes them through." Wire-level confirmation (Wayback
  archive of mwcampbell/wayland-protocols accessibility branch, commit
  a5fb7cd): JSON serialized fds, "This allows servers to avoid retaining a
  full accessibility tree for each surface", no coords, no seq/ack, per-surface
  only, UpdatesWanted re-sends full tree. Never merged upstream; AccessKit GTK
  impl stagnant at 2026 hackfest; GTK 4.18 merged AccessKit backend (2025,
  Windows/macOS only, AT-SPI stays Linux default).
- COSMIC: cosmic-a11y-unstable-v1.xml = magnifier + screen-filter toggles,
  zero semantic content. wlroots wlr_scene.h = ROOT/TREE/SURFACE/RECT/BUFFER,
  enabled flag, parent-relative x/y, no roles/text.
- Verdict (delivered 2026-08-03): method kept; stored/versioned/replayable
  scene is open ground on Linux; no existing layer can host it without
  inheriting its exact limitation. Nothing was adopted; building our own stays
  justified per the create-our-own rule.

--------------------------------------------------------------------------------
3. WIRE FORMAT (LOCKED, v0) AND SCENE STORE
--------------------------------------------------------------------------------
- Spec: scene-store-spec.md (v0), locked before code. §3: Rect is ABSOLUTE
  session-space coords. §7: commit-vs-in-flight rule — committed scene =
  server truth, in-flight input = client truth. Input flow control DROPS
  unacked presses (never queued; the ack only reopens the gate). Macros are
  per-session by design; cross-session replay via scene_store_import_macro.
- Frame: SCENE_MAGIC=0x5343454e, SCENE_PROTOCOL_V0=0, SCENE_HEADER_SIZE=16
  (magic u32, ver u16, opcode u16, plen u32, checksum u32). Checksum = FNV1a32
  over [0, 16+record_length) with the 4 checksum bytes zeroed at compute time.
  11 server records (WELCOME/ERROR/SNAPSHOT/CAPTURE/SEARCH_RESULT/PONG/
  INPUT_POINTER/INPUT_ACTIVATE/INPUT_FOCUS/PRESENT_DONE/TEXT_INDEX). Ack = 16B
  consumed_input_seq+token, does NOT advance the stream counter. Strictly
  consecutive seq starting at 1 per session; ghost rejoin rebases the stream.
- Engine core (pass 4): arena node storage (revised: slot-based with gaps,
  free-list reuse, live-count tracking); apply_ctx holds `node **arena_back`
  back-pointer synced after every arena_alloc (the SIGSEGV fix); 10k-node
  bench baselines: build ~10k rec/s, replay rebuild ~4.2M rec/s, snapshot
  597KB @ ~70MB/s, region_at ~130µs, search ~530µs. region_at/search are O(N)
  linear scans — accepted, spatial/text index is a tracked later optimization.
- Wire format has NOT been touched since locking; all passes kept it stable.

--------------------------------------------------------------------------------
4. IMPLEMENTATION PASSES 1-16 (result summary, all green)
--------------------------------------------------------------------------------
Test totals at the end of pass 16: 15 suites, 1,873 checks, 0 failures,
deterministic across repeated runs (-O2 x3, -O0 x2 on Windows w64devkit).
Suites: test_store (88), test_client (262), test_compositor (712),
test_automation (161), test_a11y (50), test_rewind (45), test_shell (95),
test_sessions (94), test_launcher (81), + app/terminal/settings/theme/image/
wallpaper (counts vary; total 1,873).

Pass 4 (scene store core): spec-first wire defect fix (checksum coverage
[0,12+len) -> [0,16+len)); scene_store_import_macro (memory-level macro
transfer between sessions, cross-app automation flow); fixed-size on-stack
walk stacks with heap fallback at all 5 DFS sites; discovered SEMANTIC
TRUTHS: engine requires strictly-consecutive seq starting at 1 per session
(two stores on one global counter = "non-monotonic seq" failure); TEXT_INDEX
must be drained before later outbound assertions; a destroyed node's texts
die with it; ack must cover delivered scene_seq (11, not 9) else flow-control
gate stays shut. Environment fact: w64devkit has NO ASan/UBSan runtimes,
`make test-dbg` dead by environment; use plain -g -O0 + gdb.

Pass 5 (wire client + server seam): scene_transport.c (loopback FIFO pairs +
blocking TCP, WinSock/POSIX); scene_client.c (seq-managed op stream,
WELCOME retained-session continuation vs fresh rebuild, typed dispatch for
all 11 server records, cli_violation closes on protocol violation,
scene_snapshot_parse with cap guards, pump reads at most ONE recv batch per
call for blocking-TCP safety); scene_server.c (raw feed, frame reassembly,
scene_frame_check, ghost reconnect via scene_store_rejoin, op-level errors
emit engine ERROR, frame-level via scene_store_fail). Bug found by wire
tests: arena_alloc free-list reuse path missed (*count)++ while op_destroy
decrements -> live count drifted -> snapshot failed `seen != count` guard;
fixed. All walk sites converted to pre-order document order; subtree destroy
keeps post-order; region_at deepest-wins provably identical.

Pass 6 (compositor core): in-house 8x8 bitmap font (scene_font_data.c,
97-glyph, ASCII 32..126 + box glyph index 96; agent failed twice, we wrote
it — create-our-own rule); scene_compositor.c (~800 lines): open-addressing
render model keyed by node id (power-of-two caps, multiplicative hash, load
0.7); role-based default style table (server-owned dark look); resolve_style
= style ref >=1 -> server styles table, else role default, else GENERIC;
text signature FNV-1a over text_id/len/first-32-bytes (16 slots); paint per
node fill->stroke->texture blit->text rows (glyphs overwrite stroke row);
damage list: frame list reset per frame + pending list for compositor-side
offers merged into frame report (cap 32, merge-into-last); first content
frame clears full fb; texture registry with store_registered flag; set_style
memcmp no-op + dirties referencing visible nodes = LIVE RE-THEME of running
apps. Engine fixes found here: (a) node creates never set blend/opacity ->
default opacity 0 = invisible; fixed at create AND ghost-resurrection (blend
0, opacity 255); (b) scene_store_ghost_mark iterated 1..node_count but arena
is slot-based with gaps -> scan 1..node_cap-1 skipping id==UINT32_MAX free
slots (also caught my own OOB fix: <= node_cap touches nodes[node_cap]);
(c) replay arena needs apply_ctx `replay` flag: op_create treats re-creates
on replay arena as resurrections (ghost pattern). Read views serve ACTIVE
view arena (replay arena in REPLAY mode, live otherwise); new accessor
scene_store_view_seq = compositor diff gate; replay/seek service =
first consumer proof: seek back/forward repaints exactly the differing
rects (2 rects at both seek(4) and seek(24)).

Pass 7 (deterministic effects + rounded corners): scene_fb_chrome_round —
single-pass per-pixel rounded-chrome rasterizer (interior pixels blend fill
once, ring pixels blend border once; radius 0 falls back to fill-over-stroke;
bw=0 -> fill_round; fill=0 -> ring only). rcorner_inset formula corrected to
canonical r - floor(sqrt(2*r*dy - dy^2)). style_chrome rewrite: PANEL
radius=4, BUTTON=4 bw=1, WINDOW=8, SCROLLBAR=0, LABEL=2, IMAGE=0, PROGRESS=2,
CHECKBOX=2 bw=1. Deterministic enter slide (ENTER free frame damages full
slide band [base.y, base.y+h+ANIM_SLIDE]; ANIM_SLIDE=6, ANIM_TICKS=8, off=0
at t=7; enter ramp {65,64,63,63,62,61,60,60}). scene_store_in_replay
accessor (mode-based, replaces seq-gap heuristic that failed at head).
anim_clear_all base-damage + anim_alloc mid-enter band-damage (phantom
cleanup). Effects suite 2->7 tests incl. byte-identical determinism across
two harnesses. Compositor suite 277->712 checks.

Pass 8 (automation consumer): tests/test_automation.c (10 tests, full
loopback with scene_compositor, tickf = flush->recv->feed->drain->pump->
frame x4); search VISIBLE filter (scene_store_search + emit_search_reply
both skip !(flags & SCENE_FLAG_VISIBLE) — the inline reply loop was caught
without the filter and patched); fixes: harness needed scene_server_attach
(welcomed stayed 0), scene_node_id is uint32_t. Custom wlroots compositor
skeleton (src/iso_compositor.c, Linux-only, not compiled on Windows).

Pass 9 (a11y consumer): scene_a11y.h/c built entirely on public store API
(walk, node_vis, texts, child_count, focus); new accessor
scene_store_node_child_count. 6 tests/50 checks. No wire changes; SET_STATE
deferred until apps need to transmit a11y-specific bits.

Pass 10 (rewind consumer): scene_rewind.h/c — modes enter/exit replay,
seek/step/tell/head/tail, diff with created/destroyed/modified delta
arrays via sorted-merge of a11y snaps between two seq points (FNV-1a text
hashing, qsort by id). Store-side direct-mode APIs: scene_store_begin_replay/
end_replay/seek_to bypass the wire (internal consumer). 6 tests. Bugs fixed:
created/destroyed arrays swapped in tail cases; build_app produces 8 ops not
7 (4 creates + 4 texts); determinism test shared a global seq across stores
(needs per-store reset).

Pass 11 (desktop shell): scene_shell.h/c (~470 lines): config parser
(Option=Value, hex ARGB, comma-separated apps), tree build (background
WINDOW, panel PANEL, start_button BUTTON, clock LABEL, menu MENU hidden,
pre-created launcher slots), tick reconciles task list (walk filter
id >= ID_BACKGROUND to exclude shell-owned nodes), handle_activate toggles
start menu + launches via system() + focuses via scene_client_focus, resize
repositions all, clock time() with last_clock_min debounce. shell.conf
defaults: 0xFF1A1A2E bg, 0xFF16213E panel, 32px, 3 launcher apps. 10 tests.

Pass 12 (shell polish): hover effects (absolute-coord hit testing via
get_abs_rect walking parent chain; hover_style = compositor style slot 1;
scene_compositor_setup_hover_style), 12h clock option (clock_12h, " 1:30p"),
config hover_color + clock_12h. scene_store_node_vis returns parent-relative
rects — get_abs_rect must walk parents for screen coords.

Pass 13 (task highlighting): scene_compositor_setup_active_style (slot 2);
scene_shell_set_active_style; per-tick scene_store_focus() query sets
focused window's task button to style 2, revert on unfocus (active_task_id
dedupe); task text refresh moved from create-only to per-tick path.
Focus unfocus uses node 10000 (background), NOT 0 (node 0 doesn't exist).

Pass 14 (multi-session compositor, commit 157f77e): scene_compositor_
add_session attaches foreign session as layer 1..n above shell (layer 0);
each layer owns store + render model + anims; dead app layer freezes
nothing — next frame repaints its area as desktop. Input routing:
pointer hit-tests app layers topmost-first via scene_store_region_at; first
session owning the point receives event + keyboard focus (focus_layer);
empty desktop falls to shell. scene_compositor_remove_session force-full-
repaints the removed layer's area. iso_drm.c integration. test_sessions.c
(94 checks): attach->paint-over-desktop, input to app layer, click-to-
focus + key routing, app-death freeze safe, remove->desktop restored,
shell death fatal, fresh session reuse of vacated layer.

Pass 15 (app launcher, commits a346eb3 + 095738b): scene_tcp_listener APIs
(NON-BLOCKING: scene_tcp_set_nonblock; recv 1 = would-block in non-blocking
mode — blocking TCP recv never returns when drained, a child blocking on a
second recv hangs forever). scene_app_new_on over a TCP target.
scene_launcher.h/c: spawn sets SCENE_STORE_PORT, non-blocking accept ->
scene_server_new + attach + scene_compositor_add_session (joined), per-
session recv/feed/outbound-drain, reap dead sessions with slot compaction,
session_added/session_exited callbacks, spawn timeout (default 10s, never-
connecting apps dropped). test_launcher.c (81 checks, 6 tests incl. child
binary test_launcher_app). KEY FIXES: child welcome-wait used
scene_client_next_seq > 1 which never advances before welcome — added
scene_client_welcomed() accessor (root cause of an 11-failure wall
isolated with 6 probes); scene_compositor_remove_session now removes BY
SERVER IDENTITY not layer index (after first reap compacts the layer array,
a stale s->layer made second removal fail silently, leaving frozen window
painted forever — found via 2-child probe); rn_text_capture dropped unused
cp param; ws_ready guarded behind _WIN32 (musl-gcc warning).
Cycle 14 ISO verification (6c49c45): scene-store builds zero-warning under
musl-gcc on the codespace; ISO 25M; boots under QEMU headless; iso-drm up
at 1280x800@75 with scene nodes=5.

Pass 16 (launcher on the ISO + DRM freeze root-cause, commit e3a731b +
7ce37fe): POSIX spawn PATH resolution (resolve_exe — execl doesn't search
PATH); scene_shell_set_launch_cb (host hook: shell menu fires
scene_launcher_launch); src/iso_demo.c first guest app (window+titlebar+
Go button, auto-ack, event log); iso_drm --autolaunch=NAME (up to 4);
kernel cmdline autolaunch=iso-demo forwarded by overlay etc/init.d/
scene-desktop (overlay OVERRIDES build.sh heredoc via `cp -a overlay/*`
at build.sh ~L357-360); build.sh ships /usr/bin/iso-drm + iso-demo,
launcher_apps=iso-demo. Tests: test_shell_launch_cb, test_menu_launch,
test_iso_demo_app (real iso_demo.exe over real TCP), test_iso_settle
(settle-timeline at ISO cadence: identity 0xFF202020 reached then held
120 frames at 3 probe points). ROOT-CAUSED DRM DOUBLE-BUFFER FREEZE:
first ISO boot showed app frozen at alpha 223/255 (enter anim t=7). Serial
instrumentation (--debug per-second frames/anims/damage/pxFb/pxDumb-both
buffers/flips) showed pxFb settled the whole run but pxDumb (DISPLAYED
buffer) flipped from settled to t=7 state at frame 228 with no blit —
damage-patched double-buffer present: each flip presents the buffer blitted
TWO flips earlier, so a later shell repaint (clock, one damaged frame ~3s
after boot) patched only its small rect into the stale buffer and re-
presented t=7-era content; no further damage -> display froze forever.
FIX scene_drm present_full: every damaged frame memcpys the whole scene fb
into the back buffer before flipping (4MB @ 60Hz — correct by construction;
damage-based present deferred until a per-buffer accumulation scheme
exists). ALSO fixed en route: drm_wait_flip read only sizeof(struct
drm_event) (8B) of the kernel's 32-byte drm_event_vblank page-flip record —
permanent event-queue desync + 100% CPU spin; now reads the full record.
Cycle 17 verification (e3a731b): 45s run + screendump — window 202020,
button 3C3C3C, border 555555, title FFFFFF, desktop 1A1A2E, all exact;
75s run forced the original trigger (clock flip #11 at frame 3561) —
pxDumb=0x202020/0x202020 on BOTH buffers, two screendumps across the
rollover byte-identical and exact. Wire format untouched by the entire pass.

--------------------------------------------------------------------------------
5. CURRENT MILESTONE: DAILY-DRIVER CONVERSION (passes 17-18 area)
--------------------------------------------------------------------------------
Goal (from pass 16 ledger): make the ISO a usable daily driver — real
networking (NIC drivers + DHCP), a package manager (apk), a working terminal
app, verified GRUB ISO boot — then prove in QEMU.

Daily-driver gaps identified: kernel had NO NIC drivers; busybox lacked
udhcpc/wget/ping/nc; apk-tools chosen as package manager (Alpine repos,
static-friendly, self-contained); scene_terminal existed as a LIBRARY only —
no app binary shipped; /etc/shell.conf launcher was iso-demo only.

KERNEL: config fingerprint cached in $SYSROOT/boot/.kconfig-fp (rebuild
skipped if unchanged); NIC drivers BUILT-IN (E1000/E1000E/RTL8139/R8169/
VIRTIO_NET/USB_NET_AX8817X/USB_NET_RTL8152) + SQUASHFS_ZSTD.

BUSYBOX: CONFIG_UDHCPC/WGET/PING/NC enabled.

TOOLCHAIN WRAPPER (critical): musl-gcc wrapper hardcodes -static, which
breaks -shared; created musl-gcc-shared twin that ADDS
-Wl,-dynamic-linker,/lib/ld-musl-x86_64.so.1 — WITHOUT this, plain
gcc --sysroot=... gives every shared binary glibc's
/lib64/ld-linux-x86-64.so.2 interpreter (runtime instant failure).
zlib/openssl/apk all use the shared twin. Verify with: readelf -l
<binary> | grep interp.

LIBRARIES: openssl 3.0.13 built --libdir=/usr/lib; zlib 1.3.1 downloaded
from zlib.net/fossils (NOT zlib.net homepage); both shared/musl-dynamic
(INTERP /lib/ld-musl-x86_64.so.1 confirmed via readelf).

APK-TOOLS 2.14.4: built via ROOT `make` (NOT `make -C src` — root Makefile
sets per-subdir `obj` variable that src/Makefile needs); LUA=no (optional
help module); scdoc installed for docs; LIBDIR := /lib (apk at /sbin/apk,
libapk at /lib). Alpine signing keys: fetched alpine-keys APK from
APKINDEX, keys live at etc/apk/keys/ INSIDE the apk (not archive root);
3 keys installed. CA bundle: CA_CERT_FILE=/etc/apk/ca.pem (libfetch
compile-time) + copies at /etc/ssl/cert.pem (OpenSSL default) and
/etc/ssl/certs/ca-certificates.crt. /etc/apk/repositories = official
latest-stable main+community. /etc/apk/world (empty ok) + /lib/apk/db/
installed must exist for `apk update` (apk 2.14 reads world first).

NETWORKING INIT: iso/overlay/etc/init.d/networking starts
`udhcpc -i $iface -b -q` per NIC, parses pkgtest=PKG cmdline ->
`apk add --no-cache PKG` (serial-log proof gate for QEMU). MUST be
chmod +x — build.sh does this alongside rcS/scene-desktop (was shipped
644, proof failed until fixed).

PROOFS COMPLETED (all verified, do not re-run casually — they take
minutes):
1. APK live chroot proof: apk update -> 28639 packages; apk add
   --no-cache htop -> 5 packages; htop --version runs inside sysroot.
2. QEMU pkgtest proof: `network: pkgtest: htop installed OK` in serial
   log after -kernel/-initrd boot with e1000 + user-net.
3. GRUB ISO boot proof: -cdrom boots GRUB 2.12 -> kernel -> compositor
   (1280x800@75, scene nodes=7). Networking ran.
4. Terminal spawn proof: `iso-drm: spawned 'iso-terminal' pid=105` ->
   app reached "running" (transport ok, app ok, welcome ok, window
   id=40004, terminal ok, running). THE WINDOW THEN PAINTED (fix +
   verification: see §16.2) — QEMU shot shows terminal bg 0xFF0C0C0C at
   the window area (60,40,648,168), prompt glyphs, and title text.

TERMINAL APP WORK: scene_terminal spawn now tries openpty (musl has
pty.h; term->child_write_fd == master for PTY), exec /bin/sh, non-blocking
master; pipe fallback retained; setenv("TERM","scene") in child.
Terminal renders via scene_app_set_text slots — compositor text cap is 16
rows (SCENE_COMPOSITOR_TEXT_CAP), so rows=16, cols=80; added
scene_terminal_view_top() accessor. iso_terminal.c: scene_app_new_on to
127.0.0.1:$SCENE_STORE_PORT, window (60,40,648,168) + titlebar (32px),
close=content-1 (base+3), 500-tick welcome wait, pump+scene_terminal_pump+
render at ~5ms, stage debug prints on stderr. Makefile iso-terminal target
MUST link build/scene_app.o (was missing -> undefined
scene_app_present/flush).

--------------------------------------------------------------------------------
6. RESOLVED BUG: TERMINAL WINDOW NEVER PAINTED ON THE ISO (CLOSED 2026-08-13)
--------------------------------------------------------------------------------
SYMPTOMS (as observed before the fix; live ISO, QEMU, kernel cmdline
autolaunch=iso-terminal):
- iso_terminal connects, receives welcome, creates window, spawns PTY
  shell — ALL confirmed via serial stage prints (reached "running").
- Screenshot shows only the desktop (0xFF1A1A2E) at all pixel coordinates
  including inside the terminal window area.
- Debug mode (iso-drm --debug): pxFb = 0x1A1A2E (compositor framebuffer
  shows desktop only), damage=0 FOREVER, l1=0 (layer-1 store view_seq=0 —
  ZERO ops ingested into the session store).
- Launcher pump byte counter: ZERO bytes ever fed to the session.
- No errors, no exits, no violations on either side; the app keeps
  running and pumping forever.

NOTE ON THE EARLIER "SMOKING GUN": the fake-server test (iso_terminal vs
standalone Python fake_server.py) showed the app sending ZERO bytes over
the wire even though its recv path worked (welcome received). Both that
symptom and the codespace conn=0 / ISO l1=0 manifestations resolved to
ONE root cause — see §16.2 for the full analysis, fix, and verification.

ROOT CAUSE (proven by local repro via fake_server + iso_terminal on
Windows, then confirmed on the codespace with tools/term_probe.sh):
iso_terminal.c's main loop called BLOCKING scene_app_pump as its first
action. The scene server only replies after the client speaks, so the
window ops (336 B) sat un-flushed in c->out while recv() blocked
forever; the launcher fed zero bytes; the compositor never ingested
anything. This violated the Pass-15 launcher-children rule: children
MUST use non-blocking sockets + poll; a child that blocks on its second
recv hangs forever.

FIX (commits 10c4165 + 2f77b4d):
1. iso_terminal.c: scene_tcp_set_nonblock(t, 1) right after
   scene_app_new_on succeeds. Pump now returns would-block immediately;
   the loop keeps flushing every tick.
2. The terminal's CONTENT node now carries SCENE_ROLE_TERMINAL (via the
   new scene_app_create_window_role API) so the compositor paints the
   server-owned terminal look; role_defaults[TERMINAL].fill set to
   0xFF0C0C0C matching the terminal's bg (previously the transparent
   GENERIC content showed the WINDOW 0xFF202020 fill through). Apps
   transmit roles; the OS owns the look — no wire format change.

VERIFICATION CHAIN:
- Windows repro (pre-fix): fake: END frames=0 bytes=0; app stuck with
  cli_emit BLOCKED, out.len=336. Post-fix: END frames=24 bytes=959, all
  checksums OK — deterministic across runs.
- New regression test test_tcp_silent_server_flush (test_client): a
  non-blocking TCP client MUST deliver window ops to a silent server
  without deadlocking (would-block contract + buffered ops reach the
  wire). 283 checks.
- test_comp_terminal_role_fill (test_compositor): TERMINAL role body
  pixels are exactly 0xFF0C0C0C, titlebar 0xFF1A1A1A. test_app gained
  test_create_window_role (role variant + GENERIC default both checked).
  Windows suite: 15 suites / 1,921 checks / 0 failures.
- ISO rebuild (scene+rootfs+initramfs, musl-gcc, 0 warnings) + QEMU shot
  (iso/qemu-proof.sh shot autolaunch=iso-terminal): serial log shows 11
  "cli_emit: op=3 conn=1 wel=1" records (op stream flushing); screendump
  decodes to: body 0xFF0C0C0C at (100,80)/(100,150)/(100,200)/(600,150)/
  (700,200), titlebar 0xFF1A1A1A, desktop 0xFF1A1A2E, 299 prompt glyph
  pixels (bbox x:64-89 = "/ # " row), 195 title-text pixels. HANDOFF §7
  pixel target met exactly.

--------------------------------------------------------------------------------
16.1 THE CODESPACE PROBE (2026-08-13, LATEST STATE OF THIS BUG)
--------------------------------------------------------------------------------
Environment: codespace glowing-memory-gxjv9j7jw65fv997, repo at
/workspaces/scene-store-os (auto-synced from the GitHub repo a-d-j-a/
scene-store-os; we edit locally at C:\Users\khalu\Desktop\iso and push).

What was built:
- iso/tools/fake_server.py: minimal scene-wire server. Accepts ONE
  connection, sends a fresh-session WELCOME (30-byte payload: scene_id u32,
  ver u16, limits u32 x4 + u64 x1 — pack format '<IHIIIIQ'; header 16B:
  magic u32, ver u16, opcode u16 (0x8001), plen u32; FNV1a32 checksum over
  [0,16+plen) with checksum bytes zeroed at compute). Then decodes inbound
  frames for 10s, printing opcode/seq/plen/checksum per frame.
- tools/term_probe.sh (repo root tools/): pkill stale fake_servers, start
  fake_server on 19999 in background, run iso-terminal with
  SCENE_STORE_PORT=19999 under timeout 20, dump both logs.

Debug instrumentation ADDED (commit e1351b5, STILL IN THE CODE — remove
before shipping):
- scene_client.c cli_emit: fprintf stderr op/conn/welcomed/fatal/out.len +
  "BLOCKED" line on reject.
- scene_client.c scene_client_flush: fprintf stderr conn/out.len/out_off.
- scene_transport.c tcp_send: fprintf stderr s/len.
- scene_transport.c tcp_recv (POSIX): fprintf stderr n/errno on error.
- Need to add: welcome dispatch print (that's the ONE key missing log
  inside dispatch(), and receive-path prints for n>0).

FIRST PROBE RUN RESULT (this session, 2026-08-13):
- fake_server FAILED to bind: "OSError: [Errno 98] Address already in
  use" — an orphaned fake_server from a prior killed ssh session was
  STILL listening on 19999 (the kill of the ssh client did not kill the
  remote python).
- iso_terminal output: transport=ok, app=ok, then EVERY flush printed
  "flush: conn=0 out.len=0 out_off=0" (all 500 iterations), welcome
  TIMEOUT (i=500), exit code 5. Also key: `cli_emit` NEVER printed —
  meaning NO op was even attempted before welcome arrived (expected)
  — but conn=0 the whole time is the anomaly. In scene_client_connect,
  conn_open is set to 1 only after a successful scene_transport_open.
  transport=ok + app=ok but conn=0 on ALL flush calls => the connection
  was dropped between connect and first flush, OR transport_open
  succeeded without a real socket (connect() never ran).
- No `tcp_recv:` lines appeared at all => if recv had failed, the new
  debug print should show errno. Their absence means the error paths in
  the window were: pump() -> scene_transport_recv -> tcp_recv where
  n>0 (no print) or would-block (no print). A welcome frame (46 bytes)
  SHOULD eventually arrive and dispatch, unless the peer never sent it
  (orphan server was NOT in accept loop? it had the port...).
- Then the app exit=5 at welcome TIMEOUT and the orphan server ~10s
  window. MOST LIKELY: the orphaned server accepted, sent welcome,
  started decode loop, hit its 10s limit, closed; or it was itself still
  mid-accept and the connection never completed.

NEXT DEBUG STEP (already prepared): rerun term_probe.sh (it now starts with
`pkill -f fake_server.py` + sleep 1, committed in d80e642 with the tcp_recv
errno trace); add one more
print in tcp_recv for the SUCCESS path (n>0 -> fprintf) and in
dispatch() for WELCOME receipt, then rerun to see exactly what the app
receives. Two candidate root causes remain (kill one each run):
(A) server-side: welcome never actually sent/arrives -> app can't
    welcome, never emits, never sends (matches zero-bytes-wire + conn=0);
(B) client-side: scene_client_connect's transport_open doesn't establish
    the socket on musl/POSIX for ip:port targets (getaddrinfo path) so
    conn_open stays 0 -> flush no-ops -> nothing on wire (also matches).

STRACE IS INSTALLED ON THE CODESPACE: `strace -f -e
trace=connect,sendto,recvfrom,send,recv ./build/iso_terminal` against a
background fake_server would show definitively whether connect() and
send() are called and their return values. That is the fastest remaining
tool if the code-path reads don't settle it.

IMPORTANT CONTRAST with the ISO: on the ISO the app DID welcome
(welcome=ok) and l1=0. So on the ISO, connect+recv worked, welcome
arrived, yet the launcher still fed ZERO bytes — meaning the app's SEND
path (or the launcher's recv path) silently produces nothing even though
welcome succeeded. The codespace repro (conn=0) may be a DIFFERENT
manifestation (orphan server artifact) — do not assume one root cause
fits both. On the ISO the question is why, after welcome, the app's
cli_emit+flush+tcp_send chain never puts bytes on the wire (the fake
server probe was intended to answer exactly this — it needs a CLEAN
run).

--------------------------------------------------------------------------------
7. QEMU PROOF HARNESS (iso/qemu-proof.sh)
--------------------------------------------------------------------------------
- Default mode: -nographic serial-proof boot of the SHIPPED kernel +
  initramfs with e1000 + user-net; greps serial log for proof lines
  (pkgtest=... installed OK etc). Uses `timeout ... || true` — the
  `timeout` command returns 124 on expiry and set -e would kill the
  script before post-run diagnostics.
- shot mode: `iso/qemu-proof.sh shot` — boots with -vga std, -monitor
  stdio piped through a subshell so `screendump /tmp/qemu-shot.ppm` can
  be injected; produces a 1280x800 P6 PPM (3MB). Screendumps are via
  monitor command. DONE 2026-08-13: terminal pixel check passed —
  0xFF0C0C0C (terminal bg) at the window area (60,40,648,168) in the
  ppm (see §6).
- GRUB ISO boot proof: `qemu-system-x86_64 -cdrom
  output/iso-custom-6.6.52.iso ...` — grub.cfg has timeout=3, serial
  console; kernel cmdline passes no autolaunch (grub.cfg does NOT
  forward pkgtest= / autolaunch= — that's for -kernel mode via the init
  scripts).

--------------------------------------------------------------------------------
8. CODESPACE / SSH QUOTING RULES (learned the hard way)
--------------------------------------------------------------------------------
- `gh codespace ssh -c <name> -- "<cmd>"`; <name> =
  glowing-memory-gxjv9j7jw65fv997.
- NO inner quotes, pipes, backslashes, or parens in the command string —
  all get mangled. `ps aux | grep x` pipes break grep (grep hangs on
  stdin). Single-token grep patterns with explicit filenames only.
- `;` and `&` and `$` survive inside the double-quoted string when using
  single quotes around the whole remote command (gh passes it to
  bash -c). Background processes started via `&` may die with the ssh
  session (orphan risk — pkill before rebinding ports).
- Write complex commands as REPO SCRIPTS (like term_probe.sh), commit,
  push, then run `gh codespace ssh ... 'git pull; ./tools/term_probe.sh'`.
- The local tree C:\Users\khalu\Desktop\iso IS the git repo (root), with
  scene-store/, iso/, tools/ subdirs. Local git is NOT on PATH: use
  & 'C:\Program Files\Git\cmd\git.exe' or gh bundles its own.
- GitHub CLI at & 'C:\Program Files\GitHub CLI\gh.exe'.

--------------------------------------------------------------------------------
9. BUILD / TEST COMMANDS (both platforms)
--------------------------------------------------------------------------------
Windows (w64devkit at C:\Users\khalu\AppData\Local\Tools\w64devkit):
- make all (15 test binaries + iso_preview + iso_demo)
- Run each: build/test_*.exe, or `make test-<suite>`.
- No ASan/UBSan in w64devkit; debug = plain -g -O0 + gdb.
Codespace (musl-gcc cross build of the ISO):
- make -C scene-store iso-drm iso-terminal iso-demo (0 warnings target)
- iso/build.sh builds the FULL ISO (kernel with NICs, busybox applets,
  zlib/openssl/apk, overlay init scripts, scene binaries via musl-gcc-
  shared twin, pack initramfs+squashfs, grub ISO).
- Output: output/iso-custom-6.6.52.iso (~31MB after scene rebuild).

--------------------------------------------------------------------------------
10. REPO LAYOUT
--------------------------------------------------------------------------------
Repo root C:\Users\khalu\Desktop\iso  (git remote origin
https://github.com/a-d-j-a/scene-store-os.git; branch master; local git
at 'C:\Program Files\Git\cmd\git.exe')
- AGENTS.md               — rules + full research/implementation ledger
- scene-store-spec.md     — the LOCKED wire format v0
- scene-store/            — engine src/include/tests/Makefile
  - src/: scene_fmt.{c,h} scene_store scene_transport scene_client
    scene_server scene_fb scene_font(+data) scene_compositor scene_a11y
    scene_rewind scene_shell scene_settings scene_theme scene_image
    scene_wallpaper scene_launcher scene_app scene_terminal
    iso_drm.c (DRM/KMS compositor, /dev/dri/*, no external libs)
    iso_demo.c (first guest app) iso_terminal.c (new terminal app)
    iso_preview.c (Windows GDI live preview) iso_compositor.c (wlroots
    skeleton, Linux-only, not built by default)
  - include/: matching headers
  - tests/: test_store client compositor automation a11y rewind shell
    app terminal sessions launcher settings theme image wallpaper
    launcher_app (child binary)
  - tools/: bench.c preview_dump.c
- iso/                     — build + boot files
  - build.sh               — full ISO build; phases include kernel
    (config fingerprint), busybox, musl-gcc-shared, zlib, openssl, apk,
    CA bundle, overlay, scene binaries, initramfs+squashfs, GRUB ISO
  - qemu-proof.sh          — QEMU proof harness (serial proof + shot)
  - grub.cfg               — GRUB menu (timeout=3, serial console)
  - overlay/etc/init.d/rcS, scene-desktop, networking
  - initramfs/init
  - tools/fake_server.py   — minimal scene-wire server (debug)
  - packages.txt, Makefile, .devcontainer/devcontainer.json
- tools/term_probe.sh      — codespace debug harness (fake_server +
  iso_terminal; pkill stale servers first)
- session-handoff.txt      — PREVIOUS handoff (passes 4-7 era; superseded
  by this file — keep for archaeology only)
- scratch evidence: scr16.png, scr18.b64, scr19.b64, serial16/17.log,
  serial.log, *.ppm, boot-scr1.png (some are accidentally committed;
  clean them out of the repo with a rm commit when convenient)

--------------------------------------------------------------------------------
11. GIT LOG (recent, oldest->newest this session era)
--------------------------------------------------------------------------------
... 2f7c42e/f18cabe cycle 13 results
157f77e multi-session compositor + test_sessions
a346eb3 app launcher + test_launcher
095738b transport ws_ready guard
6c49c45 cycle 14 results
eac8e51 launcher on the ISO: guest apps from shell menu
1b2641c iso-drm --autolaunch + overlay forwards autolaunch=
30592ba, a24f04c, 888213e iso-demo/iso-drm musl fixes
d7693da overlay scene-desktop cmdline forwarding
6f5befe DRM flip-event fix + settle-timeline test
e3a731b present_full fix + buffer probes
7ce37fe pass 16 ledger docs
3c27fcc DAILY-DRIVER BASE: terminal app + networking + apk + kconfig fp
0a3748d scene-terminal _POSIX_C_SOURCE
b63afe8 zlib fossils URL
23c629b musl-gcc-shared twin
5446715 zlib CC shared twin
fed9fd1 apk via root make
6ffdbd4 LUA=no
dcf2bff root make + scdoc
40cf8a3 alpine-keys path fix
7de7d73 musl dynamic-linker pin
dd43ba2 CA bundle paths
9f7ebc8 qemu-proof.sh
f13795a timeout fix
9b520b6 qemu-proof shot mode
de9bf0f chmod networking
43c02ce scene_app.o link fix
6d7d76a iso_terminal stage prints
265545a debug: launcher feed bytes + layer seq
6895c38 rm stray logs
ef5cd27 fake_server.py
92017e8 fake_server pack format fix
e1351b5 debug trace cli_emit/flush/tcp_send  <== REMOVED by eddb960
10c4165 iso_terminal: fix never-paints on ISO — non-blocking transport
2f77b4d terminal paints its own background: content node SCENE_ROLE_TERMINAL
        + role default fill 0xFF0C0C0C
5b9c4f6 docs: close terminal-paint bug in handoff (root cause, fix, proof)
eddb960 perf: strip debug fprintf instrumentation
7e139eb docs: usage guide, agents ledger close-out, handoff status
ceee597 persist: disk persistence via format+switch_root, pkgtest=check:,
         GRUB photo-entry proof config
884497a shell: pointer-driven window resize (edges, corner, min 96x64)
990960c iso-video: texture-streaming demo app + launcher-harness test
ec27e92 docs: pass-19 ledger, handoff record, usage guide   <== LATEST HEAD

STALE BEFORE THESE: passes 1-13 commits are earlier in the log; the
ledger in AGENTS.md covers them in detail.

--------------------------------------------------------------------------------
12. KNOWN LIMITATIONS / DEFERRED (tracked, not silently dropped)
--------------------------------------------------------------------------------
- region_at / search are O(N) linear scans (fine at 10k nodes, sub-200µs;
  spatial/text index = later optimization).
- Macros per-session by design; cross-session via scene_store_import_macro.
- Browser/video/WebGL = composited textures only (honest boundary).
- present_full memcpys 4MB every damaged frame — correct-by-construction;
  damage-based present deferred until per-buffer accumulation exists.
- w64devkit: no ASan/UBSan; test-dbg dead by environment.
- INITRD IS FULLY RAM-RESIDENT; rootfs drops static .a + strips shared
  libs (ISAMU-style). RAM mode is still the DEFAULT boot (initramfs = the
  whole rootfs). Superseded 2026-08-13: `persist=DEV` (or `persist=auto`)
  now formats a blank disk on first boot, copies the ramfs rootfs onto it,
  and every boot mounts the disk and switch_root's into it — the disk, not
  RAM, is the running system; `apk add` installs, `/etc`, `/home` survive
  reboots. Two-boot proof on the codespace (htop boot 1 → check:htop
  boot 2 `present OK`, no re-copy). See §16.3.
- The debug fprintf traces in scene_client.c/scene_transport.c MUST be
  removed before shipping (or gated behind an env var). (REMOVED —
  commit eddb960, 2026-08-13: cli_emit/flush/tcp_send/tcp_recv prints
  deleted from scene_client.c, scene_transport.c, scene_launcher.c;
  Windows suite re-run 1,921/0, ISO rebuilt, QEMU shot serial log clean —
  zero cli_emit lines, pixels unchanged.)
- gh codespace orphan processes: pkill stale servers before rebinding.

--------------------------------------------------------------------------------
13. WHAT'S NEXT (ordered)
-------------------------------------------------------------------------------
DONE this session (2026-08-13): root cause found + fixed (blocking pump
-> non-blocking transport, 10c4165); terminal bg via SCENE_ROLE_TERMINAL
(2f77b4d); Windows suite 1,921 checks green; ISO rebuilt; QEMU shot
proves terminal paints 0xFF0C0C0C at the window area. See §6.
Debug instrumentation stripped (eddb960), suite re-run green, ISO
rebuilt, second QEMU shot with a clean serial log and identical pixels.
iso/USAGE.md written. AGENTS.md ledger updated (pass 19: persistence,
GRUB proof, shell resize).
1. DONE — Remove the debug fprintf instrumentation (eddb960: cli_emit/
   flush/tcp_send/tcp_recv + launcher feed prints; suite still 1,921/0;
   QEMU re-shot clean: 0 cli_emit lines, pixels unchanged).
2. DONE — iso/USAGE.md written + updated (persistence, GRUB proof
   modes, window resize, iso-video).
3. DONE — AGENTS.md ledger updated with pass 19 (ceee597 persistence
   mode, 884497a shell resize, 990960c iso-video) — see §16.3.
4. NEXT — codespace: rebuild ISO with pass-18/19 changes (`sh iso/
   build.sh scene` + rootfs + initramfs + iso) and QEMU-proof
   `autolaunch=iso-video` (screendump: content 0xFF120040-gradient
   frames, titlebar 0xFF1A1A1A, desktop 0xFF1A1A2E; serial log: welcome
   ok, no cli_emit lines) — the one unverified item this session.
5. NEXT — shell visual polish: task-button styling, menu hover
   effects, system tray (ISO as demonstration target). Or wlroots
   compositor integration (skeleton exists, needs a Linux build env to
   compile the skeleton + test). Or cross-app automation service
   (macro record/replay surfaced in the shell).
6. DEFERRED — release gap list: real-hardware bring-up, storage/file
   manager, power management.

--------------------------------------------------------------------------------
14. OPEN QUESTION WORTH FORMALIZING
--------------------------------------------------------------------------------
The ISO shows welcome=ok + window built but l1=0/zero feed bytes; the
codespace shows conn=0 throughout. If both are real (not artifacts),
they point to one structural suspect: scene_client_flush no-ops unless
conn_open && ... and conn_open appears to be 0 in both runs at the
critical moment — on the ISO, the app's welcome wait loop pumps+flushes
500 times; if conn_open were 0 there, welcome could NEVER come (pump
guards on conn_open: scene_client_pump returns -1 immediately if
!conn_open — see line 646). Yet welcome=ok happened ON the ISO. So the
ISO's conn_open WAS 1 (or became 1), while the codespace's stayed 0.
=> Search for anything that RESETS conn_open after connect on the client
   side (scene_client_reconnect? transport reuse? scene_client_free? a
   pump->recv errno-1 path that fires spuriously?) and anything that
   makes the launcher's server never see data (server attach timing,
   buffer not drained, non-blocking recv contract, listener accept vs
   scene_server_new wiring). Compare against test_launcher which passes
   with real TCP + real child on Windows.

RESOLVED 2026-08-13: the codespace conn=0 runs were an orphaned
fake_server artifact plus the blocking-pump deadlock; the ISO welcome=ok
+ l1=0 is fully explained by the app blocking in recv() before its first
flush (window ops stuck in c->out, launcher never sees bytes). No
conn_open reset exists; the two "contradictory" observations were the
same bug seen from two angles. Fix and proof: §6.

--------------------------------------------------------------------------------
16.3 POST-MILESTONE RECORD (2026-08-13): PERSISTENCE + GRUB PROOF + RESIZE
--------------------------------------------------------------------------------
Commits ceee597 (persist + check/pkgtest + GRUB proof config), 884497a
(shell pointer resize), 990960c (iso-video; pass-18 content), docs commit
on top. Windows tree re-verified clean: 16 suites / 2,080 checks /
0 failures on the final tree (store 88, client 283, compositor 766,
automation 161, a11y 50, rewind 45, shell 200, app 52, terminal 9,
sessions 94, launcher 128, settings 43, theme 30, image 17, wallpaper
60, video_app 54). All committed to GitHub.

PERSISTENCE (iso/initramfs/init + networking + build.sh + qemu-proof.sh):
- `persist=DEV` / `persist=auto` (first of vda/sda/hda). First boot with
  a blank disk: mkfs ext2, copy the whole ramfs rootfs onto /mnt/root,
  write /mnt/root/.iso-rootfs-v1 marker. Every boot: mount disk root,
  switch_root — the disk is the running system. RAM mode (initramfs =
  rootfs, exec /sbin/init) remains the default and is untouched.
- `pkgtest=check:PKG` (networking): `command -v PKG` → prints to serial
  `present OK` / `absent FAIL` after the lease; proves installs survive.
- Proof (codespace): boot 1 `pkgtest=htop` → apk add OK; boot 2
  `pkgtest=check:htop` → `present OK`, no rootfs re-copy (marker + tail
  check). Serial logs in the codespace era; rerun with:
  `sh iso/qemu-proof.sh disk=state.img pkgtest=htop` then
  `sh iso/qemu-proof.sh disk=state.img pkgtest=check:htop`.

GRUB -cdrom BOOT PROOF (iso/grub-proof.cfg + build.sh `iso` phase):
- `sh iso/build.sh iso iso-terminal /dev/vda` appends the tracked
  grub-proof.cfg entries to the ISO GRUB menu (photo apps list included;
  proof entry: `autolaunch=iso-terminal persist=/dev/vda`, default=4).
- QEMU `-cdrom output/iso-...iso` (no -kernel/-initrd): GRUB menu → proof
  entry → kernel + initramfs load, persist disk mounts, switch_root, and
  the terminal paints. Screendump: body 0xFF0C0C0C, titlebar 0xFF1A1A1A,
  desktop 0xFF1A1A2E — identical to the -kernel mode. Serial log carries
  the switch_root/persist proof lines.

SHELL WINDOW RESIZE (scene_shell.c + test_shell.c, ~450 test lines):
- scene_shell_handle_pointer drag-resizes windows: right-edge (w += dx,
  min 96), bottom-edge (h += dy, min 64), bottom-right corner (both).
  Hit zones: within 4px of the edge, computed from the window's absolute
  rect. titlebar / close button / content node rects re-derive per tick
  from the window rect; active-task button and resize state survive.
- Tests: per-edge resize, both-edge corner, min-size clamp, resize then
  move, non-resize clicks pass through, determinism (two shells,
  identical pointer sequences → identical tree).

iso-video (990960c) — see AGENTS.md pass 18 for the full record;
its Windows-side proof (test_video_app, 54 checks) is in this tree.
VERIFIED 2026-08-14 (commits 6e5949a, 519cb4d, 1ad8386, f07b08a):
the ISO rebuild + GRUB proof now targets `autolaunch=iso-video`
(iso/grub-proof.cfg updated, 45s→80s screendump wait in qemu-proof.sh
for fresh persist-disk first boot). QEMU screendump pixel truth
(1280x800, video window at 100,50 240x160, titlebar 32px):
- titlebar (100,50)/(200,60) = 0xFF1A1A1A, desktop (700,700) =
  0xFF1A1A2E, panel (640,790) = 0xFF16213E — exact.
- content: left half (100,y) = R=(n*29+(y-82))&0xFF, G=00, B=40;
  right half (250,y) = same R, G=FF, B=80 — exact for rows 82..208
  (gradient 0xB7..0x35 mod 256 verified at 11 probes). The stream
  was live (serial logged frames to 6050+; px= values match the
  decoder formula bit-for-bit).
- Fixes found by this verification: (a) 6e5949a — iso_drm had no
  OS-side texture importer; the app's SET_TEXTURE ref-1 hit an
  unregistered store ref → engine rejected the op → launcher reaped
  the session ("app N exited layer 1"), desktop-only screen. Added
  scene_compositor_layer_store() + importer_tick() (compositor
  register per frame; store pre-seed once at session add). (b)
  519cb4d — iso_drm.c build fix (same importer call signature
  correction, musl clean). (c) 1ad8386 — real channel-mask bug in
  ALL THREE sites (iso_video.c:93, iso_drm.c:315,
  test_video_app.c:96/113): 0x00FF0080 puts 0xFF in the R channel
  (bits 23-16), not G — the QEMU pixel truth showed right half as
  (R=FF, G=00, B=80). Intended G=0xFF,B=0x80 is 0x0000FF80; fixed
  everywhere; Windows suite 16/2,080/0 re-verified. (d) f07b08a —
  proof entry + wait committed (was codespace scratch sed).
- Clean-tree final proof: repo pulled to f07b08a, tree empty
  (git status clean), full ISO rebuilt from committed sources,
  proof rerun: pixels byte-identical to the targets above.
  THE PASS-18 ISO VERIFICATION ITEM IS NOW CLOSED.