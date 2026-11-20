"""Where every guest thread of an Eden run is, through Eden's GDB stub (use_gdbstub in the Eden copy's config).

Eden waits at the program's first instruction for a debugger when the stub is on. This attaches, notes that pc (the
NRO's load address: _start is at offset 0), lets the guest run for --secs, stops it (Ctrl-C), and prints for each
thread its pc, lr and the words on its stack that point into the NRO's code, as offsets from the load address, which
aarch64-none-elf-addr2line names (tools/eden_rsp_symbolize.sh). Detaching is not supported by Eden: the run ends
with the Eden that tools/run_eden.sh started.

Usage: python nx/tools/eden_rsp.py [--port 6543] [--secs 60] [--stack-words 512] [--text-size 0x2f7d2a8] > out.txt
"""
import argparse
import socket
import struct
import sys
import time


class Rsp:
    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=30)
        self.buf = b""

    def _read_packet(self, timeout=30):
        self.s.settimeout(timeout)
        while True:
            start = self.buf.find(b"$")
            if start >= 0:
                end = self.buf.find(b"#", start)
                if end >= 0 and len(self.buf) >= end + 3:
                    data = self.buf[start + 1:end]
                    self.buf = self.buf[end + 3:]
                    self.s.sendall(b"+")
                    return data.decode("latin1")
            chunk = self.s.recv(65536)
            if not chunk:
                raise EOFError("stub closed the connection")
            self.buf += chunk

    def send(self, data, wait=True, timeout=30):
        payload = data.encode("latin1")
        self.s.sendall(b"$" + payload + b"#%02x" % (sum(payload) & 0xFF))
        return self._read_packet(timeout) if wait else None

    def interrupt(self, timeout=60):
        self.s.sendall(b"\x03")
        return self._read_packet(timeout)


def regs_of(r, tid):
    r.send(f"Hg{tid}")
    g = r.send("g")
    words = [struct.unpack("<Q", bytes.fromhex(g[i:i + 16]))[0] for i in range(0, 34 * 16, 16)]
    return {"x": words[:31], "sp": words[31], "pc": words[32]}


def read_mem(r, addr, size):
    out = b""
    while size > 0:
        n = min(size, 0x800)
        rep = r.send(f"m{addr:x},{n:x}")
        if not rep or rep.startswith("E"):
            break
        out += bytes.fromhex(rep)
        addr += n
        size -= n
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=6543)
    ap.add_argument("--secs", type=float, default=60)
    ap.add_argument("--stack-words", type=int, default=512)
    ap.add_argument("--text-size", type=lambda s: int(s, 0), default=0x2F7D2A8)
    ap.add_argument("--samples", type=int, default=0, help="then this many more stops, --interval apart: pc and lr of every thread")
    ap.add_argument("--interval", type=float, default=0.5)
    ap.add_argument("--break", dest="breaks", action="append", default=[],
                    help="NAME=OFFSET: a breakpoint there during --secs; each hit prints the thread, x0, x1 and lr")
    ap.add_argument("--once", action="store_true", help="each breakpoint removed after its first hit")
    a = ap.parse_args()

    r = Rsp(a.port)
    r.s.sendall(b"+")
    print("stop:", r.send("?"))
    first = r.send("qfThreadInfo")[1:].split(",")[0]
    base = regs_of(r, first)["pc"]
    print(f"base {base:#x}")
    breaks = {}
    for spec in a.breaks:
        name, off = spec.split("=")
        addr = base + int(off, 0)
        breaks[addr] = name
        print(f"break {name} at {addr:#x}:", r.send(f"Z0,{addr:x},4"))
    end = time.time() + a.secs
    if not breaks:
        r.send("c", wait=False)
        time.sleep(a.secs)
        print("interrupt:", r.interrupt())
    else:
        # Eden stops again at a breakpoint it is continued from, and its single step does not resume, so a hit at A
        # is stepped over with breakpoints: A out and A+4 in, continue, then at A+4 the other way round. (A function's
        # first instruction is no branch; another thread passing A meanwhile is not seen.)
        rearm = {}
        stop = None
        while time.time() < end:
            r.send("vCont;c", wait=False)
            try:
                stop = r._read_packet(timeout=max(0.1, end - time.time()))
            except socket.timeout:
                stop = None
                break
            tid = (stop.split("thread:")[1].split(";")[0]) if "thread:" in stop else "?"
            regs = regs_of(r, tid)
            pc = regs["pc"]
            if pc in rearm:
                r.send(f"z0,{pc:x},4")
                r.send(f"Z0,{rearm.pop(pc):x},4")
                continue
            name = breaks.get(pc, f"pc {pc - base:#x}")
            print(f"{time.time() - end + a.secs:7.2f}s {name} thread {tid} x0 {regs['x'][0]:#x} x1 {regs['x'][1]:#x} "
                  f"lr {regs['x'][30] - base:#x}")
            if pc in breaks:
                r.send(f"z0,{pc:x},4")
                if not a.once:
                    r.send(f"Z0,{pc + 4:x},4")
                    rearm[pc + 4] = pc
        if stop is None:
            print("interrupt:", r.interrupt())
        for addr in list(breaks) + list(rearm):
            r.send(f"z0,{addr:x},4")

    tids = []
    rep = r.send("qfThreadInfo")
    while rep.startswith("m"):
        tids += rep[1:].split(",")
        rep = r.send("qsThreadInfo")
    print(f"{len(tids)} threads")
    xml, off = "", 0
    while True:
        rep = r.send(f"qXfer:threads:read::{off:x},fff")
        if not rep or rep[0] not in "lm":
            break
        xml += rep[1:]
        off += len(rep) - 1
        if rep[0] == "l":
            break
    print("threads xml:", xml.replace(chr(10), " "))
    lo, hi = base, base + a.text_size
    for tid in tids:
        name = r.send(f"qThreadExtraInfo,{tid}")
        try:
            name = bytes.fromhex(name).decode("latin1")
        except ValueError:
            pass
        regs = regs_of(r, tid)
        pc, lr, sp = regs["pc"], regs["x"][30], regs["sp"]
        stack = read_mem(r, sp, a.stack_words * 8)
        rets = []
        for i in range(0, len(stack) - 7, 8):
            w = struct.unpack_from("<Q", stack, i)[0]
            if lo <= w < hi:
                rets.append(w - base)
        def off(v):
            return f"{v - base:#x}" if lo <= v < hi else f"abs {v:#x}"
        print(f"thread {tid} [{name}] pc {off(pc)} lr {off(lr)} sp {sp:#x}")
        print("  stack:", " ".join(f"{x:#x}" for x in rets[:40]))
    for n in range(a.samples):
        r.send("c", wait=False)
        time.sleep(a.interval)
        r.interrupt()
        line = []
        for tid in tids:
            regs = regs_of(r, tid)
            pc, lr = regs["pc"], regs["x"][30]
            line.append(f"{tid}:{pc - base:#x}/{lr - base:#x}" if lo <= pc < hi else f"{tid}:abs")
        print(f"sample {n} " + " ".join(line))
    sys.stdout.flush()


if __name__ == "__main__":
    main()
