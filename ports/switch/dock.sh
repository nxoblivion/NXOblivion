#!/bin/sh
# like sdk-docker.sh but with a named container (p3b-*) so it can be identified/killed individually
export MSYS_NO_PATHCONV=1
cd "$(dirname "$0")/../.." || exit 1
ROOT="$(pwd -W 2>/dev/null || pwd)"
exec docker run --rm --name "p3b-$$" -v "$ROOT:/work" -w /work devkitpro/devkita64:latest bash -c "export PATH=\$DEVKITPRO/devkitA64/bin:\$DEVKITPRO/tools/bin:\$PATH; $1"
