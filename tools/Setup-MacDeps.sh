#!/bin/bash
# Builds the libraries the macOS package ships, the same way the Windows SDKs are made:
# a decode-only FFmpeg from third_party/ffmpeg with dav1d, zimg, libjxl and liblzma linked in statically,
# and libheif from third_party/libheif-1.23.7 on top of that FFmpeg. Everything targets macOS 12 so the
# package runs on older systems than the build machine; Homebrew's libraries only run on the host's macOS.
# Also fetches the official Qt 6.8.3 binaries (the version whose source is in third_party/qt-6.8.3).
# "universal" builds both and merges them into one set of arm64 + x86_64 libraries for the universal package.
# Needs Xcode command line tools, cmake, ninja, meson, pkg-config, autoconf, automake, libtool, nasm and python3.
# Usage: tools/Setup-MacDeps.sh [universal|arm64|x86_64]
set -euo pipefail
ARCH=${1:-universal}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEPS="$ROOT/.deps"
if [ "$ARCH" = universal ]; then
    "$0" arm64 && "$0" x86_64
    # One library per name with both architectures; headers and pkg-config files are the same for both.
    ARM="$DEPS/mac-arm64" INTEL="$DEPS/mac-x86_64" PREFIX="$DEPS/mac-universal"
    rm -rf "$PREFIX" && mkdir -p "$PREFIX/lib/pkgconfig"
    cp -R "$ARM/include" "$PREFIX/"
    for library in "$ARM"/lib/*.dylib; do
        name=$(basename "$library")
        if [ -L "$library" ]; then cp -P "$library" "$PREFIX/lib/"; else lipo -create "$library" "$INTEL/lib/$name" -output "$PREFIX/lib/$name"; fi
    done
    for file in "$ARM"/lib/pkgconfig/*.pc; do sed "s|$ARM|$PREFIX|g" "$file" > "$PREFIX/lib/pkgconfig/$(basename "$file")"; done
    cp "$ARM/FFMPEG_BUILD_CONFIGURATION.txt" "$PREFIX/"
    cp -R "$ARM/licenses" "$PREFIX/"
    echo "macOS libraries ready: $PREFIX"
    exit 0
fi
PREFIX="$DEPS/mac-$ARCH"
WORK="$DEPS/mac-$ARCH-build"
export MACOSX_DEPLOYMENT_TARGET=12.0
JOBS=$(sysctl -n hw.ncpu)
FLAGS="-arch $ARCH -mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET"
export CFLAGS="$FLAGS -O2" CXXFLAGS="$FLAGS -O2" LDFLAGS="$FLAGS"
# Only the libraries built here are visible to pkg-config, never Homebrew's.
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig" PKG_CONFIG_PATH=""
CMAKE_COMMON=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_PREFIX_PATH="$PREFIX"
    -DCMAKE_OSX_ARCHITECTURES="$ARCH" -DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOSX_DEPLOYMENT_TARGET" -DBUILD_SHARED_LIBS=OFF
    -DCMAKE_IGNORE_PREFIX_PATH="/opt/homebrew;/usr/local")
mkdir -p "$PREFIX" "$WORK"

clone() { # url tag dir
    [ -d "$WORK/$3" ] || git -c advice.detachedHead=false clone --quiet --depth 1 --recurse-submodules --shallow-submodules --branch "$2" "$1" "$WORK/$3"
}
fetch() { # url sha256 file
    local file="$WORK/$3"
    [ -f "$file" ] || curl -fsSL "$1" -o "$file"
    echo "$2  $file" | shasum -a 256 -c --quiet
}

if [ ! -f "$PREFIX/lib/liblzma.a" ]; then
    fetch https://github.com/tukaani-project/xz/releases/download/v5.8.4/xz-5.8.4.tar.gz 0014c7886930454fe8bd4228665b51af55eeae560ea135c9c4cd33f55b2591d9 xz-5.8.4.tar.gz
    rm -rf "$WORK/xz-5.8.4" && tar xzf "$WORK/xz-5.8.4.tar.gz" -C "$WORK"
    (cd "$WORK/xz-5.8.4" && ./configure --prefix="$PREFIX" --host="$ARCH-apple-darwin" --disable-shared --enable-static \
        --disable-xz --disable-xzdec --disable-lzmadec --disable-lzmainfo --disable-scripts --disable-doc --disable-nls >/dev/null \
        && make -j"$JOBS" >/dev/null && make install >/dev/null)
fi

if [ ! -f "$PREFIX/lib/libdav1d.a" ]; then
    clone https://code.videolan.org/videolan/dav1d.git 1.5.4 dav1d
    cat > "$WORK/dav1d-cross.txt" <<EOF
[binaries]
c = 'clang'
[built-in options]
c_args = ['-arch', '$ARCH', '-mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET']
c_link_args = ['-arch', '$ARCH', '-mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET']
[host_machine]
system = 'darwin'
cpu_family = '$([ "$ARCH" = arm64 ] && echo aarch64 || echo x86_64)'
cpu = '$ARCH'
endian = 'little'
EOF
    rm -rf "$WORK/dav1d/build"
    meson setup "$WORK/dav1d/build" "$WORK/dav1d" --cross-file "$WORK/dav1d-cross.txt" --prefix="$PREFIX" --libdir=lib \
        --buildtype=release --default-library=static -Denable_tools=false -Denable_tests=false -Denable_examples=false >/dev/null
    ninja -C "$WORK/dav1d/build" install >/dev/null
fi

if [ ! -f "$PREFIX/lib/libzimg.a" ]; then
    clone https://github.com/sekrit-twc/zimg.git release-3.0.6 zimg
    (cd "$WORK/zimg" && ./autogen.sh >/dev/null 2>&1 && ./configure --prefix="$PREFIX" --host="$ARCH-apple-darwin" --disable-shared --enable-static >/dev/null \
        && make -j"$JOBS" >/dev/null && make install >/dev/null)
fi

if [ ! -f "$PREFIX/lib/libjxl.a" ]; then
    # Highway, Brotli and skcms come from the submodules pinned by this libjxl release.
    clone https://github.com/libjxl/libjxl.git v0.12.0 libjxl
    cmake -S "$WORK/libjxl" -B "$WORK/libjxl/build" "${CMAKE_COMMON[@]}" -DBUILD_TESTING=OFF -DJPEGXL_ENABLE_TOOLS=OFF \
        -DJPEGXL_ENABLE_JPEGLI=OFF -DJPEGXL_ENABLE_DOXYGEN=OFF -DJPEGXL_ENABLE_MANPAGES=OFF -DJPEGXL_ENABLE_BENCHMARK=OFF \
        -DJPEGXL_ENABLE_EXAMPLES=OFF -DJPEGXL_ENABLE_SJPEG=OFF -DJPEGXL_ENABLE_OPENEXR=OFF -DJPEGXL_ENABLE_JNI=OFF \
        -DJPEGXL_ENABLE_SKCMS=ON -DJPEGXL_BUNDLE_LIBPNG=OFF -DJPEGXL_ENABLE_TRANSCODE_JPEG=OFF -DJPEGXL_STATIC=OFF >/dev/null
    cmake --build "$WORK/libjxl/build" --parallel "$JOBS" >/dev/null
    cmake --install "$WORK/libjxl/build" >/dev/null
fi

if [ ! -f "$PREFIX/lib/libavcodec.dylib" ]; then
    # Same options as tools/ffmpeg-minimal/build.sh, with VideoToolbox in place of D3D11VA/DXVA2.
    CONFIGURE=(
        --enable-version3 --disable-debug --enable-shared --disable-static --enable-pthreads
        --disable-autodetect --enable-zlib --enable-lzma --enable-videotoolbox
        --enable-libdav1d --enable-libzimg --enable-libjxl
        --disable-programs --disable-doc --disable-avdevice --disable-network
        --disable-encoders --disable-muxers --disable-devices
        --disable-protocols --enable-protocol=file
        --disable-filters --enable-filter=buffer,buffersink,null,format,scale,zscale,tonemap,transpose,hflip,vflip
    )
    rm -rf "$WORK/ffmpeg" && mkdir -p "$WORK/ffmpeg" && tar xzf "$ROOT/third_party/ffmpeg/FFmpeg-92611900bb.tar.gz" -C "$WORK/ffmpeg" --strip-components 1
    (cd "$WORK/ffmpeg" && ./configure --prefix="$PREFIX" --arch="$ARCH" --target-os=darwin --enable-cross-compile \
        --extra-cflags="$FLAGS -I$PREFIX/include" --extra-ldflags="$FLAGS -L$PREFIX/lib" --extra-cxxflags="$FLAGS" \
        --extra-libs=-lc++ --pkg-config-flags=--static --install-name-dir='@rpath' --extra-version=compareel-min "${CONFIGURE[@]}" >/dev/null \
        || { tail -40 ffbuild/config.log; exit 1; })
    make -C "$WORK/ffmpeg" -j"$JOBS" >/dev/null
    make -C "$WORK/ffmpeg" install >/dev/null
    printf '%s\n' "${CONFIGURE[@]}" > "$PREFIX/FFMPEG_BUILD_CONFIGURATION.txt"
fi

if [ ! -f "$PREFIX/lib/libheif.dylib" ]; then
    # Same configuration as tools/Build-Heif.cmd: HEVC and AV1 decode through the FFmpeg above, no plugin loading.
    rm -rf "$WORK/libheif" && mkdir -p "$WORK/libheif" && tar xzf "$ROOT/third_party/libheif-1.23.7/libheif-1.23.7.tar.gz" -C "$WORK/libheif" --strip-components 1
    cmake -S "$WORK/libheif" -B "$WORK/libheif/build" "${CMAKE_COMMON[@]}" -DBUILD_SHARED_LIBS=ON -DCMAKE_INSTALL_NAME_DIR='@rpath' \
        -DWITH_FFMPEG_DECODER=ON -DWITH_FFMPEG_DECODER_PLUGIN=OFF -DENABLE_PLUGIN_LOADING=OFF -DWITH_LIBDE265=OFF -DWITH_X265=OFF \
        -DWITH_X264=OFF -DWITH_OpenH264_DECODER=OFF -DWITH_AOM_DECODER=OFF -DWITH_AOM_ENCODER=OFF -DWITH_DAV1D=OFF -DWITH_KVAZAAR=OFF \
        -DWITH_JPEG_DECODER=OFF -DWITH_JPEG_ENCODER=OFF -DWITH_OpenJPEG_DECODER=OFF -DWITH_OpenJPEG_ENCODER=OFF \
        -DWITH_LIBSHARPYUV=OFF -DWITH_LIBSHARPYUV_INTERNAL=OFF -DWITH_EXAMPLES=OFF -DWITH_GDK_PIXBUF=OFF -DBUILD_TESTING=OFF \
        -DBUILD_DOCUMENTATION=OFF -DENABLE_PARALLEL_TILE_DECODING=OFF >/dev/null
    cmake --build "$WORK/libheif/build" --target heif --parallel "$JOBS" >/dev/null
    cmake --install "$WORK/libheif/build" >/dev/null
fi

QT="$DEPS/Qt/6.8.3/macos"
if [ ! -d "$QT/lib/QtCore.framework" ]; then
    [ -x "$DEPS/venv/bin/aqt" ] || { python3 -m venv "$DEPS/venv" && "$DEPS/venv/bin/pip" -q install aqtinstall; }
    "$DEPS/venv/bin/aqt" install-qt mac desktop 6.8.3 clang_64 -m qtimageformats -O "$DEPS/Qt"
fi
# The license texts of the statically linked libraries stay with the libraries, so a cached set (GitHub Actions)
# can be packaged without the source checkouts in $WORK.
mkdir -p "$PREFIX/licenses"
cp "$WORK/dav1d/COPYING" "$PREFIX/licenses/dav1d-COPYING"
cp "$WORK/zimg/COPYING" "$PREFIX/licenses/zimg-COPYING"
cp "$WORK/libjxl/LICENSE" "$PREFIX/licenses/libjxl-LICENSE"
cp "$WORK/libjxl/third_party/highway/LICENSE-BSD3" "$PREFIX/licenses/Highway-LICENSE-BSD3"
cp "$WORK/libjxl/third_party/brotli/LICENSE" "$PREFIX/licenses/Brotli-LICENSE"
cp "$WORK/libjxl/third_party/skcms/LICENSE" "$PREFIX/licenses/skcms-LICENSE"
cp "$WORK/xz-5.8.4/COPYING.0BSD" "$PREFIX/licenses/liblzma-COPYING.0BSD"
echo "macOS libraries ready: $PREFIX"
