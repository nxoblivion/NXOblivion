#!/bin/bash
# A tools/run_eden.sh run with a capture of the Eden window every <every> seconds (tools/eden_shot.ps1, unfocused):
# nx/out/eden-<tag>/shot_<seconds>.png. Waits for an earlier test Eden to end first; never starts a second one.
# Usage: [EDEN_DIR=...] nx/tools/eden_shots.sh <seconds> <tag> <every> ["extra toml lines"]
SECS=${1:?seconds}; TAG=${2:?tag}; EVERY=${3:-60}; EXTRA=${4:-}
NX="$(cd "$(dirname "$0")/.." && pwd)"
for i in $(seq 1 60); do tasklist | grep -qi "^eden" || break; sleep 5; done
bash "$NX/tools/run_eden.sh" "$SECS" "$TAG" "$EXTRA" > "$NX/out/run-$TAG.txt" 2>&1 &
OUT="$NX/out/eden-$TAG"
for i in $(seq 1 30); do [ -s "$OUT/pid.raw" ] && break; sleep 1; done
PID=$(tr -dc '0-9' < "$OUT/pid.raw")
T=0
while [ $((T + EVERY)) -lt "$SECS" ]; do
  sleep "$EVERY"; T=$((T + EVERY))
  powershell -NoProfile -ExecutionPolicy Bypass -File "$NX/tools/eden_shot.ps1" -ProcessId "$PID" \
    -Out "$(cygpath -w "$OUT")\\shot_$T.png" || break
done
wait
cat "$NX/out/run-$TAG.txt"
