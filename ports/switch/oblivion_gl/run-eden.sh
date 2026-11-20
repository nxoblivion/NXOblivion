#!/bin/bash
# Runs oblivion_gl.nro in a private Eden copy (tools/eden-gl), waits, collects log + screenshots, kills ONLY its own Eden PID.
# Usage: [EDEN_DIR=tools/eden-gl2] ports/switch/oblivion_gl/run-eden.sh [seconds] [tag] ["extra toml lines"] [nro]
# The Windows PID of the started eden-cli.exe is taken from Start-Process -PassThru (the old msys-PID / command-line
# "alive" check was unreliable and killed Eden early). The run ends after <seconds>, or earlier when Eden exits (crash).
SECS=${1:-240}; TAG=${2:-run}; EXTRA=${3:-};
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NRO=${4:-$ROOT/ports/switch/oblivion_gl/oblivion_gl.nro}
EDEN="${EDEN_DIR:-$ROOT/tools/eden-gl}"; SD="$EDEN/user/sdmc/oblivion"; OUT="$ROOT/out/gl-$TAG"
RUND="$SD/run_$TAG"; mkdir -p "$RUND/sh"
if [ -n "$NODUMP" ]; then printf "%s
" "$EXTRA"; else printf 'gl_screenshot_dir = "sdmc:/oblivion/run_%s"
gl_dump_shaders = "sdmc:/oblivion/run_%s/sh"
%s
' "$TAG" "$TAG" "$EXTRA" ; fi > "$SD/oblivion.toml"
mkdir -p "$OUT"; : > "$SD/log.txt"
# Run a private copy: Eden keeps the NRO open, so a rebuild could not replace it while a run is going.
cp -f "$NRO" "$OUT/oblivion_gl.nro"; NRO="$OUT/oblivion_gl.nro"
WEXE="$(cygpath -w "$EDEN/${EDEN_EXE:-eden-cli.exe}")"; WEDEN="$(cygpath -w "$EDEN")"; WNRO="$(cygpath -w "$NRO")"; WOUT="$(cygpath -w "$OUT")"
powershell -NoProfile -Command "\$p = Start-Process -FilePath '$WEXE' -ArgumentList '-g','\"$WNRO\"' -WorkingDirectory '$WEDEN' -RedirectStandardOutput '$WOUT\eden_stdout.txt' -RedirectStandardError '$WOUT\eden_stderr.txt' -PassThru; Set-Content -Path '$WOUT\pid.raw' -Value \$p.Id" > /dev/null 2>&1 < /dev/null
# (powershell's stdout must not be a pipe here: eden-cli inherits it and a $(...) would wait until Eden exits)
PID=$(tr -dc '0-9' < "$OUT/pid.raw")
echo "eden pid $PID" | tee "$OUT/pid.txt"
alive() { powershell -NoProfile -Command "if (Get-Process -Id $PID -ErrorAction SilentlyContinue) { exit 0 } else { exit 1 }" >/dev/null 2>&1; }
T0=$(date +%s); CRASHED=0
while :; do
  sleep 10
  NOW=$(date +%s)
  if ! alive; then CRASHED=1; break; fi
  [ $((NOW-T0)) -ge "$SECS" ] && break
done
echo "elapsed $(( $(date +%s)-T0 )) s, eden exited by itself: $CRASHED" | tee -a "$OUT/pid.txt"
cp -f "$SD/log.txt" "$SD/boot.txt" "$SD/perf.txt" "$OUT/" 2>/dev/null; cp -rf "$RUND"/* "$OUT/" 2>/dev/null
cp -f "$EDEN/user/log/eden_log.txt" "$OUT/" 2>/dev/null
[ "$CRASHED" = 0 ] && powershell -NoProfile -Command "Stop-Process -Id $PID -Force -ErrorAction SilentlyContinue"
echo "done: $OUT"
