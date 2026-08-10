cd /workspaces/scene-store-os
echo "screendump /tmp/scrA.png" | socat - unix-connect:/tmp/qmon.sock
sleep 10
echo "screendump /tmp/scrB.png" | socat - unix-connect:/tmp/qmon.sock
echo "sendkey ret" | socat - unix-connect:/tmp/qmon.sock
sleep 3
echo "screendump /tmp/scrC.png" | socat - unix-connect:/tmp/qmon.sock
cp /tmp/scrA.png /tmp/scrB.png /tmp/scrC.png /workspaces/scene-store-os/
echo CMD_CYCLE_4_DONE
