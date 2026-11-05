#!/bin/bash
# macOS counterpart of build.ps1. Without arguments it builds for development; with --package VERSION it
# makes release/vVERSION/ with Compareel.app, a .dmg and the corresponding source zip.
# The libraries come from tools/Setup-MacDeps.sh (run it once first).
# Usage: ./build-macos.sh [--package VERSION] [--arch universal|arm64|x86_64]
set -euo pipefail
ROOT=$(cd "$(dirname "$0")" && pwd)
VERSION=1.0.0 PACKAGE=0 ARCH=universal
while [ $# -gt 0 ]; do
    case "$1" in
        --package) PACKAGE=1; VERSION=$2; shift 2 ;;
        --arch) ARCH=$2; shift 2 ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
done
[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+([-.][A-Za-z0-9.-]+)?$ ]] || { echo "Invalid version." >&2; exit 2; }
DEPS="$ROOT/.deps/mac-$ARCH"
QT="$ROOT/.deps/Qt/6.8.3/macos"
[ -f "$DEPS/lib/libavcodec.dylib" ] && [ -d "$QT/lib/QtCore.framework" ] || { echo "Run tools/Setup-MacDeps.sh $ARCH first." >&2; exit 1; }
DESTINATION="$ROOT/release/v$VERSION"
[ "$PACKAGE" = 0 ] || [ ! -e "$DESTINATION" ] || { echo "Package already exists: $DESTINATION. Choose a new version; existing releases are preserved." >&2; exit 1; }
BUILD="$ROOT/.build-mac-$ARCH"
DISPLAY_VERSION=$([ "$PACKAGE" = 1 ] && echo "$VERSION" || echo "$VERSION-dev")

# Only the pinned Qt and the libraries from Setup-MacDeps.sh are visible, never Homebrew's.
PKG_CONFIG_LIBDIR="$DEPS/lib/pkgconfig" PKG_CONFIG_PATH="" cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$QT" -DCMAKE_IGNORE_PREFIX_PATH="/opt/homebrew;/usr/local" -DCMAKE_OSX_ARCHITECTURES="$([ "$ARCH" = universal ] && echo "arm64;x86_64" || echo "$ARCH")" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0 -DCOMPAREEL_DISPLAY_VERSION="$DISPLAY_VERSION" >/dev/null
cmake --build "$BUILD" --target Compareel
if [ "$PACKAGE" = 0 ]; then echo "Build ready: $BUILD/Compareel.app"; exit 0; fi

STAGING="$ROOT/.build/package-$(uuidgen | tr -d - | tr A-Z a-z)"
APP="$STAGING/Compareel.app"
mkdir -p "$STAGING"
cp -R "$BUILD/Compareel.app" "$APP"
# Plugins are chosen by hand, as in build.ps1: QSQLITE is loaded by name, and only the image formats Windows ships.
PLUGINS="$APP/Contents/PlugIns"
mkdir -p "$PLUGINS/platforms" "$PLUGINS/sqldrivers" "$PLUGINS/imageformats"
cp "$QT/plugins/platforms/libqcocoa.dylib" "$PLUGINS/platforms/"
cp "$QT/plugins/sqldrivers/libqsqlite.dylib" "$PLUGINS/sqldrivers/"
EXECUTABLES=(-executable="$PLUGINS/platforms/libqcocoa.dylib" -executable="$PLUGINS/sqldrivers/libqsqlite.dylib")
# The update check reaches GitHub over HTTPS through macOS's own TLS (Secure Transport); no OpenSSL is shipped.
mkdir -p "$PLUGINS/tls" && cp "$QT/plugins/tls/libqsecuretransportbackend.dylib" "$PLUGINS/tls/"
EXECUTABLES+=(-executable="$PLUGINS/tls/libqsecuretransportbackend.dylib")
for plugin in qgif qicns qico qjpeg qsvg qtga qtiff qwbmp qwebp; do
    cp "$QT/plugins/imageformats/lib$plugin.dylib" "$PLUGINS/imageformats/"
    EXECUTABLES+=(-executable="$PLUGINS/imageformats/lib$plugin.dylib")
done
# Copies the Qt frameworks and the FFmpeg/libheif dylibs into Contents/Frameworks and points everything at them.
"$QT/bin/macdeployqt" "$APP" -no-plugins -libpath="$DEPS/lib" "${EXECUTABLES[@]}" -verbose=1
cat > "$APP/Contents/Resources/qt.conf" <<'EOF'
[Paths]
Plugins = PlugIns
EOF
# Nothing in the bundle may still point at the build machine.
LEAKS=$(find "$APP" -type f \( -perm -u+x -o -name '*.dylib' \) -print0 | xargs -0 otool -L 2>/dev/null | grep -E '/opt/homebrew|/usr/local|\.deps/' || true)
[ -z "$LEAKS" ] || { echo "Bundle still links outside itself:"; echo "$LEAKS"; exit 1; }

# License texts travel inside the bundle; the source archive is published next to the package, as HandBrake does.
RESOURCES="$APP/Contents/Resources"
{
    first=1
    section() {
        [ "$first" = 1 ] || printf '\n\n\n'; first=0
        printf '%s\n%s\n%s\n\n' "$(printf '=%.0s' {1..78})" "$1" "$(printf '=%.0s' {1..78})"
        sed -e 's/[[:space:]]*$//' "$2"
    }
    for license in "$ROOT"/third_party/qt-6.8.3/LICENSES/*; do section "Qt 6.8.3 - $(basename "${license%.*}")" "$license"; done
    section "FFmpeg - LGPL-3.0" "$ROOT/third_party/ffmpeg/COPYING.LGPLv3"
    section "libheif 1.23.7 - LGPL-3.0" "$ROOT/third_party/libheif-1.23.7/COPYING"
    section "Lucide icons - ISC" "$ROOT/third_party/lucide/LICENSE"
    section "dav1d - BSD-2-Clause" "$DEPS/licenses/dav1d-COPYING"
    section "zimg - WTFPL" "$DEPS/licenses/zimg-COPYING"
    section "libjxl - BSD-3-Clause" "$DEPS/licenses/libjxl-LICENSE"
    section "Highway - Apache-2.0 or BSD-3-Clause" "$DEPS/licenses/Highway-LICENSE-BSD3"
    section "Brotli - MIT" "$DEPS/licenses/Brotli-LICENSE"
    section "skcms - BSD-3-Clause" "$DEPS/licenses/skcms-LICENSE"
    section "XZ Utils liblzma - 0BSD" "$DEPS/licenses/liblzma-COPYING.0BSD"
} > "$RESOURCES/licenses.txt"
cp "$ROOT/THIRD_PARTY_NOTICES.md" "$RESOURCES/"
cp "$ROOT/LICENSE" "$RESOURCES/LICENSE.txt"
cp "$ROOT/packaging/SUPPORTED_FORMATS.md" "$RESOURCES/"
# The shared package README, with macOS keys, Trash, and how to open an app that is not notarized.
sed -e "s/{{VERSION}}/$VERSION/" \
    -e '/^together with the DLLs and plugin folders/d' \
    -e 's/^Install it with `Compareel-.*$/Drag **Compareel** to **Applications**. Compareel is not notarized by Apple, so macOS stops it the first time: Control-click (or right-click) Compareel in Applications, choose **Open**, then **Open** again. If macOS offers no Open button, open **System Settings → Privacy \& Security** and click **Open Anyway** next to Compareel. This is needed only once./' \
    -e 's/Recycle Bin (Trash on Linux and macOS)/Trash/g' -e 's/Recycle Bin/Trash/g' \
    -e 's/Ctrl+Shift+/⇧⌘/g' -e 's/Ctrl+/⌘/g' -e 's/Alt+/⌥/g' \
    "$ROOT/packaging/PACKAGE_README.md" > "$RESOURCES/README.md"

# The official Qt binaries are universal; a single-architecture package keeps only its own half.
[ "$ARCH" = universal ] || find "$APP" -type f -print0 | while IFS= read -r -d '' file; do
    archs=$(lipo -archs "$file" 2>/dev/null || true)
    if [[ "$archs" == *" "* ]]; then lipo -thin "$ARCH" "$file" -output "$file"; fi
done

# Unsigned distribution: an ad-hoc signature is what Apple Silicon requires to run the code at all.
codesign --force --deep --sign - "$APP"
codesign --verify --deep --strict "$APP"

# Named as HandBrake names its own: the universal disk image carries just the version.
NAME="Compareel-$VERSION$([ "$ARCH" = universal ] || echo "-macOS-$ARCH")"
DMG_ROOT="$STAGING/dmg"
mkdir -p "$DMG_ROOT"
cp -R "$APP" "$DMG_ROOT/"
ln -s /Applications "$DMG_ROOT/Applications"
cp "$RESOURCES/README.md" "$DMG_ROOT/README.md"
hdiutil create -volname "Compareel $VERSION" -srcfolder "$DMG_ROOT" -fs HFS+ -format ULFO -ov "$STAGING/$NAME.dmg" >/dev/null
rm -rf "$DMG_ROOT"

# Corresponding source: every tracked or new (not ignored) file of the working tree, so it matches the
# build even before a commit, plus the Qt SBOMs of the deployed modules.
SOURCE_NAME="Compareel-$VERSION-source"
SOURCE_ROOT="$STAGING/$SOURCE_NAME"
(cd "$ROOT" && git -c core.quotepath=off ls-files -z --cached --others --exclude-standard) | while IFS= read -r -d '' file; do
    [ -f "$ROOT/$file" ] || continue # deleted but not yet committed
    mkdir -p "$SOURCE_ROOT/$(dirname "$file")" && cp "$ROOT/$file" "$SOURCE_ROOT/$file"
done
mkdir -p "$SOURCE_ROOT/third_party/qt-6.8.3/sbom"
for module in qtbase qtsvg qtimageformats; do cp "$QT/sbom/$module-6.8.3.spdx.json" "$SOURCE_ROOT/third_party/qt-6.8.3/sbom/"; done
(cd "$STAGING" && ditto -c -k --keepParent --norsrc "$SOURCE_NAME" "$SOURCE_NAME.zip")
rm -rf "$SOURCE_ROOT"

case "$STAGING" in "$ROOT/.build/package-"*) ;; *) echo "Invalid staging path." >&2; exit 1 ;; esac
mkdir -p "$ROOT/release"
mv "$STAGING" "$DESTINATION"
ls -l "$DESTINATION"
