#!/usr/bin/env bash
# Checks the `slp` tarball after a fresh extract: all libraries resolve, none is Qt, libtbb comes from the bundle,
# and the bundled tzdata loads.
#
# Usage: check-slp-tarball.sh <tarball>
set -euo pipefail

TARBALL="$1"
rm -rf slp-check
mkdir slp-check
tar -xzf "$TARBALL" -C slp-check
SLP="$(echo slp-check/slp-*/slp)"
test -x "$SLP"
if ldd "$SLP" | grep -E 'not found|libQt6'; then
    echo "slp in the tarball has unresolved or Qt dependencies" >&2
    exit 1
fi
ldd "$SLP" | grep -q "$(dirname "$SLP")/lib/libtbb"
"$SLP" --version
# tzdata next to the binary must load, so no fallback warning.
echo '{"msg":"hello"}' | "$SLP" query --tz Europe/Prague - 2>slp.stderr
if grep -q 'timezone data was not found' slp.stderr; then
    echo "slp did not find the bundled tzdata" >&2
    exit 1
fi
