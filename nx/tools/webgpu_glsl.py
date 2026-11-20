"""reOblivion web (2026-11-18): SPIRV-Cross's Vulkan GLSL -> GLSL whose SPIR-V tint takes (WebGPU's WGSL).

    python tools/webgpu_glsl.py in.vk.glsl out.glsl --stage vert|frag|comp   (prints the binding line)

The input is `spirv-cross x.spv --version 460 --vulkan-semantics --relax-nan-checks` of a shader DXC compiled with
-DBANJO_DEKO3D (no push-constant addresses, no 64-bit buffer addresses: the deko3d flavour, shader_common.h) and without
-fvk-invert-y (WebGPU's clip space is D3D's). WebGPU has no descriptor heaps (runtime arrays of textures), no push
constants and at most 4 bind groups, so:

- Texture tables (Renut's heaps, the host shaders' tables): each distinct index expression of a table becomes one bound
  texture, and each distinct sampler index one bound sampler. The index has to be a word the back end can read when the
  draw is recorded: a word of SharedConstants (the UBO g_UboShared: game shaders) or of the push constants (host
  shaders). plume_webgpu reads it there (masked: Renut's flags above bit 26) and binds that heap slot's view. Group 0;
  binding = stage base + k (textures) or + 16 + k (samplers): vertex 32, fragment 0, compute 0, so the two stages of a
  pipeline never share a binding number.
- Constant buffers: the game shaders' sets 5, 6, 7 (vertex floats, pixel floats, SharedConstants) are group 1,
  bindings 0, 1, 2 (dynamic offsets); host shaders' other constant buffers group 1 binding 3 and on.
- Storage buffers: group 2, in the order of their (set, binding); the deko3d address window (set 15, binding 15) is
  binding 0 and the memexport buffer (set 15, binding 14) binding 1.
- Push constants: a std140 uniform block at group 3, binding 0 (plume_webgpu copies the pushed bytes into a ring and
  binds it with a dynamic offset).
- Vertex inputs: renumbered 0.. in order (WebGPU has 16 attributes; Renut's locations reach 22).
- Specialization constants stay (tint makes them WGSL overrides, set per pipeline).

The binding line (first line of the output, "// webgpu ...") lists what the back end binds: tex=<binding>:<type>:<heap
set>:<source><word>:<f sampled | u loaded only>,... (source s = SharedConstants, p = push constants; word = byte offset), smp=<binding>:<heap
set>:<source><word>,..., ubo=<bindings>, ssbo=<binding><r|w>@<set>.<binding it had>,..., push=<bytes>,
vin=<original locations>.

Raises Unsupported with the reason when the shader has something this does not handle.
"""
import argparse
import re
import sys

STAGE_BASE = {'vert': 32, 'frag': 0, 'comp': 0}
GAME_UBO_SETS = {5: 0, 6: 1, 7: 2}
RAW_WINDOW_SET = 15
PUSH_GROUP = 3
TEX_TYPES = ('texture1D', 'texture2D', 'texture3D', 'textureCube', 'texture2DArray', 'textureCubeArray',
             'texture2DMS', 'texture2DMSArray', 'utexture2D', 'itexture2D', 'utexture3D', 'utexture2DMS')
DROPPED_EXTENSIONS = ('GL_EXT_nonuniform_qualifier', 'GL_EXT_scalar_block_layout', 'GL_EXT_spirv_intrinsics')


NAN_HELPERS = (
    'bool web_isnan(float x) { return (floatBitsToUint(x) & 0x7FFFFFFFu) > 0x7F800000u; }\n'
    'bool web_isinf(float x) { return (floatBitsToUint(x) & 0x7FFFFFFFu) == 0x7F800000u; }\n'
    + ''.join(f'bvec{n} web_isnan(vec{n} x) {{ return greaterThan(floatBitsToUint(x) & uvec{n}(0x7FFFFFFFu), '
              f'uvec{n}(0x7F800000u)); }}\n'
              f'bvec{n} web_isinf(vec{n} x) {{ return equal(floatBitsToUint(x) & uvec{n}(0x7FFFFFFFu), '
              f'uvec{n}(0x7F800000u)); }}\n' for n in (2, 3, 4)))


class Unsupported(Exception):
    pass


def matching(text, open_index, open_ch, close_ch):
    depth = 0
    for i in range(open_index, len(text)):
        c = text[i]
        if c == open_ch:
            depth += 1
        elif c == close_ch:
            depth -= 1
            if depth == 0:
                return i
    raise Unsupported(f'unbalanced {open_ch}{close_ch} at {open_index}')


def strip_nonuniform(text):
    out = []
    i = 0
    key = 'nonuniformEXT('
    while True:
        j = text.find(key, i)
        if j < 0:
            out.append(text[i:])
            return ''.join(out)
        out.append(text[i:j])
        close = matching(text, j + len(key) - 1, '(', ')')
        out.append('(' + text[j + len(key):close] + ')')
        i = close + 1


def array_index(text, name_end):
    """For `NAME[expr]` with NAME ending at name_end: (expr, index after ']') or None."""
    k = name_end
    while k < len(text) and text[k] == ' ':
        k += 1
    if k >= len(text) or text[k] != '[':
        return None
    close = matching(text, k, '[', ']')
    return text[k + 1:close], close + 1


def unwrap(e):
    e = e.strip()
    while e.startswith('(') and matching(e, 0, '(', ')') == len(e) - 1:
        e = e[1:-1].strip()
    return e


def strip_mask(e):
    """Renut's descriptor index mask (& 0x03FFFFFF, renutDescriptor) and uint() casts taken off."""
    for _ in range(6):
        e = unwrap(e)
        m = re.match(r'^(uint|floatBitsToUint|int)\((.*)\)$', e, re.S)
        if m and matching(e, len(m.group(1)), '(', ')') == len(e) - 1:
            e = m.group(2)
            continue
        m = re.match(r'^(.*?)\s*&\s*(67108863u|0x03ffffffu|0x3ffffffu)$', e, re.S | re.I)
        if m:
            left = m.group(1)
            depth = 0
            ok = True
            for c in left:
                depth += c in '(['
                depth -= c in ')]'
                if depth < 0:
                    ok = False
            if ok and depth == 0:
                e = left
                continue
        break
    return unwrap(e)


class Words:
    """Where index expressions come from: SharedConstants vectors and push constant members."""

    def __init__(self, text):
        self.temps = {m.group(1): m.group(2).strip()
                      for m in re.finditer(r'^\s*(?:uint|int|float) (_\d+) = ([^;]+);', text, re.M)}
        self.push_name = None
        self.push_offsets = {}
        m = re.search(r'layout\(push_constant(?:, std430)?\) uniform \w+\n\{\n(.*?)\n\} (\w+);', text, re.S)
        if m:
            self.push_name = m.group(2)
            self.push_offsets = push_offsets(m.group(1))

    def resolve(self, expr):
        e = strip_mask(expr)
        for _ in range(8):
            if e in self.temps:
                e = strip_mask(self.temps[e])
            else:
                break
        m = re.match(r'^g_UboShared\.v\[(\d+)u?\]\.([xyzw])$', e)
        if m:
            return 's', int(m.group(1)) * 16 + 'xyzw'.index(m.group(2)) * 4
        m = re.match(r'^g_UboShared\.v\[(\d+)u?\]\[(\d)u?\]$', e)
        if m:
            return 's', int(m.group(1)) * 16 + int(m.group(2)) * 4
        if self.push_name:
            m = re.match(r'^' + re.escape(self.push_name) + r'\.(\w+)$', e)
            if m and m.group(1) in self.push_offsets:
                return 'p', self.push_offsets[m.group(1)]
        return None


SCALAR_SIZES = {'float': 4, 'uint': 4, 'int': 4, 'bool': 4}
VEC_RE = re.compile(r'^(u|i|b)?vec([234])$')


def type_size_align(t):
    if t in SCALAR_SIZES:
        return 4, 4
    m = VEC_RE.match(t)
    if m:
        n = int(m.group(2))
        return 4 * n, 16 if n >= 3 else 8
    m = re.match(r'^mat([234])$', t)
    if m:
        n = int(m.group(1))
        return 16 * n, 16
    raise Unsupported(f'push constant member type {t}')


def push_offsets(body):
    """Byte offsets of a std430 push block's members (explicit layout(offset = N) wins)."""
    out = {}
    off = 0
    for line in body.split('\n'):
        line = line.strip()
        if not line:
            continue
        m = re.match(r'^(?:layout\(offset = (\d+)\) )?(\w+) (\w+)(\[(\d+)\])?;$', line)
        if not m:
            raise Unsupported('push constant member not understood: ' + line)
        size, align = type_size_align(m.group(2))
        if m.group(4):
            if m.group(2) in ('float', 'uint', 'int', 'vec2', 'uvec2', 'ivec2', 'bool'):
                raise Unsupported('push constant array with a std430-only stride: ' + line)
            size *= int(m.group(5))
        if m.group(1):
            off = int(m.group(1))
        else:
            off = (off + align - 1) & ~(align - 1)
        out[m.group(3)] = off
        off += size
    out['__size__'] = off
    return out


def convert(src, stage):
    text = src.replace('\r\n', '\n')
    base = STAGE_BASE[stage]
    words = Words(text)

    # ---- texture and sampler tables
    tables = {}  # name -> (type, set)
    samplers = {}  # name -> set
    decl_re = re.compile(r'^layout\(set = (\d+), binding = (\d+)\) uniform (\w+) (\w+)(\[\d*\])?;\n', re.M)

    single_textures = []

    def take_decl(m):
        s, b, typ, name, arr = int(m.group(1)), int(m.group(2)), m.group(3), m.group(4), m.group(5)
        if typ == 'sampler':
            if not arr:
                raise Unsupported(f'single sampler {name}')
            samplers[name] = s
            return ''
        if typ in TEX_TYPES:
            if not arr:
                single_textures.append((s, b, typ, name))
                return m.group(0)
            tables[name] = (typ, s)
            return ''
        raise Unsupported(f'uniform declaration {typ} {name}')

    text = decl_re.sub(take_decl, text)
    if single_textures:
        raise Unsupported('single (non-table) textures: ' + ', '.join(t[3] for t in single_textures))
    text = strip_nonuniform(text)

    tex_bind = {}  # (type, set, src, word) -> k
    smp_bind = {}  # (set, src, word) -> k

    def tex_var(name, expr):
        typ, s = tables[name]
        w = words.resolve(expr)
        if w is None:
            raise Unsupported(f'texture index not a constant word: {name}[{expr.strip()[:120]}]')
        key = (typ, s) + w
        if key not in tex_bind:
            tex_bind[key] = len(tex_bind)
        return f'web_t{tex_bind[key]}'

    def smp_var(name, expr):
        s = samplers[name]
        w = words.resolve(expr)
        if w is None:
            raise Unsupported(f'sampler index not a constant word: {name}[{expr.strip()[:120]}]')
        key = (s,) + w
        if key not in smp_bind:
            smp_bind[key] = len(smp_bind)
        return f'web_s{smp_bind[key]}'

    names = list(tables) + list(samplers)
    if names:
        name_re = re.compile(r'\b(' + '|'.join(map(re.escape, names)) + r')\b')
        pos = 0
        out = []
        while True:
            m = name_re.search(text, pos)
            if not m:
                out.append(text[pos:])
                break
            idx = array_index(text, m.end())
            if idx is None:
                raise Unsupported(f'table {m.group(1)} used without an index')
            expr, after = idx
            out.append(text[pos:m.start()])
            out.append(tex_var(m.group(1), expr) if m.group(1) in tables else smp_var(m.group(1), expr))
            pos = after
        text = ''.join(out)
    if len(tex_bind) > 16 or len(smp_bind) > 16:
        raise Unsupported(f'{len(tex_bind)} textures, {len(smp_bind)} samplers')

    # ---- push constants -> std140 UBO in group 3
    push_size = 0
    m = re.search(r'^layout\(push_constant(?:, std430)?\) uniform (\w+)', text, re.M)
    if m:
        push_size = words.push_offsets.get('__size__', 0)
        text = text[:m.start()] + f'layout(set = {PUSH_GROUP}, binding = 0, std140) uniform {m.group(1)}' + text[m.end():]

    # ---- constant buffers
    ubos = []
    extra_ubo = 3

    def ubo_decl(m):
        nonlocal extra_ubo
        s, b = int(m.group(1)), int(m.group(2))
        if s == PUSH_GROUP and b == 0 and push_size:
            return m.group(0)
        if b == 0 and s in GAME_UBO_SETS:
            nb = GAME_UBO_SETS[s]
        else:
            nb = extra_ubo
            extra_ubo += 1
        ubos.append(nb)
        return f'layout(set = 1, binding = {nb}, std140) uniform'
    text = re.sub(r'layout\(set = (\d+), binding = (\d+), std140\) uniform', ubo_decl, text)

    # ---- storage buffers
    ssbo_re = re.compile(r'layout\(set = (\d+), binding = (\d+), std430\) ((?:readonly |writeonly |restrict )*)buffer (\w+)\n'
                         r'\{(.*?)\n\} (\w+)(\[(\d+)\])?;', re.S)
    found = sorted({(int(m.group(1)), int(m.group(2))) for m in ssbo_re.finditer(text)})
    slots = {}
    nxt = 2
    for s, b in found:
        if s == RAW_WINDOW_SET and b in (14, 15):
            slots[(s, b)] = 0 if b == 15 else 1
        else:
            slots[(s, b)] = nxt
            nxt += 1
    ssbos = []

    def ssbo_decl(m):
        if m.group(7):
            raise Unsupported('storage buffer array')
        slot = slots[(int(m.group(1)), int(m.group(2)))]
        quals = m.group(3) or ''
        # slot, r/w, and the (set, binding) it had: the descriptor set plume_webgpu takes the buffer from
        ssbos.append(f'{slot}{"r" if "readonly" in quals else "w"}@{m.group(1)}.{m.group(2)}')
        return f'layout(set = 2, binding = {slot}, std430) {quals}buffer {m.group(4)}\n{{{m.group(5)}\n}} {m.group(6)};'
    text = ssbo_re.sub(ssbo_decl, text)
    if stage == 'vert' and any(s.endswith('w') for s in ssbos):
        raise Unsupported('a vertex shader writes a storage buffer (WebGPU allows none): memexport')

    for bad in ('buffer_reference', 'push_constant', 'layout(set = 0', 'layout(set = 4', 'GL_EXT_buffer_reference',
                'gl_DrawID', 'subpassInput', 'spirv_instruction', 'uint64_t', 'int64_t', 'gl_BaseInstance',
                'gl_BaseVertex'):
        if bad in text:
            raise Unsupported(f'{bad} remains')

    # ---- early fragment tests: no WGSL form (WebGPU tests early by itself wherever the shader allows it)
    text = re.sub(r'^layout\(early_fragment_tests\) in;\n', '', text, flags=re.M)

    # ---- NaN and infinity tests: WGSL has neither (tint refuses OpIsNan); the bits are tested instead.
    if re.search(r'\bisnan\(|\bisinf\(', text):
        text = re.sub(r'\bisnan\(', 'web_isnan(', text)
        text = re.sub(r'\bisinf\(', 'web_isinf(', text)
        at = text.find('\nvoid main()')
        if at < 0:
            raise Unsupported('no main() for the NaN helpers')
        text = text[:at + 1] + NAN_HELPERS + text[at + 1:]

    # ---- vertex inputs
    vin = []
    if stage == 'vert':
        in_re = re.compile(r'^layout\(location = (\d+)\) in (\w+) (\w+)(\[\d+\])?;', re.M)
        decls = in_re.findall(text)
        if any(d[3] for d in decls):
            raise Unsupported('vertex input array')
        vin = sorted(int(d[0]) for d in decls)
        if len(vin) > 16:
            raise Unsupported(f'{len(vin)} vertex inputs')
        new_of = {old: i for i, old in enumerate(vin)}
        text = in_re.sub(lambda m: f'layout(location = {new_of[int(m.group(1))]}) in {m.group(2)} {m.group(3)};', text)

    # ---- declarations of the bound textures and samplers
    decls = []
    meta_tex = []
    for (typ, s, src, word), k in sorted(tex_bind.items(), key=lambda kv: kv[1]):
        decls.append(f'layout(set = 0, binding = {base + k}) uniform {typ} web_t{k};')
        # f: sampled through a sampler (a filterable binding); u: only loaded or measured (unfilterable: depth and
        # 32-bit float formats bind there too).
        sampled = re.search(r'\b[iu]?sampler\w*\(\s*web_t%d\s*,' % k, text) is not None
        meta_tex.append(f'{base + k}:{typ}:{s}:{src}{word}:{"f" if sampled else "u"}')
    meta_smp = []
    for (s, src, word), k in sorted(smp_bind.items(), key=lambda kv: kv[1]):
        decls.append(f'layout(set = 0, binding = {base + 16 + k}) uniform sampler web_s{k};')
        meta_smp.append(f'{base + 16 + k}:{s}:{src}{word}')

    meta = '// webgpu'
    if meta_tex:
        meta += ' tex=' + ','.join(meta_tex)
    if meta_smp:
        meta += ' smp=' + ','.join(meta_smp)
    if ubos:
        meta += ' ubo=' + ','.join(map(str, sorted(set(ubos))))
    if ssbos:
        meta += ' ssbo=' + ','.join(sorted(set(ssbos)))
    if push_size:
        meta += f' push={push_size}'
    if vin:
        meta += ' vin=' + ','.join(map(str, vin))

    lines = text.split('\n')
    lines = [l for l in lines if not any(l.startswith('#extension ' + e) for e in DROPPED_EXTENSIONS)]
    if not lines[0].startswith('#version'):
        raise Unsupported('no #version line')
    head = [meta]
    if tables:
        head.append('#extension GL_EXT_samplerless_texture_functions : enable')
    lines[1:1] = head + decls
    return '\n'.join(lines) + ('\n' if not lines[-1] == '' else ''), meta


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('input')
    ap.add_argument('output')
    ap.add_argument('--stage', required=True, choices=sorted(STAGE_BASE))
    a = ap.parse_args()
    try:
        out, meta = convert(open(a.input, encoding='utf-8').read(), a.stage)
    except Unsupported as e:
        print(f'webgpu_glsl: {a.input}: {e}', file=sys.stderr)
        return 1
    with open(a.output, 'w', encoding='utf-8', newline='\n') as f:
        f.write(out)
    print(meta)
    return 0


if __name__ == '__main__':
    sys.exit(main())
