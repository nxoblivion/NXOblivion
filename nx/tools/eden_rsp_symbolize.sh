#!/bin/bash
# Names the offsets that tools/eden_rsp.py printed (pc, lr, stack words) with addr2line, in devkitPro's container.
# Usage: nx/tools/eden_rsp_symbolize.sh <eden_rsp output> [elf, default app/out/switch/oblivion] > named.txt
IN=${1:?eden_rsp output}; ELF=${2:-app/out/switch/oblivion}
NX="$(cd "$(dirname "$0")/.." && pwd)"
REL="$(realpath --relative-to="$NX" "$IN")"
exec sh "$NX/tools/switch_docker.sh" "python3 - '$REL' '$ELF' <<'PY'
import re, subprocess, sys
src, elf = sys.argv[1], sys.argv[2]
lines = open(src).read().splitlines()
offs = sorted({int(x, 16) for l in lines if l.startswith(('thread', '  stack')) for x in re.findall(r'(?<![a-z] )0x[0-9a-f]+', l.split(' sp ')[0] if l.startswith('thread') else l)})
out = subprocess.run(['aarch64-none-elf-addr2line', '-f', '-C', '-e', elf] + [hex(o) for o in offs], capture_output=True, text=True).stdout.splitlines()
names = {o: (out[2*i][:110] + '  ' + out[2*i+1].split('/')[-1]) for i, o in enumerate(offs)}
for l in lines:
    print(l)
    if l.startswith(('thread', '  stack')):
        for x in re.findall(r'0x[0-9a-f]+', l.split(' sp ')[0] if l.startswith('thread') else l):
            o = int(x, 16)
            if o in names: print(f'      {x:>10} {names[o]}')
PY"
