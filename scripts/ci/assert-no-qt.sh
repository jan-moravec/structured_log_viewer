#!/usr/bin/env bash
# Fails if a Linux or macOS binary links Qt. `slp` must stay Qt-free.
#
# Usage: assert-no-qt.sh <binary>
set -euo pipefail

BINARY="$1"
test -x "$BINARY"
case "$(uname -s)" in
    Darwin) DEPS="$(otool -L "$BINARY")" ;;
    *) DEPS="$(ldd "$BINARY")" ;;
esac
if grep -E 'Qt6|libQt6' <<<"$DEPS"; then
    echo "$BINARY links Qt" >&2
    exit 1
fi
