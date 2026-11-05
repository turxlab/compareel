#!/bin/bash
# Builds a decode-only FFmpeg for Compareel inside the BtbN win64-lgpl-8.1 toolchain image.
# Compareel only reads media: it never encodes, muxes, opens devices or networks, so all of that is left out.
# Kept: every native decoder, demuxer, parser and bitstream filter (so any file that opened before still opens),
# AV1 through dav1d, JPEG XL through libjxl, and the few filters the player uses (scale, zscale, tonemap, rotation).
set -xe
SOURCE=/source/FFmpeg-92611900bb.tar.gz
rm -rf /ffbuild/src /ffbuild/prefix && mkdir -p /ffbuild/src && cd /ffbuild/src
tar xzf "$SOURCE" && cd FFmpeg-*

CONFIGURE=(
    --enable-version3 --disable-debug --enable-shared --disable-static
    --disable-w32threads --enable-pthreads
    # Nothing is picked up implicitly from the toolchain image; each external piece is named below.
    --disable-autodetect --enable-zlib --enable-lzma --enable-d3d11va --enable-dxva2
    --enable-libdav1d --enable-libzimg --enable-libjxl
    --disable-programs --disable-doc --disable-avdevice --disable-network
    --disable-encoders --disable-muxers --disable-devices
    --disable-protocols --enable-protocol=file
    --disable-filters --enable-filter=buffer,buffersink,null,format,scale,zscale,tonemap,transpose,hflip,vflip
)
./configure --prefix=/ffbuild/prefix --pkg-config-flags="--static" $FFBUILD_TARGET_FLAGS "${CONFIGURE[@]}" \
    --extra-cflags="$FF_CFLAGS" --extra-cxxflags="$FF_CXXFLAGS" --extra-libs="$FF_LIBS" \
    --extra-ldflags="$FF_LDFLAGS" --extra-ldexeflags="$FF_LDEXEFLAGS" \
    --cc="$CC" --cxx="$CXX" --ar="$AR" --ranlib="$RANLIB" --nm="$NM" \
    --extra-version="compareel-min" || { tail -50 ffbuild/config.log; exit 1; }
make -j"$(nproc)"
make install

# Same SDK layout as the BtbN packages (bin, lib with MSVC import libraries, include).
OUT=/ffbuild/out; rm -rf "$OUT"; mkdir -p "$OUT"/bin "$OUT"/lib "$OUT"/include
cp /ffbuild/prefix/bin/*.dll "$OUT"/bin
cp /ffbuild/prefix/bin/*.lib "$OUT"/lib
cp /ffbuild/prefix/lib/*.def /ffbuild/prefix/lib/*.dll.a "$OUT"/lib
cp -r /ffbuild/prefix/include/* "$OUT"/include
cp COPYING.LGPLv3 "$OUT"/LICENSE.txt
printf '%s\n' "${CONFIGURE[@]}" > "$OUT"/BUILD_CONFIGURATION.txt
