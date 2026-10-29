#!/usr/bin/env bash
# Starts a GUI binary without a display and fails if it quits within 5 seconds.
# This catches missing libraries, plugins, and broken library paths in a package.
#
# Usage: smoke-launch.sh <executable>
set -euo pipefail

EXE="$1"
LOG_DIR="$(mktemp -d)"

QT_QPA_PLATFORM=offscreen "$EXE" >"$LOG_DIR/stdout" 2>"$LOG_DIR/stderr" &
PID=$!
sleep 5
if ! kill -0 "$PID" 2>/dev/null; then
    RC=0
    wait "$PID" || RC=$?
    echo "$EXE exited prematurely with code $RC" >&2
    echo "--- stdout ---" >&2
    cat "$LOG_DIR/stdout" >&2 || true
    echo "--- stderr (last 200 lines) ---" >&2
    tail -n 200 "$LOG_DIR/stderr" >&2 || true
    exit 1
fi
kill "$PID" 2>/dev/null || true
wait "$PID" 2>/dev/null || true
echo "$EXE launched successfully and was terminated after 5s."
