#!/bin/sh
# Run a command inside the devkitA64 container with the project mounted at /work.
# Usage: ports/switch/sdk-docker.sh "<shell command>"
# On Git Bash, MSYS_NO_PATHCONV=1 is set here to avoid path mangling.
export MSYS_NO_PATHCONV=1
cd "$(dirname "$0")/../.." || exit 1
if pwd -W >/dev/null 2>&1; then ROOT="$(pwd -W)"; else ROOT="$(pwd)"; fi
exec docker run --rm -v "$ROOT:/work" -w /work devkitpro/devkita64:latest bash -c "export PATH=\$DEVKITPRO/devkitA64/bin:\$DEVKITPRO/tools/bin:\$PATH; $1"
