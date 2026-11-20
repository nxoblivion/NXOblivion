#!/bin/sh
# Usage: tools/ab.sh <name> [exe args...] -- boot, wait 50s for the auto "Start new game?" dialog, screenshot, kill
N=$1; shift
cd "$(dirname "$0")/.." || exit 1
taskkill //F //IM oblivion.exe > /dev/null 2>&1
(cd out/build/local-release && ./oblivion.exe --log_level=info --log_file=../../../tools/ab_$N.log "$@" > /dev/null 2>&1 &)
sleep 50
powershell -ExecutionPolicy Bypass -File tools/keys.ps1 oblivion tools/ab_$N.png ${KEYS:-}
taskkill //F //IM oblivion.exe > /dev/null 2>&1
