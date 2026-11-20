#!/bin/sh
# Usage: tools/trial.sh <shotname> [exe args...]  -- run, wait 45s, screenshot, kill
N=$1; shift
cd "$(dirname "$0")/../out/build/local-release"
./oblivion.exe --log_level=info --log_file=../../../tools/trial_$N.log "$@" > /dev/null 2>&1 &
sleep 45
cd ../../..
powershell -ExecutionPolicy Bypass -File tools/shot.ps1 tools/shot_$N.png
taskkill //F //IM oblivion.exe > /dev/null 2>&1
