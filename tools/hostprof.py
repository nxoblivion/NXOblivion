#!/usr/bin/env python3
"""Symbolise sdmc:/oblivion/hostprof.txt (Switch host sampling profiler, switch_hostprof_hz).

Usage: tools/hostprof.py hostprof.txt [--elf out/switch-gen/oblivion_gl.elf] [--windows a:b] [--top 25]
                         [--threads substr,...] [--lr] [--incl] [--stacks N] [--match substr]

Needs a symbol list: `aarch64-none-elf-nm -n -C --defined-only oblivion_gl.elf > oblivion_gl.syms` (run in the
devkitA64 container); it is created next to the ELF on first use if missing and docker is available.
Per thread it prints the busiest functions (self samples). Samples whose PC is in an svc stub (svcWaitSynchronization,
svcSleepThread, svcArbitrateLock, svcWaitProcessWideKeyAtomic, ...) are blocked time; their LR names the caller.
Profiles written with switch_hostprof_stack_depth > 0 also hold caller chains ('S' lines): --incl prints inclusive
time per function (sample has the function anywhere in its chain), --stacks N the N most common chains (leaf
function first, then its callers), --match keeps only chains containing a function name substring.
"""
import argparse
import bisect
import collections
import os
import re
import sys


def load_syms(elf, syms_path=None):
    explicit = syms_path is not None
    if not explicit:
        syms_path = os.path.splitext(elf)[0] + ".syms"
    if not os.path.exists(syms_path) or (not explicit and os.path.getmtime(syms_path) < os.path.getmtime(elf)):
        root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
        rel = os.path.relpath(os.path.abspath(elf), root).replace("\\", "/")
        rel_out = os.path.relpath(os.path.abspath(syms_path), root).replace("\\", "/")
        # (Python's "bash" on Windows may be WSL's, so the git-bash command is printed instead of run.)
        sys.exit(f"{syms_path} is missing or older than the ELF; create it with:\n"
                 f"  ports/switch/oblivion_gl/docker.sh nm 'aarch64-none-elf-nm -n -C --defined-only "
                 f"/work/{rel} > /work/{rel_out}'")
    addrs, names = [], []
    with open(syms_path, encoding="utf-8", errors="replace") as f:
        for line in f:
            parts = line.rstrip("\n").split(" ", 2)
            if len(parts) < 3 or parts[1] not in "tTwW":
                continue
            try:
                a = int(parts[0], 16)
            except ValueError:
                continue
            if a == 0:
                continue
            addrs.append(a)
            names.append(parts[2])
    return addrs, names


NAMES = {}  # 360 address -> readable name (tools/pcmatch, out/names/oblivion360_names.txt)


def load_names(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            parts = line.split(None, 2)
            if len(parts) >= 2 and not line.startswith("#"):
                try:
                    NAMES[int(parts[0], 16)] = parts[1]
                except ValueError:
                    pass


def sym(addrs, names, a):
    i = bisect.bisect_right(addrs, a) - 1
    if i < 0:
        return f"?{a:x}"
    n = names[i]
    if NAMES and "sub_" in n:
        n = re.sub(r"\bsub_([0-9A-Fa-f]{8})\b",
                   lambda m: f"{NAMES[int(m.group(1), 16)]}[{m.group(1).upper()}]" if int(m.group(1), 16) in NAMES else m.group(0), n)
    return n


BLOCKING = ("svcWaitSynchronization", "svcSleepThread", "svcArbitrateLock", "svcWaitProcessWideKeyAtomic",
            "svcWaitForAddress", "svcReplyAndReceive", "svcSendSyncRequest", "svcWaitSynchronizationSingle")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("prof")
    ap.add_argument("--elf", default=os.path.join(os.path.dirname(__file__), "..", "out", "switch-gen", "oblivion_gl.elf"))
    ap.add_argument("--windows", default=None, help="a:b window index range (python slice), default all")
    ap.add_argument("--top", type=int, default=25)
    ap.add_argument("--threads", default=None)
    ap.add_argument("--lr", action="store_true", help="also show the callers (LR) of blocked samples")
    ap.add_argument("--syms", default=None, help="symbol list of the build that wrote the profile (default: <elf>.syms)")
    ap.add_argument("--names", default=None, help="360 function names (addr name ...) to show instead of sub_XXXXXXXX "
                    "(default: out/names/oblivion360_names.txt if present; 'none' to disable)")
    ap.add_argument("--base", default=None, help="image base to subtract (hex), when the file's header says base=0")
    ap.add_argument("--incl", action="store_true", help="inclusive time per function (needs caller chains)")
    ap.add_argument("--stacks", type=int, default=0, help="print the N most common caller chains per thread")
    ap.add_argument("--match", default=None, help="only chains that contain a function with this substring")
    ap.add_argument("--depth", type=int, default=10, help="frames printed per chain")
    args = ap.parse_args()
    addrs, names = load_syms(args.elf, args.syms)
    names_path = args.names or os.path.join(os.path.dirname(__file__), "..", "out", "names", "oblivion360_names.txt")
    if args.names != "none" and os.path.exists(names_path):
        load_names(names_path)

    windows = []
    cur = None
    base = int(args.base, 16) if args.base else 0
    with open(args.prof, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line or line[0] == "#":
                continue
            tag = line[0]
            if tag == "W":
                _, t, ws = line.split()
                cur = {"t": float(t), "len": float(ws), "threads": {}}
                windows.append(cur)
            elif tag == "T":
                try:
                    _, idx, rest = line.split(" ", 2)
                    name, samples, fails = rest.rsplit("|", 2)
                except ValueError:
                    break  # truncated last window (the app died while writing)
                th = {"name": name, "samples": int(samples), "fails": int(fails), "pc": collections.Counter(),
                      "lr": collections.Counter(), "stacks": collections.Counter()}
                cur["threads"][int(idx)] = th
            elif tag in "PL":
                parts = line.split()
                if len(parts) != 3:
                    continue
                th["pc" if tag == "P" else "lr"][int(parts[1], 16) - base] += int(parts[2])
            elif tag == "S":
                parts = line.split()
                if len(parts) != 3:
                    continue
                th["stacks"][tuple(int(x, 16) - base for x in parts[2].split(","))] += int(parts[1])
    if args.windows:
        a, b = (args.windows.split(":") + [""])[:2]
        windows = windows[int(a) if a else None:int(b) if b else None]
    if not windows:
        print("no windows")
        return
    print(f"windows {len(windows)}: t={windows[0]['t'] - windows[0]['len']:.0f}..{windows[-1]['t']:.0f} s")
    agg = {}
    for w in windows:
        for idx, th in w["threads"].items():
            key = (idx, th["name"])
            a = agg.setdefault(key, {"samples": 0, "fails": 0, "func": collections.Counter(),
                                     "blocked_lr": collections.Counter(), "pc_total": 0,
                                     "chains": collections.Counter()})
            a["samples"] += th["samples"]
            a["fails"] += th["fails"]
            for pc, c in th["pc"].items():
                a["func"][sym(addrs, names, pc)] += c
                a["pc_total"] += c
            for lr, c in th["lr"].items():
                a["blocked_lr"][sym(addrs, names, lr)] += c
            for st, c in th["stacks"].items():
                # Chain of function names, leaf first; recursion / LR == first frame collapsed.
                chain = []
                for addr in st:
                    fn = sym(addrs, names, addr - 4)  # return address -> the call instruction's function
                    if not chain or chain[-1] != fn:
                        chain.append(fn)
                a["chains"][tuple(chain)] += c
    filt = args.threads.split(",") if args.threads else None
    for (idx, name), a in sorted(agg.items(), key=lambda kv: -kv[1]["samples"]):
        if filt and not any(s in name for s in filt):
            continue
        if not a["samples"]:
            continue
        blocked = sum(c for fn, c in a["func"].items() if fn.startswith(BLOCKING))
        print(f"\n== thread {idx} '{name}': {a['samples']} samples (fails {a['fails']}), blocked in svc "
              f"{100.0 * blocked / max(1, a['samples']):.0f}%  (histogram keeps top PCs: {a['pc_total']} listed)")
        for fn, c in a["func"].most_common(args.top):
            print(f"  {100.0 * c / a['samples']:5.1f}%  {fn[:150]}")
        if args.lr:
            print("  -- LR (callers):")
            for fn, c in a["blocked_lr"].most_common(12):
                print(f"  {100.0 * c / a['samples']:5.1f}%  {fn[:150]}")
        chains = a["chains"]
        if args.match:
            chains = collections.Counter({k: v for k, v in chains.items() if any(args.match in fn for fn in k)})
        if args.incl and chains:
            incl = collections.Counter()
            for chain, c in chains.items():
                for fn in set(chain):
                    incl[fn] += c
            print(f"  -- inclusive (chains cover {sum(chains.values())} samples):")
            for fn, c in incl.most_common(args.top):
                print(f"  {100.0 * c / a['samples']:5.1f}%  {fn[:150]}")
        if args.stacks and chains:
            print(f"  -- top chains (callers of the PC's function, innermost first):")
            for chain, c in chains.most_common(args.stacks):
                short = [fn.split("(")[0][-60:] for fn in chain[:args.depth]]
                print(f"  {100.0 * c / a['samples']:5.1f}%  " + " <- ".join(short))


if __name__ == "__main__":
    main()
