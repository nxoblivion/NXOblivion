"""Cross-reference recompiled functions in generated/default.

Usage:
  xref.py callers <sub_XXXXXXXX>     functions that call it
  xref.py callees <sub_XXXXXXXX>     functions it calls
  xref.py asm <sub_XXXXXXXX>         its PPC disassembly (from the codegen comments)
"""
import glob
import os
import re
import sys

GEN = os.path.join(os.path.dirname(__file__), '..', 'generated', 'default')
FUNC_RE = re.compile(r'^DEFINE_REX_FUNC\((\w+)\)')
CALL_RE = re.compile(r'^\t(\w+)\(ctx, base\);')


def functions():
    for path in sorted(glob.glob(os.path.join(GEN, '*_recomp.*.cpp'))):
        name, body = None, []
        with open(path, encoding='utf-8', errors='replace') as f:
            for line in f:
                m = FUNC_RE.match(line)
                if m:
                    if name:
                        yield name, body
                    name, body = m.group(1), []
                elif name:
                    body.append(line.rstrip('\n'))
        if name:
            yield name, body


def main():
    mode, target = sys.argv[1], sys.argv[2]
    for name, body in functions():
        if mode == 'callers' and any(CALL_RE.match(l) and CALL_RE.match(l).group(1) == target for l in body):
            print(name)
        elif name == target:
            if mode == 'callees':
                print('\n'.join(sorted({CALL_RE.match(l).group(1) for l in body if CALL_RE.match(l)})))
            elif mode == 'asm':
                print('\n'.join(l[4:] for l in body if l.startswith('\t// ')))
            return


if __name__ == '__main__':
    main()
