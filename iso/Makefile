# ISO build targets
# Run: make        (full build)
#      make clean  (nuke everything)
#      make scene  (build scene-store only)
#      make iso    (just create the ISO from existing build)

SHELL  = /bin/sh
BUILD  = build.sh

.PHONY: all clean fetch kernel musl busybox deps wlroots scene iso

all:
	./$(BUILD) all

clean:
	./$(BUILD) clean

fetch:
	./$(BUILD) fetch

kernel:
	./$(BUILD) kernel

musl:
	./$(BUILD) musl

busybox:
	./$(BUILD) busybox

deps:
	./$(BUILD) deps

wlroots:
	./$(BUILD) wlroots

scene:
	./$(BUILD) scene

iso:
	./$(BUILD) iso
