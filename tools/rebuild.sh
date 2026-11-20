#!/bin/sh
cd "$(dirname "$0")/.."
cmd //c "tools\build.bat ${1:-local-release}" > tools/build.log 2>&1
grep -E "error|FAILED" tools/build.log | head -20; tail -2 tools/build.log
