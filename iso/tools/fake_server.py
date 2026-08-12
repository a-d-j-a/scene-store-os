#!/usr/bin/env python3
"""fake_server.py PORT — debug tool: minimal scene-wire server.

Accepts one connection, sends a fresh-session WELCOME (30-byte payload,
FNV1a checksum over [0,16+plen) with the checksum bytes zeroed), then
reads and decodes every inbound frame for 10 seconds. Prints one line
per frame: opcode, seq, payload length, checksum pass/fail. Proves what
a guest app actually sends over the wire, independent of the launcher.
"""
import socket
import struct
import sys
import time

MAGIC = 0x5343454E  # "SCEN" LE


def fnv1a32(b):
    h = 0x811C9DC5
    for x in b:
        h ^= x
        h = (h * 0x01000193) & 0xFFFFFFFF
    return h


def welcome_frame():
    payload = struct.pack('<IHIIIQ', 0x1337, 0, 4096, 1024, 32, 8192, 0)
    hdr = struct.pack('<IHHI', MAGIC, 0, 0x8001, len(payload))
    zeroed = hdr + b'\x00\x00\x00\x00' + payload
    return hdr + struct.pack('<I', fnv1a32(zeroed)) + payload


def main():
    port = int(sys.argv[1])
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(('127.0.0.1', port))
    s.listen(1)
    print('fake: listening %d' % port, flush=True)
    c, _ = s.accept()
    print('fake: accepted', flush=True)
    c.sendall(welcome_frame())
    c.settimeout(2)
    buf = b''
    t0 = time.time()
    nframes = 0
    total = 0
    while time.time() - t0 < 10:
        try:
            b = c.recv(65536)
        except socket.timeout:
            continue
        if not b:
            print('fake: closed by peer', flush=True)
            break
        buf += b
        total += len(b)
        while len(buf) >= 16:
            magic, ver, op, plen = struct.unpack('<IHHI', buf[:16])
            if magic != MAGIC or len(buf) < 16 + plen:
                break
            full = buf[:16 + plen]
            ck = struct.unpack('<I', full[12:16])[0]
            zeroed = full[:12] + b'\x00\x00\x00\x00' + full[16:]
            good = fnv1a32(zeroed) == ck
            seq = 0
            if plen >= 8:
                seq = struct.unpack('<Q', full[16:24])[0]
            print('fake: frame op=0x%04X seq=%u plen=%u ck=%s'
                  % (op, seq, plen, 'OK' if good else 'BAD'), flush=True)
            nframes += 1
            buf = buf[16 + plen:]
        if nframes >= 5:
            print('fake: pausing decode loop (frames=%d, bytes=%d)'
                  % (nframes, total), flush=True)
            break
    c.close()
    print('fake: END frames=%d bytes=%d' % (nframes, total), flush=True)


if __name__ == '__main__':
    main()