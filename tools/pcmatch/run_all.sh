#!/bin/sh
# Rebuild out/names/oblivion360_names.txt (needs C:\GOG Games\Oblivion\Oblivion.exe + OBSE source, tools/image.bin, generated/).
# Cache lives in out/names/cache. PC exe is only read, nothing from it is stored in the repo.
set -e
cd "$(dirname "$0")/../.."
python tools/pcmatch/obse_names.py      # OBSE function names (1.2.416 branches)
python tools/pcmatch/pc_index.py        # PC functions, RTTI vtables, strings (~1 min)
python tools/pcmatch/x360_index.py      # 360 functions from generated/ + image.bin
python tools/pcmatch/obse_vslots.py     # OBSE virtual slot names, validated against RTTI vtable sizes
python tools/pcmatch/match1.py          # NiRTTI vtable pairing + string anchors
python tools/pcmatch/match2.py          # vtable slot alignment, ctor/dtor, call-graph + address-order propagation
python tools/pcmatch/names.py           # names + out/names/oblivion360_{names,matches}.txt  (tools/pcmatch/manual_names.txt is merged in)
