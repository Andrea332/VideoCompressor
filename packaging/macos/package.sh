#!/usr/bin/env bash
# Creates <build>/VideoCompressor-<version>-macos-arm64.dmg from a configured and built build folder:
# the app bundle with the Qt frameworks (macdeployqt), an ad-hoc signature and a disk image to drag it
# into Applications.
#
# Usage:  packaging/macos/package.sh [build]      (Qt's bin folder, with macdeployqt, must be in PATH)
set -euo pipefail

build=$(cd "${1:-build}" && pwd)
version=$(sed -n 's/^CMAKE_PROJECT_VERSION:STATIC=//p' "$build/CMakeCache.txt")
stage="$build/dmg"
app="$stage/VideoCompressor.app"
dmg="$build/VideoCompressor-$version-macos-arm64.dmg"

rm -rf "$stage"
cmake --install "$build" --prefix "$stage"

# Apple Silicon runs only signed code: without a Developer ID the app gets an ad-hoc signature,
# so macOS asks the user to allow it the first time (Privacy & Security > Open anyway)
macdeployqt "$app" -always-overwrite -codesign=-
codesign --force --sign - "$app"
codesign --verify --deep --strict --verbose=2 "$app"

ln -s /Applications "$stage/Applications"
rm -f "$dmg"
hdiutil create -volname "Video Compressor" -srcfolder "$stage" -fs HFS+ -format UDZO -ov "$dmg"

echo "Created $dmg"
