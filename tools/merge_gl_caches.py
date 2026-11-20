#!/usr/bin/env python3
"""Merge Switch GL shader cache files (sdmc:/oblivion/cache/) from several machines (Eden runs, consoles).

Usage: tools/merge_gl_caches.py OUT_DIR BASE_DIR OTHER_DIR [OTHER_DIR ...] [--suffix-in _ab] [--suffix-out ""]

Each directory holds gl_programs<sfx>.bin, gl_binaries<sfx>.bin, gl_nvc0<sfx>.bin. The BASE files decide the header
(program cache format/tag, Mesa fingerprint); records of the others are appended only when their header matches, so
the result is never rejected (a rejected file is recreated empty by the game). The loaders tolerate duplicates
(shaders/programs already known are skipped, binary and nvc0 records of the same key replace earlier ones).
"""
import argparse
import os
import struct
import sys

PROGRAMS = (b"RXGLPC01", 16)  # magic, u32 format, u32 tag
BINARIES = (b"RXGLPB01", 16)  # magic, u64 Mesa fingerprint
NVC0 = (b"RXNVC001", 16)      # magic, u64 fingerprint


def records_programs(data, pos):
    out = []
    while pos + 16 <= len(data):
        tag = data[pos:pos + 1]
        if tag == b"S":
            count = struct.unpack_from("<I", data, pos + 4)[0]
            end = pos + 16 + count * 4
            if count == 0 or count > 0x10000 or end > len(data):
                break
        elif tag == b"P":
            end = pos + 40
            if end > len(data):
                break
        else:
            break
        out.append(data[pos:end])
        pos = end
    return out


def records_keyed(data, pos, head):
    # binaries: u64 key, u32 format, u32 size, data; nvc0: u64 key, u32 size, data
    out = []
    while pos + head <= len(data):
        size = struct.unpack_from("<I", data, pos + head - 4)[0]
        end = pos + head + size
        if size == 0 or end > len(data):
            break
        out.append(data[pos:end])
        pos = end
    return out


def merge(kind, paths, out_path):
    magic, hlen = {"programs": PROGRAMS, "binaries": BINARIES, "nvc0": NVC0}[kind]
    base = None
    recs = []
    seen = set()
    for p in paths:
        if not os.path.exists(p):
            print(f"  {kind}: {p} missing, skipped")
            continue
        data = open(p, "rb").read()
        if len(data) < hlen or data[:8] != magic:
            print(f"  {kind}: {p} has no valid header, skipped")
            continue
        header = data[:hlen]
        if base is None:
            base = header
        elif header != base:
            print(f"  {kind}: {p} header {header[8:].hex()} != base {base[8:].hex()} (other Mesa/translator), skipped")
            continue
        if kind == "programs":
            rs = records_programs(data, hlen)
        else:
            rs = records_keyed(data, hlen, 16 if kind == "binaries" else 12)
        new = 0
        for r in rs:
            if r in seen:
                continue
            seen.add(r)
            recs.append(r)
            new += 1
        print(f"  {kind}: {p}: {len(rs)} records, {new} new")
    if base is None:
        print(f"  {kind}: nothing to write")
        return
    with open(out_path, "wb") as f:
        f.write(base)
        for r in recs:
            f.write(r)
    print(f"  {kind}: wrote {out_path} ({len(recs)} records, {os.path.getsize(out_path)} bytes)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out_dir")
    ap.add_argument("dirs", nargs="+", help="base dir first, then the dirs to merge in")
    ap.add_argument("--suffix-in", default="", help="comma list, one per dir (e.g. ',_ab' = base plain, 2nd _ab)")
    ap.add_argument("--suffix-out", default="")
    a = ap.parse_args()
    sfx = a.suffix_in.split(",") if a.suffix_in else [""] * len(a.dirs)
    if len(sfx) == 1:
        sfx = sfx * len(a.dirs)
    if len(sfx) != len(a.dirs):
        sys.exit("--suffix-in needs one entry per dir")
    os.makedirs(a.out_dir, exist_ok=True)
    for kind, name in (("programs", "gl_programs"), ("binaries", "gl_binaries"), ("nvc0", "gl_nvc0")):
        merge(kind, [os.path.join(d, f"{name}{s}.bin") for d, s in zip(a.dirs, sfx)],
              os.path.join(a.out_dir, f"{name}{a.suffix_out}.bin"))


if __name__ == "__main__":
    main()
