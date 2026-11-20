"""Fixes Eden v0.2.1's deko3d crash in a private Eden copy (eden-cli.exe of the official v0.2.1 Windows MSVC build).

Eden v0.2.1's MacroEngine::Execute (video_core/macro.cpp), for a macro called at an offset inside an uploaded block
(every deko3d program does that at dkQueueCreate), takes a reference to the block's code in uploaded_macro_code (an
ankerl::unordered_dense::map, whose values live in one std::vector) and then inserts uploaded_macro_code[method]: the
insert reallocates the vector, the reference dangles, and code.resize() throws std::length_error on Eden's GPU thread
(abort, 0xC0000409). Eden's master has since rewritten that path (a copy, then emplace). This patch swaps the two
lookups in the machine code (RVA 0x3a626b-0x3a62ae): insert first, then look up the block.

The original bytes are checked first; anything else is refused. Keep a copy: cp eden-cli.exe eden-cli.exe.orig
Usage: python -I nx/tools/eden_patch_macro.py <eden copy>/eden-cli.exe
"""
import struct, sys
exe = sys.argv[1]; d = bytearray(open(exe, 'rb').read())
pe = struct.unpack_from('<I', d, 0x3c)[0]; nsec = struct.unpack_from('<H', d, pe + 6)[0]; optsz = struct.unpack_from('<H', d, pe + 20)[0]
def off(rva):
    for i in range(nsec):
        o = pe + 24 + optsz + 40 * i; vsz, va, rsz, raw = struct.unpack_from('<IIII', d, o + 8)
        if va <= rva < va + max(vsz, rsz): return raw + rva - va
START, END, OPIDX = 0x3a626b, 0x3a62ae, 0x225c50
o = off(START)
orig = bytes(d[o:o + END - START])
expect = bytes.fromhex('4c8d7340' '488d95c0010000' '4c89f1') 
assert orig.startswith(expect), orig.hex()
assert orig[-3:] == bytes.fromhex('4989c6'), orig[-3:].hex()
code = bytearray()
def call():
    rel = OPIDX - (START + len(code) + 5); code.extend(b'\xe8' + struct.pack('<i', rel))
code += bytes.fromhex('4c8d7340')          # lea r14,[rbx+0x40]   (&uploaded_macro_code)
code += bytes.fromhex('488d95ec010000')    # lea rdx,[rbp+0x1ec]  (&method)
code += bytes.fromhex('4c89f1'); call()    # mov rcx,r14; call operator[]  -> &code (inserts)
code += bytes.fromhex('4989c7')            # mov r15,rax
code += bytes.fromhex('488d95c0010000')    # lea rdx,[rbp+0x1c0]  (&mid_method)
code += bytes.fromhex('4c89f1'); call()    # mov rcx,r14; call operator[]  -> &macro_cached (exists)
code += bytes.fromhex('4d89fe')            # mov r14,r15  (code)
code += bytes.fromhex('4989c7')            # mov r15,rax  (macro_cached)
code += bytes.fromhex('4c89f0')            # mov rax,r14  (the code after reads code through rax too)
code += bytes.fromhex('448ba5ec010000')    # mov r12d,[rbp+0x1ec]
code += bytes.fromhex('442ba5c0010000')    # sub r12d,[rbp+0x1c0] (rebased_method)
pad = END - START - len(code); assert pad >= 0
code += bytes.fromhex('662e0f1f840000000000')[:pad] if pad == 10 else b'\x90' * pad
d[o:o + len(code)] = code
open(exe, 'wb').write(d); print('patched', len(code), 'bytes, pad', pad)
