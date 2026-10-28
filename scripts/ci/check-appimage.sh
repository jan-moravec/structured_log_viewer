#!/usr/bin/env bash
# Verifies the AppImage produced by package-appimage.sh: it must launch
# headless, and its payload must contain the binary, Qt libraries, the xcb
# platform plugin, tzdata, and the license file, but not `slp`.
#
# Environment: APP_NAME (default StructuredLogViewer).
set -euo pipefail
shopt -s nullglob

APP_NAME="${APP_NAME:-StructuredLogViewer}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

APPIMAGES=("$APP_NAME"-*.AppImage)
if [ "${#APPIMAGES[@]}" -eq 0 ]; then
    echo "No AppImage produced by linuxdeploy" >&2
    exit 1
fi
APPIMAGE="${APPIMAGES[0]}"
chmod +x "$APPIMAGE"

"$SCRIPT_DIR/smoke-launch.sh" "./$APPIMAGE"

# Structural check independent of Qt's launch path.
rm -rf squashfs-root
"./$APPIMAGE" --appimage-extract >/dev/null
test -x "squashfs-root/usr/bin/$APP_NAME"
# slp ships in its own tarball; the AppImage only launches the GUI.
test ! -e "squashfs-root/usr/bin/slp"
test -f "squashfs-root/usr/share/doc/$APP_NAME/THIRD_PARTY_LICENSES.txt"
test -f "squashfs-root/usr/lib/libQt6Core.so.6"
test -f "squashfs-root/usr/lib/libQt6Widgets.so.6"
test -d "squashfs-root/usr/share/tzdata"
test -f "squashfs-root/usr/plugins/platforms/libqxcb.so"
echo "AppImage structural sanity check passed."
