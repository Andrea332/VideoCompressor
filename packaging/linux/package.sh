#!/usr/bin/env bash
# Creates <build>/VideoCompressor-<version>-linux-<x86_64|aarch64>.AppImage, for the processor of this
# machine, from a configured and built build folder, with linuxdeploy and its Qt plugin (downloaded into
# <build>/tools if missing).
#
# Usage:  QMAKE=/path/to/Qt/bin/qmake packaging/linux/package.sh [build]
set -euo pipefail

build=$(realpath "${1:-build}")
version=$(sed -n 's/^CMAKE_PROJECT_VERSION:STATIC=//p' "$build/CMakeCache.txt")
appdir="$build/AppDir"
tools="$build/tools"
arch=$(uname -m)                      # x86_64 or aarch64

rm -rf "$appdir"
cmake --install "$build" --prefix "$appdir/usr"

mkdir -p "$tools"
for tool in linuxdeploy linuxdeploy-plugin-qt; do
    if [ ! -x "$tools/$tool-$arch.AppImage" ]; then
        curl -fsSL -o "$tools/$tool-$arch.AppImage" \
            "https://github.com/linuxdeploy/$tool/releases/download/continuous/$tool-$arch.AppImage"
        chmod +x "$tools/$tool-$arch.AppImage"
    fi
done

export PATH="$tools:$PATH"            # linuxdeploy looks for its plugins in PATH
# the installed executable no longer points to Qt: tell linuxdeploy where Qt's libraries are
qt_libs=$("${QMAKE:?set QMAKE to the qmake of Qt}" -query QT_INSTALL_LIBS)
export LD_LIBRARY_PATH="$qt_libs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export APPIMAGE_EXTRACT_AND_RUN=1     # build machines often have no FUSE
export ARCH="$arch"                   # which AppImage runtime to embed
export LDAI_OUTPUT="$build/VideoCompressor-$version-linux-$arch.AppImage"
export OUTPUT="$LDAI_OUTPUT"
# native on Wayland desktops, not only through XWayland (the plugin names change between Qt versions)
qt_platforms="$("$QMAKE" -query QT_INSTALL_PLUGINS)/platforms"
wayland=$(cd "$qt_platforms" && ls libqwayland*.so 2> /dev/null | paste -sd ';' || true)
if [ -n "$wayland" ]; then
    export EXTRA_PLATFORM_PLUGINS="$wayland"
fi
echo "Wayland platform plugins: ${wayland:-none}"

"linuxdeploy-$arch.AppImage" --appdir "$appdir" \
    --executable "$appdir/usr/bin/VideoCompressor" \
    --desktop-file "$appdir/usr/share/applications/videocompressor.desktop" \
    --icon-file "$appdir/usr/share/icons/hicolor/256x256/apps/videocompressor.png" \
    --plugin qt --output appimage

echo "Created $LDAI_OUTPUT"
# the oldest Linux it runs on is given by the newest glibc that the app or its libraries need
glibc=$(find "$appdir" -type f \( -name '*.so*' -o -path '*/bin/*' \) -exec objdump -T {} + 2> /dev/null \
    | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -n 1 || true)
echo "Needs ${glibc:-an unknown glibc version}"
