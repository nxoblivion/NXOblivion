#!/bin/sh
# Runs the nx/ PC build for N seconds with logging (builds nothing).
# Usage: nx/tools/run_pc.sh [seconds] [preset] [extra args...]   (default 60 s, win-amd64-release)
# Log: nx/out/run.log (stdout/stderr: nx/out/run.out). LOGLEVEL=debug for more.
# The window starts minimised and the audio is muted (the SDK fork's window_start_minimized / audio_mute);
# MINIMIZED=0 and MUTE=0 turn them off. vfs_index=false: no index file next to the game folder; vd_swap_log_every:
# a log line every 300 presented frames (both bootstrap-notes.md).
NX="$(cd "$(dirname "$0")/.." && pwd)"
SECS=${1:-60}
PRESET=${2:-win-amd64-release}
[ $# -gt 0 ] && shift
[ $# -gt 0 ] && shift
MIN=true; [ "${MINIMIZED:-1}" = 1 ] || MIN=false
MUTED=true; [ "${MUTE:-1}" = 1 ] || MUTED=false
cd "$NX/app/out/build/$PRESET" || exit 1
mkdir -p "$NX/out"
rm -f "$NX/out/run.log"
timeout "$SECS" ./oblivion.exe --log_file="$NX/out/run.log" --log_level="${LOGLEVEL:-info}" \
    --window_start_minimized="$MIN" --audio_mute="$MUTED" --vfs_index=false --vd_swap_log_every=300 "$@" > "$NX/out/run.out" 2>&1
echo "exit $?"
tail -25 "$NX/out/run.log"
