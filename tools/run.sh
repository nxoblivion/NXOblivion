#!/bin/sh
# Usage: tools/run.sh [seconds] [preset]   -- builds nothing, runs exe with logging
cd "$(dirname "$0")/../out/build/${2:-local-release}" || exit 1
rm -f ../../../tools/run.log; timeout ${1:-40} ./oblivion.exe --log_file=../../../tools/run.log --log_level=${LOGLEVEL:-info} > ../../../tools/run.out 2>&1
echo "exit $?"
tail -25 ../../../tools/run.log
