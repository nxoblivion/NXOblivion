#!/bin/sh
# Deploy / launch a Switch NRO from the PC over the LAN (no SD card swapping).
#
#   SWITCH_IP=192.168.1.50 ports/switch/deploy.sh ftp   [app.nro]   # persistent install via FTP
#   SWITCH_IP=192.168.1.50 ports/switch/deploy.sh run   [app.nro]   # nxlink launch + live log
#   SWITCH_IP=192.168.1.50 ports/switch/deploy.sh both  [app.nro]   # FTP install, then nxlink launch
#
# Env: SWITCH_IP    Switch address (required for ftp; optional for run -> nxlink broadcast discovery)
#      FTP_PORT     default 5000 (ftpd / sys-ftpd-light default; check the port the server shows)
#      FTP_USER / FTP_PASS   credentials; default anonymous (set both if your server needs login)
#      REMOTE_NRO   default /switch/oblivion/oblivion.nro (SD-relative)
#      NXLINK       explicit path to a nxlink binary (else PATH, devkitPro install, then Docker)
#      NXLINK_ARGS  extra program arguments passed to the NRO
# See ports/switch/DEBUGGING.md, section "Deploy from PC".
set -u

MODE="${1:-both}"
cd "$(dirname "$0")/../.." || exit 1

NRO="${2:-${NRO:-}}"
if [ -z "$NRO" ]; then
  NRO="$(ls -t out/*.nro 2>/dev/null | head -n 1)"
fi
[ -f "$NRO" ] || { echo "error: NRO not found (pass a path as arg 2 or set NRO=); looked in out/*.nro" >&2; exit 2; }

FTP_PORT="${FTP_PORT:-5000}"
FTP_USER="${FTP_USER:-anonymous}"
FTP_PASS="${FTP_PASS:-anonymous@}"
REMOTE_NRO="${REMOTE_NRO:-/switch/oblivion/oblivion.nro}"
NXLINK_ARGS="${NXLINK_ARGS:-}"

need_ip() {
  [ -n "${SWITCH_IP:-}" ] || { echo "error: set SWITCH_IP=<switch ip> (shown in ftpd's screen / Switch network settings)" >&2; exit 2; }
}

do_ftp() {
  need_ip
  command -v curl >/dev/null 2>&1 || { echo "error: curl not found" >&2; exit 2; }
  echo "[ftp] checking ftp://$SWITCH_IP:$FTP_PORT ..."
  if ! curl -s --connect-timeout 5 -u "$FTP_USER:$FTP_PASS" "ftp://$SWITCH_IP:$FTP_PORT/" -o /dev/null; then
    echo "error: cannot reach/login to an FTP server at $SWITCH_IP:$FTP_PORT." >&2
    echo "  - Is the Switch awake and on the same LAN? (ping $SWITCH_IP)" >&2
    echo "  - Is ftpd (NRO) running, or sys-ftpd-light enabled (Tesla overlay)? Check the port it displays." >&2
    echo "  - sys-ftpd-light 1.0.4+ rejects anonymous: set FTP_USER/FTP_PASS to match config.ini." >&2
    exit 3
  fi
  echo "[ftp] uploading $NRO ($(wc -c < "$NRO") bytes) -> $REMOTE_NRO"
  # --ftp-create-dirs makes /switch/oblivion if missing. Upload to .part then rename so a dropped
  # connection never leaves a truncated NRO that hbmenu would try to launch.
  curl --connect-timeout 5 --ftp-create-dirs -u "$FTP_USER:$FTP_PASS" \
       -T "$NRO" "ftp://$SWITCH_IP:$FTP_PORT${REMOTE_NRO}.part" \
       -Q "-RNFR ${REMOTE_NRO}.part" -Q "-RNTO ${REMOTE_NRO}" --progress-bar \
    || { echo "error: FTP upload failed" >&2; exit 3; }
  echo "[ftp] done"
}

find_nxlink() {
  if [ -n "${NXLINK:-}" ]; then echo "$NXLINK"; return; fi
  for c in nxlink nxlink.exe /c/devkitPro/tools/bin/nxlink.exe; do
    if command -v "$c" >/dev/null 2>&1; then command -v "$c"; return; fi
  done
  echo ""
}

do_run() {
  ts="$(date +%Y%m%d-%H%M%S)"
  mkdir -p out/switch-logs
  LOG="out/switch-logs/$ts.txt"
  NX="$(find_nxlink)"
  ARGS=""
  [ -n "${SWITCH_IP:-}" ] && ARGS="-a $SWITCH_IP"
  if [ -n "$NX" ]; then
    echo "[nxlink] $NX $ARGS -s $NRO  (log -> $LOG)"
    echo "[nxlink] Switch must be sitting in hbmenu with the netloader open (press Y)."
    # shellcheck disable=SC2086
    "$NX" $ARGS -s "$NRO" ${NXLINK_ARGS:+--args "$NXLINK_ARGS"} 2>&1 | tee "$LOG"
    rc=$?
  else
    # Docker fallback. TCP upload works through Docker Desktop NAT, UDP discovery does not (need SWITCH_IP),
    # and the -s callback (Switch -> PC:28771) only works if the port is published and the Switch's
    # connection reaches the container. UNVERIFIED; prefer a native nxlink.exe.
    need_ip
    echo "[nxlink] no native nxlink found; trying Docker (unverified path)."
    export MSYS_NO_PATHCONV=1
    ROOT="$(pwd -W 2>/dev/null || pwd)"
    docker run --rm -p 28771:28771 -v "$ROOT:/work" -w /work devkitpro/devkita64:latest \
      /opt/devkitpro/tools/bin/nxlink -a "$SWITCH_IP" -s "$NRO" 2>&1 | tee "$LOG"
    rc=$?
  fi
  if grep -q "No response from Switch" "$LOG" 2>/dev/null; then
    echo "error: no response from the Switch. Open the netloader in hbmenu (Y), same LAN/Wi-Fi, or set SWITCH_IP." >&2
    exit 4
  fi
  echo "[nxlink] finished (rc=$rc), log saved: $LOG"
}

case "$MODE" in
  ftp)  do_ftp ;;
  run)  do_run ;;
  both) do_ftp; do_run ;;
  *) echo "usage: SWITCH_IP=<ip> $0 {ftp|run|both} [app.nro]" >&2; exit 2 ;;
esac
