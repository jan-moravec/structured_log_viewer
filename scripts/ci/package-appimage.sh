#!/usr/bin/env bash
# Builds the Linux AppImage and its .zsync update file from the `release` build.
#
# Environment: APP_NAME (default StructuredLogViewer). EXTRA_PLATFORM_PLUGINS is read by the linuxdeploy Qt plugin.
set -euo pipefail

APP_NAME="${APP_NAME:-StructuredLogViewer}"
# linuxdeploy embeds the update source and writes the .zsync file next to the AppImage.
if [ -z "${UPDATE_INFORMATION:-}" ] && [ -n "${GITHUB_REPOSITORY:-}" ]; then
    REPO_OWNER="${GITHUB_REPOSITORY%%/*}"
    REPO_NAME="${GITHUB_REPOSITORY#*/}"
    export UPDATE_INFORMATION="gh-releases-zsync|$REPO_OWNER|$REPO_NAME|latest|$APP_NAME-*x86_64.AppImage.zsync"
fi
BIN_DIR="build/release/bin/Release"
# Pinned versions, so the same commit always produces the same AppImage.
LINUXDEPLOY_TAG="1-alpha-20251107-1"
LINUXDEPLOY_PLUGIN_QT_TAG="1-alpha-20250213-1"

RELEASES="https://github.com/linuxdeploy"
wget --quiet "$RELEASES/linuxdeploy/releases/download/$LINUXDEPLOY_TAG/linuxdeploy-x86_64.AppImage"
QT_PLUGIN="linuxdeploy-plugin-qt-x86_64.AppImage"
wget --quiet "$RELEASES/linuxdeploy-plugin-qt/releases/download/$LINUXDEPLOY_PLUGIN_QT_TAG/$QT_PLUGIN"
chmod +x linuxdeploy-x86_64.AppImage linuxdeploy-plugin-qt-x86_64.AppImage

APP_DIR="$APP_NAME.AppDir"
mkdir -p "$APP_DIR/usr/bin" \
    "$APP_DIR/usr/lib" \
    "$APP_DIR/usr/share/applications" \
    "$APP_DIR/usr/share/icons/hicolor/256x256/apps" \
    "$APP_DIR/usr/share/doc/$APP_NAME"
cp "$BIN_DIR/$APP_NAME" "$APP_DIR/usr/bin/"
cp -r "$BIN_DIR/tzdata" "$APP_DIR/usr/share/"
cp resources/icon-white.png "$APP_DIR/usr/share/icons/hicolor/256x256/apps/$APP_NAME.png"
cp resources/app.desktop "$APP_DIR/usr/share/applications/$APP_NAME.desktop"
cp "$BIN_DIR/THIRD_PARTY_LICENSES.txt" "$BIN_DIR/LICENSE" "$APP_DIR/usr/share/doc/$APP_NAME/"

./linuxdeploy-x86_64.AppImage --appdir="$APP_DIR" --plugin qt --output appimage
