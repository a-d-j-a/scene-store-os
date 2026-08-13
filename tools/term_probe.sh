#!/bin/sh
# term_probe.sh — run iso_terminal against the fake wire server, dump both logs.
cd /workspaces/scene-store-os/scene-store || exit 1
pkill -f fake_server.py 2>/dev/null
sleep 1
python3 /workspaces/scene-store-os/iso/tools/fake_server.py 19999 > /tmp/fake.log 2>&1 &
SRV=$!
sleep 1
SCENE_STORE_PORT=19999 timeout 20 ./build/iso_terminal > /tmp/app.log 2>&1
RC=$?
kill $SRV 2>/dev/null
sleep 1
echo "=== APP exit=$RC ==="
cat /tmp/app.log
echo "=== FAKE ==="
cat /tmp/fake.log