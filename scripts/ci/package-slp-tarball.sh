#!/usr/bin/env bash
# Packages the standalone Linux `slp` tarball for servers, CI, and
# containers: no Qt, no FUSE. It bundles libtbb and the GCC C++ runtime the
# binary was built against (newer than the base distro's); glibc and
# OpenSSL 3 come from the host. The binary finds them through an
# $ORIGIN rpath and finds tzdata next to itself.
#
# Prints the tarball name and, under GitHub Actions, exports it as
# SLP_TARBALL.
set -euo pipefail

BIN_DIR="build/release/bin/Release"
VERSION="$("$BIN_DIR/slp" --version | awk '{print $2}')"
STAGE="slp-${VERSION}-linux-x86_64"

rm -rf "$STAGE"
mkdir -p "$STAGE/lib"
cp "$BIN_DIR/slp" "$STAGE/"
cp -r "$BIN_DIR/tzdata" "$STAGE/"
for lib in $(ldd "$STAGE/slp" | awk '/=> \// {print $3}' | grep -E '/(libtbb[^/]*|libstdc\+\+[^/]*|libgcc_s[^/]*)$'); do
    cp -L "$lib" "$STAGE/lib/"
done
test -n "$(ls "$STAGE"/lib/libtbb*)"
# Literal $ORIGIN: the dynamic loader expands it, not the shell.
# shellcheck disable=SC2016
patchelf --set-rpath '$ORIGIN/lib' "$STAGE/slp"
cp "$BIN_DIR/THIRD_PARTY_LICENSES.txt" "$BIN_DIR/LICENSE" "$STAGE/"
cp doc/cli.md "$STAGE/README.md"
tar -czf "$STAGE.tar.gz" "$STAGE"

echo "$STAGE.tar.gz"
if [ -n "${GITHUB_ENV:-}" ]; then
    echo "SLP_TARBALL=$STAGE.tar.gz" >>"$GITHUB_ENV"
fi
