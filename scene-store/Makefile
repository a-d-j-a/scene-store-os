CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -O2 -Iinclude
CFLAGS_DBG = -std=c11 -Wall -Wextra -g -fsanitize=address,undefined \
             -Iinclude

# Cross-platform: detect OS
UNAME_S := $(shell uname -s 2>/dev/null || echo Windows_NT)
IS_WINDOWS := $(strip $(findstring MINGW,$(UNAME_S))$(findstring MSYS,$(UNAME_S))$(findstring CYGWIN,$(UNAME_S))$(findstring Windows_NT,$(UNAME_S)))
ifdef IS_WINDOWS
  EXE    = .exe
  SOCKLIB = -lws2_32
else
  EXE    =
  SOCKLIB =
endif

all: build/test_store$(EXE) build/test_client$(EXE) build/test_compositor$(EXE) build/test_automation$(EXE) build/test_a11y$(EXE) build/test_rewind$(EXE) build/test_shell$(EXE) build/test_app$(EXE) build/test_terminal$(EXE) build/test_settings$(EXE) build/test_theme$(EXE) build/test_image$(EXE) build/test_wallpaper$(EXE) build/test_sessions$(EXE) build/test_launcher$(EXE) build/test_launcher_app$(EXE) build/iso_preview$(EXE) build/iso_demo$(EXE) build/test_video_app$(EXE) build/iso_video$(EXE) build/test_app_wm$(EXE) build/test_hostwm$(EXE) build/test_shellwm$(EXE) build/iso_play$(EXE) build/iso_install$(EXE) build/test_play_app$(EXE) build/iso_files$(EXE) build/test_files_app$(EXE) build/iso_photo$(EXE) build/test_photo_app$(EXE) build/test_searchoverlay$(EXE) build/iso_edit$(EXE) build/test_edit_app$(EXE) build/test_codec$(EXE)

# Vendored ffmpeg static build (see third_party/ffmpeg/LICENSE.md for
# the locked configure and the written adoption justification). The ISO
# build overrides FFMPEG_DIR with its own musl static build (build.sh
# phase 5a); Windows uses the vendored mingw libs.
FFMPEG_DIR ?= third_party/ffmpeg
FFMPEG_INC  = -I$(FFMPEG_DIR)/include
FFMPEG_LIBS = $(FFMPEG_DIR)/lib/libavformat.a \
              $(FFMPEG_DIR)/lib/libavcodec.a \
              $(FFMPEG_DIR)/lib/libswscale.a \
              $(FFMPEG_DIR)/lib/libavutil.a
ifdef IS_WINDOWS
  FFMPEG_EXTRA = -lbcrypt          # avutil random_seed on mingw
else
  FFMPEG_EXTRA = -lm -lz -pthread
endif

build:
	mkdir -p build

build/scene_fmt.o: src/scene_fmt.c include/scene_fmt.h | build
	$(CC) $(CFLAGS) -c src/scene_fmt.c -o $@

build/scene_store.o: src/scene_store.c include/scene_store.h include/scene_fmt.h | build
	$(CC) $(CFLAGS) -c src/scene_store.c -o $@

build/scene_transport.o: src/scene_transport.c include/scene_transport.h | build
	$(CC) $(CFLAGS) -c src/scene_transport.c -o $@

build/scene_client.o: src/scene_client.c include/scene_client.h include/scene_transport.h include/scene_fmt.h | build
	$(CC) $(CFLAGS) -c src/scene_client.c -o $@

build/scene_server.o: src/scene_server.c include/scene_server.h include/scene_store.h include/scene_fmt.h include/scene_transport.h | build
	$(CC) $(CFLAGS) -c src/scene_server.c -o $@

build/scene_fb.o: src/scene_fb.c include/scene_fb.h include/scene_fmt.h | build
	$(CC) $(CFLAGS) -c src/scene_fb.c -o $@

build/scene_font.o: src/scene_font.c include/scene_font.h include/scene_fb.h | build
	$(CC) $(CFLAGS) -c src/scene_font.c -o $@

build/scene_font_data.o: src/scene_font_data.c include/scene_font.h | build
	$(CC) $(CFLAGS) -c src/scene_font_data.c -o $@

build/scene_compositor.o: src/scene_compositor.c include/scene_compositor.h include/scene_fb.h include/scene_font.h include/scene_server.h include/scene_store.h include/scene_fmt.h | build
	$(CC) $(CFLAGS) -c src/scene_compositor.c -o $@

build/scene_a11y.o: src/scene_a11y.c include/scene_a11y.h include/scene_store.h include/scene_fmt.h | build
	$(CC) $(CFLAGS) -c src/scene_a11y.c -o $@

build/scene_rewind.o: src/scene_rewind.c include/scene_rewind.h include/scene_store.h include/scene_a11y.h include/scene_fmt.h | build
	$(CC) $(CFLAGS) -c src/scene_rewind.c -o $@

build/scene_shell.o: src/scene_shell.c include/scene_shell.h include/scene_client.h include/scene_store.h include/scene_a11y.h include/scene_fmt.h | build
	$(CC) $(CFLAGS) -c src/scene_shell.c -o $@

build/scene_app.o: src/scene_app.c include/scene_app.h include/scene_client.h include/scene_transport.h include/scene_store.h include/scene_fmt.h | build
	$(CC) $(CFLAGS) -c src/scene_app.c -o $@

build/scene_launcher.o: src/scene_launcher.c include/scene_launcher.h include/scene_compositor.h include/scene_transport.h | build
	$(CC) $(CFLAGS) -c src/scene_launcher.c -o $@

build/scene_terminal.o: src/scene_terminal.c include/scene_terminal.h include/scene_app.h | build
	$(CC) $(CFLAGS) -c src/scene_terminal.c -o $@

build/scene_settings.o: src/scene_settings.c include/scene_settings.h include/scene_shell.h include/scene_client.h include/scene_store.h | build
	$(CC) $(CFLAGS) -c src/scene_settings.c -o $@

build/scene_theme.o: src/scene_theme.c include/scene_theme.h include/scene_shell.h | build
	$(CC) $(CFLAGS) -c src/scene_theme.c -o $@

build/scene_image.o: src/scene_image.c include/scene_image.h | build
	$(CC) $(CFLAGS) -c src/scene_image.c -o $@

build/scene_wallpaper.o: src/scene_wallpaper.c include/scene_wallpaper.h include/scene_image.h | build
	$(CC) $(CFLAGS) -c src/scene_wallpaper.c -o $@

build/scene_codec.o: src/scene_codec.c include/scene_codec.h | build
	$(CC) $(CFLAGS) $(FFMPEG_INC) -c src/scene_codec.c -o $@

build/test_codec$(EXE): tests/test_codec.c build/scene_codec.o | build
	$(CC) $(CFLAGS) tests/test_codec.c build/scene_codec.o \
		$(FFMPEG_LIBS) $(FFMPEG_EXTRA) -o $@

build/test_store$(EXE): tests/test_store.c build/scene_fmt.o build/scene_store.o | build
	$(CC) $(CFLAGS) tests/test_store.c build/scene_fmt.o build/scene_store.o -o $@

build/test_client$(EXE): tests/test_client.c build/scene_fmt.o build/scene_store.o \
	build/scene_transport.o build/scene_client.o build/scene_server.o | build
	$(CC) $(CFLAGS) tests/test_client.c build/scene_fmt.o build/scene_store.o \
		build/scene_transport.o build/scene_client.o build/scene_server.o \
		$(SOCKLIB) -o $@

build/test_compositor$(EXE): tests/test_compositor.c build/scene_fmt.o \
	build/scene_store.o build/scene_transport.o build/scene_client.o \
	build/scene_server.o build/scene_fb.o build/scene_font.o \
	build/scene_font_data.o build/scene_compositor.o | build
	$(CC) $(CFLAGS) tests/test_compositor.c build/scene_fmt.o build/scene_store.o \
		build/scene_transport.o build/scene_client.o build/scene_server.o \
		build/scene_fb.o build/scene_font.o build/scene_font_data.o \
		build/scene_compositor.o $(SOCKLIB) -o $@

build/test_automation$(EXE): tests/test_automation.c build/scene_fmt.o \
	build/scene_store.o build/scene_transport.o build/scene_client.o \
	build/scene_server.o build/scene_fb.o build/scene_font.o \
	build/scene_font_data.o build/scene_compositor.o | build
	$(CC) $(CFLAGS) tests/test_automation.c build/scene_fmt.o build/scene_store.o \
		build/scene_transport.o build/scene_client.o build/scene_server.o \
		build/scene_fb.o build/scene_font.o build/scene_font_data.o \
		build/scene_compositor.o $(SOCKLIB) -o $@

build/test_a11y$(EXE): tests/test_a11y.c build/scene_fmt.o build/scene_store.o \
	build/scene_a11y.o | build
	$(CC) $(CFLAGS) tests/test_a11y.c build/scene_fmt.o build/scene_store.o \
		build/scene_a11y.o -o $@

build/test_rewind$(EXE): tests/test_rewind.c build/scene_fmt.o build/scene_store.o \
	build/scene_a11y.o build/scene_rewind.o | build
	$(CC) $(CFLAGS) tests/test_rewind.c build/scene_fmt.o build/scene_store.o \
		build/scene_a11y.o build/scene_rewind.o -o $@

build/test_shell$(EXE): tests/test_shell.c build/scene_fmt.o build/scene_store.o \
	build/scene_transport.o build/scene_client.o build/scene_server.o \
	build/scene_fb.o build/scene_font.o build/scene_font_data.o \
	build/scene_compositor.o build/scene_a11y.o build/scene_shell.o \
	build/scene_wallpaper.o build/scene_image.o | build
	$(CC) $(CFLAGS) tests/test_shell.c build/scene_fmt.o build/scene_store.o \
		build/scene_transport.o build/scene_client.o build/scene_server.o \
		build/scene_fb.o build/scene_font.o build/scene_font_data.o \
		build/scene_compositor.o build/scene_a11y.o build/scene_shell.o \
		build/scene_wallpaper.o build/scene_image.o \
		$(SOCKLIB) -o $@

build/test_app$(EXE): tests/test_app.c build/scene_fmt.o build/scene_store.o \
	build/scene_transport.o build/scene_client.o build/scene_server.o \
	build/scene_fb.o build/scene_font.o build/scene_font_data.o \
	build/scene_compositor.o build/scene_app.o | build
	$(CC) $(CFLAGS) tests/test_app.c build/scene_fmt.o build/scene_store.o \
		build/scene_transport.o build/scene_client.o build/scene_server.o \
		build/scene_fb.o build/scene_font.o build/scene_font_data.o \
		build/scene_compositor.o build/scene_app.o \
		$(SOCKLIB) -o $@

build/test_terminal$(EXE): tests/test_terminal.c build/scene_fmt.o build/scene_store.o \
	build/scene_transport.o build/scene_client.o build/scene_server.o \
	build/scene_fb.o build/scene_font.o build/scene_font_data.o \
	build/scene_compositor.o build/scene_app.o build/scene_terminal.o | build
	$(CC) $(CFLAGS) tests/test_terminal.c build/scene_fmt.o build/scene_store.o \
		build/scene_transport.o build/scene_client.o build/scene_server.o \
		build/scene_fb.o build/scene_font.o build/scene_font_data.o \
		build/scene_compositor.o build/scene_app.o build/scene_terminal.o \
		$(SOCKLIB) -o $@

build/test_sessions$(EXE): tests/test_sessions.c build/scene_fmt.o \
	build/scene_store.o build/scene_transport.o build/scene_client.o \
	build/scene_server.o build/scene_fb.o build/scene_font.o \
	build/scene_font_data.o build/scene_compositor.o build/scene_app.o | build
	$(CC) $(CFLAGS) tests/test_sessions.c build/scene_fmt.o build/scene_store.o \
		build/scene_transport.o build/scene_client.o build/scene_server.o \
		build/scene_fb.o build/scene_font.o build/scene_font_data.o \
		build/scene_compositor.o build/scene_app.o \
		$(SOCKLIB) -o $@

build/test_launcher$(EXE): tests/test_launcher.c build/scene_fmt.o \
	build/scene_store.o build/scene_transport.o build/scene_client.o \
	build/scene_server.o build/scene_fb.o build/scene_font.o \
	build/scene_font_data.o build/scene_compositor.o build/scene_app.o \
	build/scene_launcher.o build/scene_a11y.o build/scene_shell.o \
	build/scene_wallpaper.o build/scene_image.o | build
	$(CC) $(CFLAGS) tests/test_launcher.c build/scene_fmt.o \
		build/scene_store.o build/scene_transport.o build/scene_client.o \
		build/scene_server.o build/scene_fb.o build/scene_font.o \
		build/scene_font_data.o build/scene_compositor.o build/scene_app.o \
		build/scene_launcher.o build/scene_a11y.o build/scene_shell.o \
		build/scene_wallpaper.o build/scene_image.o \
		$(SOCKLIB) -o $@

build/test_launcher_app$(EXE): tests/test_launcher_app.c build/scene_fmt.o \
	build/scene_transport.o build/scene_client.o build/scene_app.o | build
	$(CC) $(CFLAGS) tests/test_launcher_app.c build/scene_fmt.o \
		build/scene_store.o build/scene_transport.o build/scene_client.o \
		build/scene_app.o $(SOCKLIB) -o $@

# Guest app binary: the first app shipped on the ISO (spawned by the
# launcher; connects over TCP via SCENE_STORE_PORT).
build/iso_demo$(EXE): src/iso_demo.c build/scene_fmt.o \
	build/scene_transport.o build/scene_client.o build/scene_app.o | build
	$(CC) $(CFLAGS) src/iso_demo.c build/scene_fmt.o \
		build/scene_store.o build/scene_transport.o build/scene_client.o \
		build/scene_app.o $(SOCKLIB) -o $@

build/iso_video$(EXE): src/iso_video.c build/scene_fmt.o \
	build/scene_transport.o build/scene_client.o build/scene_app.o | build
	$(CC) $(CFLAGS) src/iso_video.c build/scene_fmt.o \
		build/scene_store.o build/scene_transport.o build/scene_client.o \
		build/scene_app.o $(SOCKLIB) -o $@

build/test_video_app$(EXE): tests/test_video_app.c build/scene_fmt.o \
	build/scene_store.o build/scene_transport.o build/scene_client.o \
	build/scene_server.o build/scene_fb.o build/scene_font.o \
	build/scene_font_data.o build/scene_compositor.o | build
	$(CC) $(CFLAGS) tests/test_video_app.c build/scene_fmt.o \
		build/scene_store.o build/scene_transport.o build/scene_client.o \
		build/scene_server.o build/scene_fb.o build/scene_font.o \
		build/scene_font_data.o build/scene_compositor.o \
		$(SOCKLIB) -o $@

# App-side WM suite (titlebar drag move + edge/corner resize)
build/test_app_wm$(EXE): tests/test_app_wm.c build/scene_fmt.o \
	build/scene_store.o build/scene_transport.o build/scene_client.o \
	build/scene_server.o build/scene_fb.o build/scene_font.o \
	build/scene_font_data.o build/scene_compositor.o build/scene_app.o | build
	$(CC) $(CFLAGS) tests/test_app_wm.c build/scene_fmt.o \
		build/scene_store.o build/scene_transport.o build/scene_client.o \
		build/scene_server.o build/scene_fb.o build/scene_font.o \
		build/scene_font_data.o build/scene_compositor.o build/scene_app.o \
		$(SOCKLIB) -o $@

# Host-side WM service suite (engine host APIs, no wire bytes)
build/test_hostwm$(EXE): tests/test_hostwm.c build/scene_fmt.o \
	build/scene_store.o | build
	$(CC) $(CFLAGS) tests/test_hostwm.c build/scene_fmt.o \
		build/scene_store.o -o $@

# Shell WM + network tray suite (taskbar over app layers)
build/test_shellwm$(EXE): tests/test_shellwm.c build/scene_fmt.o \
	build/scene_store.o build/scene_transport.o build/scene_client.o \
	build/scene_server.o build/scene_fb.o build/scene_font.o \
	build/scene_font_data.o build/scene_compositor.o build/scene_a11y.o \
	build/scene_shell.o build/scene_wallpaper.o build/scene_image.o | build
	$(CC) $(CFLAGS) tests/test_shellwm.c build/scene_fmt.o \
		build/scene_store.o build/scene_transport.o build/scene_client.o \
		build/scene_server.o build/scene_fb.o build/scene_font.o \
		build/scene_font_data.o build/scene_compositor.o build/scene_a11y.o \
		build/scene_shell.o build/scene_wallpaper.o build/scene_image.o \
		$(SOCKLIB) -o $@

# First-party audio player guest app (raw ALSA PCM UAPI on the ISO;
# _WIN32 guards compile a null sink on Windows for the test suite).
# -lm: the SYN generator uses sin()/fabs() (musl keeps libm separate).
build/iso_play$(EXE): src/iso_play.c build/scene_fmt.o \
	build/scene_transport.o build/scene_client.o build/scene_app.o | build
	$(CC) $(CFLAGS) src/iso_play.c build/scene_fmt.o \
		build/scene_store.o build/scene_transport.o build/scene_client.o \
		build/scene_app.o $(SOCKLIB) -lm -o $@

# First-party installer guest app (POSIX fdisk/mke2fs/mount/grub steps;
# _WIN32 guards compile stub steps so Windows catches scene wiring errors)
build/iso_install$(EXE): src/iso_install.c build/scene_fmt.o \
	build/scene_transport.o build/scene_client.o build/scene_app.o | build
	$(CC) $(CFLAGS) src/iso_install.c build/scene_fmt.o \
		build/scene_store.o build/scene_transport.o build/scene_client.o \
		build/scene_app.o $(SOCKLIB) -o $@

build/test_play_app$(EXE): tests/test_play_app.c build/scene_fmt.o \
	build/scene_store.o build/scene_transport.o build/scene_client.o \
	build/scene_server.o build/scene_fb.o build/scene_font.o \
	build/scene_font_data.o build/scene_compositor.o build/scene_app.o \
	build/scene_launcher.o build/scene_a11y.o build/scene_shell.o \
	build/scene_wallpaper.o build/scene_image.o | build
	$(CC) $(CFLAGS) tests/test_play_app.c build/scene_fmt.o \
		build/scene_store.o build/scene_transport.o build/scene_client.o \
		build/scene_server.o build/scene_fb.o build/scene_font.o \
		build/scene_font_data.o build/scene_compositor.o build/scene_app.o \
	build/scene_launcher.o build/scene_a11y.o build/scene_shell.o \
	build/scene_wallpaper.o build/scene_image.o \
	$(SOCKLIB) -o $@

# First-party file browser guest app (POSIX dirent/stat; ships on the ISO)
build/iso_files$(EXE): src/iso_files.c build/scene_fmt.o \
	build/scene_transport.o build/scene_client.o build/scene_app.o | build
	$(CC) $(CFLAGS) src/iso_files.c build/scene_fmt.o \
		build/scene_store.o build/scene_transport.o build/scene_client.o \
		build/scene_app.o $(SOCKLIB) -o $@

# First-party text editor guest app (open-with target for text files)
build/iso_edit$(EXE): src/iso_edit.c build/scene_fmt.o \
	build/scene_transport.o build/scene_client.o build/scene_app.o | build
	$(CC) $(CFLAGS) src/iso_edit.c build/scene_fmt.o \
		build/scene_store.o build/scene_transport.o build/scene_client.o \
		build/scene_app.o $(SOCKLIB) -o $@

# Editor proof: real iso_edit.exe child over real TCP
build/test_edit_app$(EXE): tests/test_edit_app.c build/scene_fmt.o \
	build/scene_store.o build/scene_transport.o build/scene_client.o \
	build/scene_server.o build/scene_fb.o build/scene_font.o \
	build/scene_font_data.o build/scene_compositor.o | build
	$(CC) $(CFLAGS) tests/test_edit_app.c build/scene_fmt.o \
		build/scene_store.o build/scene_transport.o build/scene_client.o \
		build/scene_server.o build/scene_fb.o build/scene_font.o \
		build/scene_font_data.o build/scene_compositor.o \
		$(SOCKLIB) -o $@

# File browser proof: real child over real TCP against a deterministic fixture
build/test_files_app$(EXE): tests/test_files_app.c build/scene_fmt.o \
	build/scene_store.o build/scene_transport.o build/scene_client.o \
	build/scene_server.o build/scene_fb.o build/scene_font.o \
	build/scene_font_data.o build/scene_compositor.o build/scene_image.o | build
	$(CC) $(CFLAGS) tests/test_files_app.c build/scene_fmt.o \
		build/scene_store.o build/scene_transport.o build/scene_client.o \
		build/scene_server.o build/scene_fb.o build/scene_font.o \
		build/scene_font_data.o build/scene_compositor.o build/scene_image.o \
		$(SOCKLIB) -o $@

# First-party photo viewer guest app (texture seam: refs on the wire only)
build/iso_photo$(EXE): src/iso_photo.c build/scene_fmt.o \
	build/scene_transport.o build/scene_client.o build/scene_app.o | build
	$(CC) $(CFLAGS) src/iso_photo.c build/scene_fmt.o \
		build/scene_store.o build/scene_transport.o build/scene_client.o \
		build/scene_app.o $(SOCKLIB) -o $@

# Photo proof: harness plays the OS importer (decodes fixture BMP, seeds ref 2)
build/test_photo_app$(EXE): tests/test_photo_app.c build/scene_fmt.o \
	build/scene_store.o build/scene_transport.o build/scene_client.o \
	build/scene_server.o build/scene_fb.o build/scene_font.o \
	build/scene_font_data.o build/scene_compositor.o build/scene_image.o | build
	$(CC) $(CFLAGS) tests/test_photo_app.c build/scene_fmt.o \
		build/scene_store.o build/scene_transport.o build/scene_client.o \
		build/scene_server.o build/scene_fb.o build/scene_font.o \
		build/scene_font_data.o build/scene_compositor.o build/scene_image.o \
		$(SOCKLIB) -o $@

# Cross-app search overlay (Super+S) suite — the OS-layer flagship consumer
build/test_searchoverlay$(EXE): tests/test_searchoverlay.c build/scene_fmt.o \
	build/scene_store.o build/scene_transport.o build/scene_client.o \
	build/scene_server.o build/scene_fb.o build/scene_font.o \
	build/scene_font_data.o build/scene_compositor.o build/scene_a11y.o \
	build/scene_shell.o build/scene_wallpaper.o build/scene_image.o | build
	$(CC) $(CFLAGS) tests/test_searchoverlay.c build/scene_fmt.o \
		build/scene_store.o build/scene_transport.o build/scene_client.o \
		build/scene_server.o build/scene_fb.o build/scene_font.o \
		build/scene_font_data.o build/scene_compositor.o build/scene_a11y.o \
		build/scene_shell.o build/scene_wallpaper.o build/scene_image.o \
		$(SOCKLIB) -o $@

build/test_settings$(EXE): tests/test_settings.c build/scene_fmt.o build/scene_store.o \
	build/scene_transport.o build/scene_client.o build/scene_server.o \
	build/scene_fb.o build/scene_font.o build/scene_font_data.o \
	build/scene_compositor.o build/scene_shell.o build/scene_settings.o \
	build/scene_wallpaper.o build/scene_image.o | build
	$(CC) $(CFLAGS) tests/test_settings.c build/scene_fmt.o build/scene_store.o \
		build/scene_transport.o build/scene_client.o build/scene_server.o \
		build/scene_fb.o build/scene_font.o build/scene_font_data.o \
		build/scene_compositor.o build/scene_shell.o build/scene_settings.o \
		build/scene_wallpaper.o build/scene_image.o \
		$(SOCKLIB) -o $@

build/test_theme$(EXE): tests/test_theme.c build/scene_theme.o build/scene_shell.o \
	build/scene_store.o build/scene_fmt.o build/scene_client.o build/scene_server.o \
	build/scene_transport.o build/scene_compositor.o build/scene_fb.o build/scene_font.o \
	build/scene_font_data.o build/scene_wallpaper.o build/scene_image.o | build
	$(CC) $(CFLAGS) tests/test_theme.c build/scene_theme.o build/scene_shell.o \
		build/scene_store.o build/scene_fmt.o build/scene_client.o build/scene_server.o \
		build/scene_transport.o build/scene_compositor.o build/scene_fb.o build/scene_font.o \
		build/scene_font_data.o build/scene_wallpaper.o build/scene_image.o \
		$(SOCKLIB) -o $@

build/test_image$(EXE): tests/test_image.c build/scene_image.o | build
	$(CC) $(CFLAGS) tests/test_image.c build/scene_image.o -o $@

build/test_wallpaper$(EXE): tests/test_wallpaper.c build/scene_wallpaper.o build/scene_image.o | build
	$(CC) $(CFLAGS) tests/test_wallpaper.c build/scene_wallpaper.o build/scene_image.o -o $@

# Windows GDI live preview (shell + wallpaper visible on desktop)
build/iso_preview$(EXE): src/iso_preview.c build/scene_fmt.o build/scene_store.o \
	build/scene_transport.o build/scene_client.o build/scene_server.o \
	build/scene_fb.o build/scene_font.o build/scene_font_data.o \
	build/scene_compositor.o build/scene_a11y.o build/scene_shell.o \
	build/scene_wallpaper.o build/scene_image.o | build
	$(CC) $(CFLAGS) src/iso_preview.c build/scene_fmt.o build/scene_store.o \
		build/scene_transport.o build/scene_client.o build/scene_server.o \
		build/scene_fb.o build/scene_font.o build/scene_font_data.o \
		build/scene_compositor.o build/scene_a11y.o build/scene_shell.o \
		build/scene_wallpaper.o build/scene_image.o \
		$(SOCKLIB) -lgdi32 -o $@

build/test_store_dbg$(EXE): tests/test_store.c src/scene_fmt.c src/scene_store.c | build
	$(CC) $(CFLAGS_DBG) tests/test_store.c src/scene_fmt.c src/scene_store.c -o $@

build/bench$(EXE): tools/bench.c build/scene_fmt.o build/scene_store.o | build
	$(CC) $(CFLAGS) tools/bench.c build/scene_fmt.o build/scene_store.o -o $@

build/preview_dump$(EXE): tools/preview_dump.c build/scene_fmt.o build/scene_store.o \
	build/scene_transport.o build/scene_client.o build/scene_server.o \
	build/scene_fb.o build/scene_font.o build/scene_font_data.o \
	build/scene_compositor.o build/scene_a11y.o build/scene_shell.o \
	build/scene_wallpaper.o build/scene_image.o | build
	$(CC) $(CFLAGS) tools/preview_dump.c build/scene_fmt.o build/scene_store.o \
		build/scene_transport.o build/scene_client.o build/scene_server.o \
		build/scene_fb.o build/scene_font.o build/scene_font_data.o \
		build/scene_compositor.o build/scene_a11y.o build/scene_shell.o \
		build/scene_wallpaper.o build/scene_image.o \
		$(SOCKLIB) -o $@

test: build/test_store$(EXE)
	./build/test_store$(EXE)

test-client: build/test_client$(EXE)
	./build/test_client$(EXE)

test-compositor: build/test_compositor$(EXE)
	./build/test_compositor$(EXE)

test-automation: build/test_automation$(EXE)
	./build/test_automation$(EXE)

test-a11y: build/test_a11y$(EXE)
	./build/test_a11y$(EXE)

test-rewind: build/test_rewind$(EXE)
	./build/test_rewind$(EXE)

test-shell: build/test_shell$(EXE)
	./build/test_shell$(EXE)

test-app: build/test_app$(EXE)
	./build/test_app$(EXE)

test-terminal: build/test_terminal$(EXE)
	./build/test_terminal$(EXE)

test-sessions: build/test_sessions$(EXE)
	./build/test_sessions$(EXE)

test-launcher: build/test_launcher$(EXE)
	./build/test_launcher$(EXE)

test-settings: build/test_settings$(EXE)
	./build/test_settings$(EXE)

test-theme: build/test_theme$(EXE)
	./build/test_theme$(EXE)

test-image: build/test_image$(EXE)
	./build/test_image$(EXE)

test-wallpaper: build/test_wallpaper$(EXE)
	./build/test_wallpaper$(EXE)

test-video: build/test_video_app$(EXE) build/iso_video$(EXE)
	./build/test_video_app$(EXE)

test-appwm: build/test_app_wm$(EXE)
	./build/test_app_wm$(EXE)

test-hostwm: build/test_hostwm$(EXE)
	./build/test_hostwm$(EXE)

test-shellwm: build/test_shellwm$(EXE)
	./build/test_shellwm$(EXE)

test-play: build/test_play_app$(EXE) build/iso_play$(EXE)
	./build/test_play_app$(EXE)

test-files: build/test_files_app$(EXE) build/iso_files$(EXE)
	./build/test_files_app$(EXE)

test-photo: build/test_photo_app$(EXE) build/iso_photo$(EXE)
	./build/test_photo_app$(EXE)

test-searchoverlay: build/test_searchoverlay$(EXE)
	./build/test_searchoverlay$(EXE)

test-codec: build/test_codec$(EXE)
	./build/test_codec$(EXE)

bench: build/bench$(EXE)
	./build/bench$(EXE)

test-dbg: build/test_store_dbg$(EXE)
	ASAN_OPTIONS=detect_leaks=1 ./build/test_store_dbg$(EXE)

# wlroots compositor (Linux only)
ifneq ($(UNAME_S),Windows)
build/iso_compositor.o: src/iso_compositor.c include/iso_compositor.h include/scene_compositor.h include/scene_fb.h include/scene_client.h include/scene_store.h include/scene_shell.h | build
	$(CC) $(CFLAGS) $(shell pkg-config --cflags wlroots wayland-server) -c src/iso_compositor.c -o $@

build/iso_compositor$(EXE): src/iso_compositor.c build/scene_fmt.o build/scene_store.o \
	build/scene_transport.o build/scene_client.o build/scene_server.o \
	build/scene_fb.o build/scene_font.o build/scene_font_data.o \
	build/scene_compositor.o build/scene_a11y.o build/scene_shell.o \
	build/scene_wallpaper.o build/scene_image.o \
	build/iso_compositor.o | build
	$(CC) $(CFLAGS) $(shell pkg-config --cflags --libs wlroots wayland-server) \
		src/iso_compositor.c build/scene_fmt.o build/scene_store.o \
		build/scene_transport.o build/scene_client.o build/scene_server.o \
		build/scene_fb.o build/scene_font.o build/scene_font_data.o \
		build/scene_compositor.o build/scene_a11y.o build/scene_shell.o \
		build/scene_wallpaper.o build/scene_image.o \
		-lwayland-server -lpthread -o $@

iso-compositor: build/iso_compositor$(EXE)

# DRM/KMS compositor (Linux only): pure kernel-UAPI, no external libs.
# ffmpeg static libs: the OS-side media importer (scene_codec) decodes
# the shared clip at this seam — the honest-boundary adoption, whose
# lock + justification live in third_party/ffmpeg/LICENSE.md.
build/iso_drm$(EXE): src/iso_drm.c build/scene_fmt.o build/scene_store.o \
	build/scene_transport.o build/scene_client.o build/scene_server.o \
	build/scene_fb.o build/scene_font.o build/scene_font_data.o \
	build/scene_compositor.o build/scene_a11y.o build/scene_shell.o \
	build/scene_wallpaper.o build/scene_image.o build/scene_launcher.o \
	build/scene_codec.o | build
	$(CC) $(CFLAGS) $(FFMPEG_INC) src/iso_drm.c build/scene_fmt.o \
		build/scene_store.o build/scene_transport.o build/scene_client.o \
		build/scene_server.o build/scene_fb.o build/scene_font.o \
		build/scene_font_data.o build/scene_compositor.o \
		build/scene_a11y.o build/scene_shell.o \
		build/scene_wallpaper.o build/scene_image.o \
		build/scene_launcher.o build/scene_codec.o \
		$(FFMPEG_LIBS) $(FFMPEG_EXTRA) -lm -lpthread -o $@

iso-drm: build/iso_drm$(EXE)

# Terminal app (Linux only): PTY-backed shell (fork/pty unavailable on Windows).
build/iso_terminal$(EXE): src/iso_terminal.c build/scene_fmt.o build/scene_store.o \
	build/scene_transport.o build/scene_client.o build/scene_server.o \
	build/scene_fb.o build/scene_font.o build/scene_font_data.o \
	build/scene_compositor.o build/scene_a11y.o build/scene_shell.o \
	build/scene_wallpaper.o build/scene_image.o build/scene_launcher.o \
	build/scene_terminal.o build/scene_app.o | build
	$(CC) $(CFLAGS) src/iso_terminal.c build/scene_fmt.o build/scene_store.o \
		build/scene_transport.o build/scene_client.o build/scene_server.o \
		build/scene_fb.o build/scene_font.o build/scene_font_data.o \
		build/scene_compositor.o build/scene_a11y.o build/scene_shell.o \
		build/scene_wallpaper.o build/scene_image.o build/scene_launcher.o \
		build/scene_terminal.o build/scene_app.o -lm -lpthread -o $@

iso-terminal: build/iso_terminal$(EXE)
endif

clean:
	rm -rf build

.PHONY: all test test-client test-compositor test-automation test-a11y test-rewind test-shell test-app test-terminal test-sessions test-launcher test-settings test-theme test-image test-wallpaper bench test-dbg iso-compositor iso-drm iso-terminal test-video test-appwm test-hostwm test-shellwm test-play clean test-codec
