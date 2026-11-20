#!/bin/sh
# Symbolize exe offsets from oblivion_crash.txt using the PDB.
D="$(dirname "$0")/../out/build/${1:-local-release}"
for off in $(grep -o 'oblivion.exe+0x[0-9a-f]*' $D/oblivion_crash.txt | sed 's/.*+//'); do
  echo "== $off"; "/c/Program Files/LLVM/bin/llvm-symbolizer.exe" --relative-address --obj=$D/oblivion.exe $off | head -2
done
