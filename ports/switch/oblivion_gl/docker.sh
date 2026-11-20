#!/bin/sh
# Like ports/switch/sdk-docker.sh but names the container (p4b-<tag>) so it never collides with other agents' containers.
# Usage: ports/switch/oblivion_gl/docker.sh <tag> "<shell command>"
export MSYS_NO_PATHCONV=1
cd "$(dirname "$0")/../../.." || exit 1
if pwd -W >/dev/null 2>&1; then ROOT="$(pwd -W)"; else ROOT="$(pwd)"; fi
exec docker run --rm --name "p4b-$1" -v "$ROOT:/work" -w /work devkitpro/devkita64:latest bash -c "export PATH=\$DEVKITPRO/devkitA64/bin:\$DEVKITPRO/tools/bin:\$PATH; $2"
