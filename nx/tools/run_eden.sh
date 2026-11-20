#!/bin/bash
# Runs the reNut Switch build (app/out/switch/oblivion.nro) in a private Eden copy and kills ONLY the Eden it started
# (after ports/switch/oblivion_gl/run-eden.sh). The NRO goes to sdmc:/switch/reoblivion/reoblivion.nro with
# an oblivion.toml beside it (the player's file: built-in settings + "extra toml lines"); the game is the SD's
# sdmc:/oblivion/assets (src/oblivion_app.h). Results in nx/out/eden-<tag>: the SD folder's logs, Eden's log.
# Usage: [EDEN_DIR=../tools/eden-gl2] [EDEN_SD_AS_IS=1] nx/tools/run_eden.sh [seconds] [tag] ["extra toml lines"] [nro]
# Never while the user plays in that Eden copy: it is killed at <seconds>.
SECS=${1:-240}; TAG=${2:-run}; EXTRA=${3:-}
NX="$(cd "$(dirname "$0")/.." && pwd)"
NRO=${4:-$NX/app/out/switch/oblivion.nro}
EDEN="${EDEN_DIR:-$NX/../tools/eden-gl2}"; DIR="$EDEN/user/sdmc/switch/reoblivion"; OUT="$NX/out/eden-$TAG"
if tasklist | grep -qi "^eden"; then echo "an Eden is running (the user's?): not starting another"; exit 1; fi
mkdir -p "$DIR" "$OUT"
# EDEN_GDB=1: Eden's GDB stub on (port 6543); Eden then waits at the first instruction for tools/eden_rsp.py.
GDB_ON=$([ "${EDEN_GDB:-0}" = 1 ] && echo true || echo false)
for ini in "$EDEN/user/config/sdl2-config.ini" "$EDEN/user/config/qt-config.ini"; do
  [ -f "$ini" ] && sed -i "s/^use_gdbstub\\\\default=true/use_gdbstub\\\\default=false/; s/^use_gdbstub=.*/use_gdbstub=$GDB_ON/" "$ini"
done
# A fresh logs folder per run (the previous one moved aside).
[ -d "$DIR/logs" ] && mv "$DIR/logs" "$DIR/logs.prev.$$"
# EDEN_CAP: the run's frame cap (30 as on the card; 0 = none, to compare two builds by the game thread's time).
printf 'oblivion_renut_frame_cap = %s\n%s\n' "${EDEN_CAP:-30.0}" "$EXTRA" > "$DIR/oblivion.toml"
# EDEN_SD_AS_IS=1: the card's NRO and shader files as they are (what the installer wrote: nx/tools/build_kit.sh).
if [ "${EDEN_SD_AS_IS:-0}" != 1 ]; then
  cp -f "$NRO" "$DIR/reoblivion.nro"
  # The PC-built DKSH cache (tools/deko3d_variant_cache.py) when it is newer than the card's (the console appends to it).
  CACHE="$NX/out/deko3d_shaders/deko3d_shaders.bin"
  [ -f "$CACHE" ] && [ "$CACHE" -nt "$DIR/deko3d_shaders.bin" ] && cp -f "$CACHE" "$DIR/deko3d_shaders.bin"
fi
WEXE="$(cygpath -w "$EDEN/${EDEN_EXE:-eden-cli.exe}")"; WEDEN="$(cygpath -w "$EDEN")"
WNRO="$(cygpath -w "$DIR/reoblivion.nro")"; WOUT="$(cygpath -w "$OUT")"
powershell -NoProfile -Command "\$p = Start-Process -FilePath '$WEXE' -ArgumentList '-g','\"$WNRO\"' -WorkingDirectory '$WEDEN' -RedirectStandardOutput '$WOUT\eden_stdout.txt' -RedirectStandardError '$WOUT\eden_stderr.txt' -PassThru; Set-Content -Path '$WOUT\pid.raw' -Value \$p.Id" > /dev/null 2>&1 < /dev/null
PID=$(tr -dc '0-9' < "$OUT/pid.raw")
echo "eden pid $PID (killed at ${SECS} s)" | tee "$OUT/pid.txt"
alive() { powershell -NoProfile -Command "if (Get-Process -Id $PID -ErrorAction SilentlyContinue) { exit 0 } else { exit 1 }" >/dev/null 2>&1; }
T0=$(date +%s); EXITED=0
while :; do
  sleep 10
  if ! alive; then EXITED=1; break; fi
  [ $(( $(date +%s) - T0 )) -ge "$SECS" ] && break
done
echo "elapsed $(( $(date +%s) - T0 )) s, eden exited by itself: $EXITED" | tee -a "$OUT/pid.txt"
cp -rf "$DIR/logs" "$OUT/" 2>/dev/null; cp -f "$DIR"/*.txt "$OUT/" 2>/dev/null
cp -f "$EDEN/user/log/eden_log.txt" "$OUT/" 2>/dev/null
[ "$EXITED" = 0 ] && powershell -NoProfile -Command "Stop-Process -Id $PID -Force -ErrorAction SilentlyContinue"
echo "done: $OUT"
