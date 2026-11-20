#!/usr/bin/env python3
"""Minimal native nxlink client (hbmenu netloader protocol), for Windows hosts without devkitPro.

Usage: tools/nxlink.py <switch-ip> <file.nro> [--path oblivion/oblivion.nro] [--port 28280]

--path is relative to sdmc:/switch/ (an absolute "/switch/..." path makes hbmenu answer "Failed to create file").

nxlink inside the Docker Desktop container does not work: its NAT accepts the TCP connection before the data is
forwarded, and hbmenu gives up with "Error getting name length err=11".
"""
import argparse
import os
import socket
import struct
import sys
import zlib

CHUNK = 16 * 1024  # hbmenu rejects compressed chunks larger than its zlib buffer


def recv_int(s):
    buf = b""
    while len(buf) < 4:
        part = s.recv(4 - len(buf))
        if not part:
            sys.exit("connection closed by the Switch")
        buf += part
    return struct.unpack("<i", buf)[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("address")
    ap.add_argument("nro")
    ap.add_argument("--path", default=None, help="upload path on the SD card (default /switch/<basename>)")
    ap.add_argument("--port", type=int, default=28280)
    a = ap.parse_args()
    data = open(a.nro, "rb").read()
    name = (a.path or os.path.basename(a.nro)).encode()
    s = socket.create_connection((a.address, a.port), timeout=30)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    # Name length, name and file size in one write so hbmenu finds them at once.
    s.sendall(struct.pack("<i", len(name)) + name + struct.pack("<i", len(data)))
    r = recv_int(s)
    if r != 0:
        sys.exit({-1: "Failed to create file", -2: "Insufficient space", -3: "Insufficient memory"}.get(r, f"response {r}"))
    print(f"Sending {name.decode()}, {len(data)} bytes")
    z = zlib.compressobj(zlib.Z_DEFAULT_COMPRESSION)
    sent = blocks = 0
    pos = 0

    def send_out(out):
        nonlocal blocks
        for i in range(0, len(out), CHUNK):
            piece = out[i:i + CHUNK]
            s.sendall(struct.pack("<i", len(piece)) + piece)
            blocks += 1

    while pos < len(data):
        send_out(z.compress(data[pos:pos + CHUNK]))
        pos += CHUNK
        if pos // CHUNK % 512 == 0:
            print(f"\r{min(pos, len(data)) * 100 // len(data)}%", end="", flush=True)
    send_out(z.flush())
    print(f"\r{len(data)} sent (100%), {blocks} blocks")
    r = recv_int(s)
    if r != 0:
        sys.exit(f"Failed sending {name.decode()} (response {r})")
    cmd = name + b"\0"
    s.sendall(struct.pack("<i", len(cmd)) + cmd)
    s.close()
    print("done; the Switch starts the NRO")


if __name__ == "__main__":
    main()
