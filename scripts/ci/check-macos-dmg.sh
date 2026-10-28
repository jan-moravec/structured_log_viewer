#!/usr/bin/env bash
# Verifies the DMG from package-macos.sh and smoke-launches the deployed
# .app bundle headless.
#
# `slp` lives beside the .app with no tzdata next to it, so the check also
# exercises the UTC fallback: it must warn once and still run.
#
# Environment: APP_NAME (default StructuredLogViewer).
set -euo pipefail

APP_NAME="${APP_NAME:-StructuredLogViewer}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

MOUNT="$(mktemp -d)"
hdiutil attach "$APP_NAME.dmg" -mountpoint "$MOUNT" -nobrowse -quiet
trap 'hdiutil detach "$MOUNT" -quiet || true' EXIT
test -x "$MOUNT/slp"
test -d "$MOUNT/$APP_NAME.app"
test -f "$MOUNT/THIRD_PARTY_LICENSES.txt"
if [ -e "$MOUNT/$APP_NAME.app/Contents/MacOS/slp" ]; then
    echo "slp must not be inside the .app bundle" >&2
    exit 1
fi
"$MOUNT/slp" --version
echo '{"msg":"hello"}' | "$MOUNT/slp" query --tz Europe/Prague - 2>slp.stderr
test "$(grep -c 'timezone data was not found' slp.stderr)" -eq 1

EXE="build/release/bin/Release/$APP_NAME.app/Contents/MacOS/$APP_NAME"
if [ ! -x "$EXE" ]; then
    echo "Packaged .app is missing executable at $EXE" >&2
    exit 1
fi
"$SCRIPT_DIR/smoke-launch.sh" "$EXE"
