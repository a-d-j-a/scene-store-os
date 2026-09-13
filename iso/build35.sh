#!/bin/sh
# build35.sh - build the full ISO with pass 35 additions
# Run on the codespace: nohup sh /tmp/build35.sh &
set -e
cd /workspaces/scene-store-os/iso
sh build.sh all
