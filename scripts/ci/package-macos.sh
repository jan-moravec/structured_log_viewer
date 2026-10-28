#!/usr/bin/env bash
# Builds the macOS DMG: the deployed .app bundle, plus `slp` and its TBB
# dylib beside it (not inside the bundle).
#
# POST_BUILD rules in app/CMakeLists.txt stage tzdata and the licenses into
# the bundle's Resources/; they are verified before macdeployqt rewrites the
# @rpaths.
#
# Environment: APP_NAME (default StructuredLogViewer).
set -euo pipefail

APP_NAME="${APP_NAME:-StructuredLogViewer}"
ROOT="$(pwd)"
BIN_DIR="$ROOT/build/release/bin/Release"
DMG_ROOT="$ROOT/dmg-root"

cd "$BIN_DIR"
if [ ! -d "$APP_NAME.app/Contents/Resources/tzdata" ]; then
    echo "ERROR: tzdata missing from bundle Resources" >&2
    exit 1
fi
if [ ! -f "$APP_NAME.app/Contents/Resources/THIRD_PARTY_LICENSES.txt" ]; then
    echo "ERROR: THIRD_PARTY_LICENSES.txt missing from bundle Resources" >&2
    exit 1
fi
macdeployqt "$APP_NAME.app"

rm -rf "$DMG_ROOT"
mkdir -p "$DMG_ROOT"
cp -R "$APP_NAME.app" "$DMG_ROOT/"
cp slp "$DMG_ROOT/slp"
cp "$APP_NAME.app/Contents/Resources/THIRD_PARTY_LICENSES.txt" "$DMG_ROOT/"
# slp finds its TBB dylib through the build tree's rpath, so stage that
# dylib beside it.
for dep in $(otool -L slp | awk '/@rpath\/libtbb/ {print $1}'); do
    lib="$(basename "$dep")"
    src="$(find "$ROOT/build/release" -name "$lib" -type f | head -n 1)"
    if [ -z "$src" ]; then
        echo "ERROR: could not locate $lib for slp" >&2
        exit 1
    fi
    cp "$src" "$DMG_ROOT/$lib"
done
install_name_tool -add_rpath @executable_path "$DMG_ROOT/slp" || true
hdiutil create -volname "$APP_NAME" -srcfolder "$DMG_ROOT" -ov -format UDZO "$ROOT/$APP_NAME.dmg"
