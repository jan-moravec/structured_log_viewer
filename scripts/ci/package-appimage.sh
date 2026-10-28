#!/usr/bin/env bash
# Builds the Linux AppImage (plus its .zsync delta file) from the `release`
# build tree with linuxdeploy and its Qt plugin.
#
# Environment: APP_NAME (default StructuredLogViewer), UPDATE_INFORMATION
# (embedded update source), EXTRA_PLATFORM_PLUGINS (read by the Qt plugin).
set -euo pipefail

APP_NAME="${APP_NAME:-StructuredLogViewer}"
BIN_DIR="build/release/bin/Release"
# Pinned tags (not `continuous`) so two runs of the same commit produce
# identical AppImages. Bump deliberately.
LINUXDEPLOY_TAG="1-alpha-20251107-1"
LINUXDEPLOY_PLUGIN_QT_TAG="1-alpha-20250213-1"

wget --quiet "https://github.com/linuxdeploy/linuxdeploy/releases/download/${LINUXDEPLOY_TAG}/linuxdeploy-x86_64.AppImage"
wget --quiet "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/${LINUXDEPLOY_PLUGIN_QT_TAG}/linuxdeploy-plugin-qt-x86_64.AppImage"
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
