#!/usr/bin/env python3
"""Moves reNut's D3D hook table (katamari-nx's hooks/{device,draw,resource,shader,state}.cpp) to Oblivion's
addresses (docs/renut-port.md, milestone 2), from tools/renut/hook_map.txt and the generated state hooks
(tools/renut/gen_state_hooks.py).

For every BANJO_RENUT_HOOK / _RAW / _UNHOOKED_HOOK[_SHARED] line:
  - the render and sampler state hooks are replaced by the generated block (at the first one's place);
  - a hook whose katamari address hook_map.txt pairs with an Oblivion function gets that address and the pairing's
    confidence in a comment; one with no Oblivion function ("none") is commented out;
  - an unhooked one (katamari could not place it) is hooked when Oblivion's game code calls the paired function
    (out/renut/d3d_api.txt): reNut's device is not Oblivion's layout, so the game's own copy cannot run on it;
  - the pairings hook_map.txt marks ARG SHIFT get ADAPTERS' handler instead (written by hand in the hook files).
Lines already rewritten carry "oblivion:" and are left alone, so the tool can run again.

  python tools/renut/remap_hooks.py --hooks <renderer>/gpu/d3d/hooks [--dry-run]
"""
import argparse
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HOOK = re.compile(r'^(BANJO_RENUT_(?:UNHOOKED_)?HOOK(?:_SHARED)?)\(sub_([0-9A-F]{8}), ([^)]*(?:<\d+>)?)\);(.*)$')
RAW = re.compile(r'^BANJO_RENUT_HOOK_RAW\(sub_([0-9A-F]{8})\)(.*)$')
STATE = re.compile(r'[SG]et(Render|Sampler)StateHook<')
# Oblivion's argument order differs from the handler's (hook_map.txt "ARG SHIFT"): these adapters, in the hook files.
ADAPTERS = {
    '824DE218': 'OblivionSetGammaRamp',           # (dev, flags, ramp)
    '824DE2F8': 'OblivionSetPWLGamma',            # (dev, flags, pwl)
    '824DF3F0': 'OblivionRingBufferDeviceAllocate',  # (context, dev, flags, pSize, alignment)
    '824DB210': 'OblivionLockResource',           # (resource, fence, type, ...)
    '824DB760': 'OblivionVertexBufferLock',       # returns base + offset
    '824DB870': 'OblivionIndexBufferLock',        # returns base + offset
}


def load_map(path):
    pairs = {}
    for line in open(path, encoding='utf-8'):
        if line.startswith('#') or not line.strip():
            continue
        f = line.split(None, 4)
        if len(f) < 4 or not f[1].startswith('sub_'):
            continue
        obl = f[2][4:] if f[2].startswith('sub_') else None
        pairs.setdefault(f[1][4:], (obl, f[3], f[0]))
    return pairs


def load_api(path):
    return {line[4:12] for line in open(path) if line.startswith('sub_')}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--hooks', required=True)
    ap.add_argument('--map', default=os.path.join(ROOT, 'tools', 'renut', 'hook_map.txt'))
    ap.add_argument('--api', default=os.path.join(ROOT, 'out', 'renut', 'd3d_api.txt'))
    ap.add_argument('--states', default=os.path.join(ROOT, 'out', 'renut', 'state_hooks.inc'))
    ap.add_argument('--dry-run', action='store_true')
    a = ap.parse_args()
    pairs, api = load_map(a.map), load_api(a.api)
    states = open(a.states, encoding='utf-8').read().rstrip('\n').split('\n')
    stats = {'hooked': 0, 'promoted': 0, 'none': 0, 'unhooked': 0, 'adapter': 0, 'unmapped': 0, 'state': 0}
    for name in ('device.cpp', 'draw.cpp', 'resource.cpp', 'shader.cpp', 'state.cpp'):
        path = os.path.join(a.hooks, name)
        lines = open(path, encoding='utf-8').read().split('\n')
        out, states_done = [], False
        for line in lines:
            if 'oblivion:' in line:
                out.append(line)
                continue
            m = HOOK.match(line)
            if m and STATE.search(m.group(3)):
                stats['state'] += 1
                if not states_done:
                    out.extend(states)
                    states_done = True
                continue
            r = RAW.match(line)
            if r:
                obl, conf, _ = pairs.get(r.group(1), (None, 'none', ''))
                if obl:
                    out.append(f'BANJO_RENUT_HOOK_RAW(sub_{obl}){r.group(2)}  // oblivion: katamari sub_{r.group(1)}, '
                               f'{conf}')
                    stats['hooked'] += 1
                else:
                    out.append(f'// oblivion: no counterpart, RAW of sub_{r.group(1)} needs one')
                    stats['unmapped'] += 1
                continue
            if not m:
                out.append(line)
                continue
            macro, kat, handler, rest = m.groups()
            if kat not in pairs:
                out.append(f'// oblivion: not in hook_map.txt: {line}')
                stats['unmapped'] += 1
                continue
            obl, conf, _ = pairs[kat]
            if not obl:
                out.append(f'// oblivion: not in XDK 2417: {line}')
                stats['none'] += 1
                continue
            if kat in ADAPTERS:
                handler = ADAPTERS[kat]
                stats['adapter'] += 1
            note = f'  // oblivion: katamari sub_{kat}, {conf}'
            if macro.startswith('BANJO_RENUT_UNHOOKED'):
                if obl in api and obl not in raw_targets:
                    macro = 'BANJO_RENUT_HOOK'
                    note += ', called by the game'
                    stats['promoted'] += 1
                else:
                    stats['unhooked'] += 1
            else:
                stats['hooked'] += 1
            out.append(f'{macro}(sub_{obl}, {handler});{rest}{note}')
        if not a.dry_run:
            with open(path, 'w', encoding='utf-8', newline='\n') as f:
                f.write('\n'.join(out))
    print(' '.join(f'{k}={v}' for k, v in stats.items()))


if __name__ == '__main__':
    main()
