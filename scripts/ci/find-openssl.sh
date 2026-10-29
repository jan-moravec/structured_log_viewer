#!/usr/bin/env bash
# Installs OpenSSL 3 with Homebrew (no-op if present) and sets OPENSSL_ROOT_DIR for CMake.
set -euo pipefail

brew install openssl@3
OPENSSL_PREFIX="$(brew --prefix openssl@3)"
if [ ! -x "$OPENSSL_PREFIX/bin/openssl" ] || [ ! -d "$OPENSSL_PREFIX/include/openssl" ]; then
    echo "OpenSSL prefix $OPENSSL_PREFIX is missing openssl or headers" >&2
    ls -la "$OPENSSL_PREFIX" || true
    exit 1
fi
"$OPENSSL_PREFIX/bin/openssl" version
if [ -n "${GITHUB_ENV:-}" ]; then
    echo "OPENSSL_ROOT_DIR=$OPENSSL_PREFIX" >>"$GITHUB_ENV"
fi
