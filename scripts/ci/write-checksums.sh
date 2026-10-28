#!/usr/bin/env bash
# Writes a `<file>.sha256` next to each file in `sha256sum` format.
#
# Usage: write-checksums.sh <file>...
set -euo pipefail

for file in "$@"; do
    if [ ! -f "$file" ]; then
        echo "Cannot checksum missing file: $file" >&2
        exit 1
    fi
    if command -v sha256sum >/dev/null; then
        sha256sum "$file" >"$file.sha256"
    else
        shasum -a 256 "$file" >"$file.sha256"
    fi
    cat "$file.sha256"
done
