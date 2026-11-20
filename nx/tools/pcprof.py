"""Samples a running oblivion.exe on Windows (the PC build's answer to tools/hostprof.py on the Switch).

Usage: python nx/tools/pcprof.py [--after 70] [--seconds 10] [--hz 1000] [--top 30] [--exe-name oblivion.exe]
                                 [--pdb-exe nx/app/out/build/win-amd64-release/oblivion.exe] [--threads N]

Waits for the process to start, then --after seconds, then for --seconds suspends each thread --hz times a second
and reads its instruction pointer (GetThreadContext). Per thread (the busiest --threads), prints the functions most
often at the top (self samples) and, from the return addresses found on the stack near rsp (a scan, so callers are
approximate), the functions most often anywhere in the sample (inclusive). Symbols: llvm-symbolizer against
the executable's PDB. A thread blocked in the kernel shows as ntdll / KERNELBASE wait functions.
"""
import argparse
import collections
import ctypes
import ctypes.wintypes as wt
import os
import shutil
import subprocess
import sys
import time

k32 = ctypes.WinDLL('kernel32', use_last_error=True)
psapi = ctypes.WinDLL('psapi', use_last_error=True)

TH32CS_SNAPPROCESS, TH32CS_SNAPTHREAD = 0x2, 0x4
THREAD_ALL = 0x0002 | 0x0008 | 0x0010 | 0x0040  # suspend/resume, get context, query information
PROCESS_QUERY_READ = 0x0400 | 0x0010


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [('dwSize', wt.DWORD), ('cntUsage', wt.DWORD), ('th32ProcessID', wt.DWORD),
                ('th32DefaultHeapID', ctypes.c_size_t), ('th32ModuleID', wt.DWORD), ('cntThreads', wt.DWORD),
                ('th32ParentProcessID', wt.DWORD), ('pcPriClassBase', ctypes.c_long), ('dwFlags', wt.DWORD),
                ('szExeFile', ctypes.c_wchar * 260)]


class THREADENTRY32(ctypes.Structure):
    _fields_ = [('dwSize', wt.DWORD), ('cntUsage', wt.DWORD), ('th32ThreadID', wt.DWORD),
                ('th32OwnerProcessID', wt.DWORD), ('tpBasePri', ctypes.c_long), ('tpDeltaPri', ctypes.c_long),
                ('dwFlags', wt.DWORD)]


k32.CreateToolhelp32Snapshot.restype = wt.HANDLE
k32.OpenThread.restype = wt.HANDLE
k32.OpenProcess.restype = wt.HANDLE
CONTEXT_SIZE = 1232
CONTEXT_CONTROL = 0x100001
OFF_FLAGS, OFF_RSP, OFF_RIP = 0x30, 0x98, 0xF8


def find_process(name):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    e = PROCESSENTRY32W()
    e.dwSize = ctypes.sizeof(e)
    pid = None
    ok = k32.Process32FirstW(snap, ctypes.byref(e))
    while ok:
        if e.szExeFile.lower() == name.lower():
            pid = e.th32ProcessID
        ok = k32.Process32NextW(snap, ctypes.byref(e))
    k32.CloseHandle(snap)
    return pid


def threads_of(pid):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    e = THREADENTRY32()
    e.dwSize = ctypes.sizeof(e)
    out = []
    ok = k32.Thread32First(snap, ctypes.byref(e))
    while ok:
        if e.th32OwnerProcessID == pid:
            out.append(e.th32ThreadID)
        ok = k32.Thread32Next(snap, ctypes.byref(e))
    k32.CloseHandle(snap)
    return out


def modules_of(process):
    """[(base, size, path)] of the process's modules."""
    mods = (wt.HMODULE * 1024)()
    needed = wt.DWORD()
    psapi.EnumProcessModulesEx(process, mods, ctypes.sizeof(mods), ctypes.byref(needed), 3)
    out = []

    class MODULEINFO(ctypes.Structure):
        _fields_ = [('base', ctypes.c_void_p), ('size', wt.DWORD), ('entry', ctypes.c_void_p)]

    for i in range(needed.value // ctypes.sizeof(wt.HMODULE)):
        info = MODULEINFO()
        psapi.GetModuleInformation(process, ctypes.c_void_p(mods[i]), ctypes.byref(info), ctypes.sizeof(info))
        name = ctypes.create_unicode_buffer(520)
        psapi.GetModuleFileNameExW(process, ctypes.c_void_p(mods[i]), name, 520)
        out.append((info.base or 0, info.size, name.value))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--after', type=float, default=70)
    ap.add_argument('--seconds', type=float, default=10)
    ap.add_argument('--hz', type=float, default=1000)
    ap.add_argument('--top', type=int, default=30)
    ap.add_argument('--threads', type=int, default=3)
    ap.add_argument('--exe-name', default='oblivion.exe')
    ap.add_argument('--pdb-exe', default=os.path.join(os.path.dirname(__file__), '..', 'app', 'out', 'build',
                                                      'win-amd64-release', 'oblivion.exe'))
    ap.add_argument('--stack-words', type=int, default=256, help='stack qwords scanned for return addresses')
    ap.add_argument('--depth', type=int, default=6, help='frames per printed call chain')
    ap.add_argument('--wait', type=float, default=120, help='seconds to wait for the process to appear')
    args = ap.parse_args()

    deadline = time.time() + args.wait
    pid = None
    while time.time() < deadline and not (pid := find_process(args.exe_name)):
        time.sleep(0.2)
    if not pid:
        sys.exit(f'{args.exe_name} not found')
    print(f'pid {pid}; sampling in {args.after:.0f} s', flush=True)
    time.sleep(args.after)
    process = k32.OpenProcess(PROCESS_QUERY_READ, False, pid)
    mods = modules_of(process)
    exe = next(((b, s, p) for b, s, p in mods if p.lower().endswith(args.exe_name.lower())), None)
    if not exe:
        sys.exit('module not found')
    exe_base, exe_size = exe[0], exe[1]

    def module_of(addr):
        for b, s, p in mods:
            if b <= addr < b + s:
                return os.path.basename(p), addr - b
        return '?', addr

    raw = ctypes.create_string_buffer(CONTEXT_SIZE + 16)
    ctx_addr = (ctypes.addressof(raw) + 15) & ~15
    ctx = (ctypes.c_char * CONTEXT_SIZE).from_address(ctx_addr)
    stack = (ctypes.c_uint64 * args.stack_words)()
    read = ctypes.c_size_t()
    handles = {}
    self_samples = collections.defaultdict(collections.Counter)  # tid -> Counter(rip)
    incl_samples = collections.defaultdict(collections.Counter)  # tid -> Counter(exe return address)
    chains = collections.defaultdict(collections.Counter)  # tid -> Counter((top, return addresses nearest first))
    totals = collections.Counter()
    period = 1.0 / args.hz
    end = time.time() + args.seconds
    next_refresh = 0
    while time.time() < end:
        now = time.time()
        if now >= next_refresh:
            for tid in threads_of(pid):
                if tid not in handles:
                    h = k32.OpenThread(THREAD_ALL, False, tid)
                    if h:
                        handles[tid] = h
            next_refresh = now + 1.0
        for tid, h in list(handles.items()):
            if k32.SuspendThread(h) == 0xFFFFFFFF:
                k32.CloseHandle(h)
                del handles[tid]
                continue
            ctypes.memset(ctx_addr, 0, CONTEXT_SIZE)
            ctypes.c_uint32.from_address(ctx_addr + OFF_FLAGS).value = CONTEXT_CONTROL
            ok = k32.GetThreadContext(h, ctypes.c_void_p(ctx_addr))
            rip = ctypes.c_uint64.from_address(ctx_addr + OFF_RIP).value if ok else 0
            rsp = ctypes.c_uint64.from_address(ctx_addr + OFF_RSP).value if ok else 0
            seen = set()
            chain = []
            if rsp and k32.ReadProcessMemory(process, ctypes.c_void_p(rsp), stack, ctypes.sizeof(stack),
                                             ctypes.byref(read)):
                for v in stack[:read.value // 8]:
                    if exe_base <= v < exe_base + exe_size:
                        seen.add(v)
                        if len(chain) < 12:
                            chain.append(v)
            k32.ResumeThread(h)
            if not rip:
                continue
            self_samples[tid][rip] += 1
            totals[tid] += 1
            for v in seen:
                incl_samples[tid][v] += 1
            chains[tid][(module_of(rip)[0] if not exe_base <= rip < exe_base + exe_size else rip,) + tuple(chain)] += 1
        time.sleep(max(0.0, period - (time.time() - now)))

    # Symbolize: exe addresses by llvm-symbolizer (function names), the rest by module.

    # Busiest: the threads with the most samples outside the wait functions of the system modules.
    idle_mods = ('ntdll.dll', 'win32u.dll', 'kernelbase.dll', 'kernel32.dll')
    busy = collections.Counter()
    for tid, samples in self_samples.items():
        for a, c in samples.items():
            if module_of(a)[0].lower() not in idle_mods:
                busy[tid] += c
    exe_addrs = set()
    busiest = [tid for tid, _ in busy.most_common(args.threads)]
    for tid in busiest:
        exe_addrs.update(a for a in self_samples[tid] if exe_base <= a < exe_base + exe_size)
        exe_addrs.update(incl_samples[tid])
        for c in chains[tid]:
            exe_addrs.update(a for a in c if isinstance(a, int))
    names = {}
    symbolizer = shutil.which('llvm-symbolizer') or r'C:\Program Files\LLVM\bin\llvm-symbolizer.exe'
    if exe_addrs:
        ordered = sorted(exe_addrs)
        query = '\n'.join(hex(0x140000000 + a - exe_base) for a in ordered) + '\n'
        res = subprocess.run([symbolizer, '--obj=' + os.path.abspath(args.pdb_exe), '--functions=short',
                              '--no-inlines', '--demangle'], input=query, capture_output=True, text=True)
        blocks = res.stdout.strip().split('\n\n')
        for a, block in zip(ordered, blocks):
            line = block.split('\n')[0].strip()
            names[a] = line if line and line != '??' else f'exe+{a - exe_base:#x}'

    def name(addr):
        if addr in names:
            return names[addr]
        mod, off = module_of(addr)
        return f'{mod}+{off:#x}' if mod != '?' else f'{addr:#x}'

    for tid in busiest:
        n = totals[tid]
        print(f'\n=== thread {tid}: {n} samples')
        by_fn = collections.Counter()
        for a, c in self_samples[tid].items():
            fn = name(a)
            if '+' in fn and not fn.startswith('exe+'):
                fn = fn.split('+')[0]  # a system module: by module
            by_fn[fn] += c
        print('  self:')
        for fn, c in by_fn.most_common(args.top):
            print(f'    {100.0 * c / n:5.1f}%  {fn}')
        incl = collections.Counter()
        for a, c in incl_samples[tid].items():
            incl[name(a)] += c
        print('  on the stack (approximate inclusive):')
        for fn, c in [(f, c) for f, c in incl.most_common() if not f.startswith('exe+')][:args.top]:
            print(f'    {100.0 * c / n:5.1f}%  {fn}')
        # Call chains: the top plus the named return addresses nearest the stack pointer (a scan: may hold stale ones).
        merged = collections.Counter()
        for c, k in chains[tid].items():
            top = c[0] if isinstance(c[0], str) else name(c[0])
            if '+' in top and not top.startswith('exe+'):
                top = top.split('+')[0]
            frames = [top]
            for a in c[1:]:
                f = name(a)
                if not f.startswith('exe+') and f != frames[-1]:
                    frames.append(f)
            merged[' < '.join(frames[:args.depth])] += k
        print('  chains (top < callers):')
        for ch, c in merged.most_common(args.top):
            print(f'    {100.0 * c / n:5.1f}%  {ch}')
    for h in handles.values():
        k32.CloseHandle(h)


if __name__ == '__main__':
    main()
