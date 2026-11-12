# Third-party components

The license texts for every component below are collected in `licenses.txt` next to this file.
The corresponding source for this release (Compareel itself and the exact Qt, FFmpeg and libheif sources,
SPDX bills of materials and build recipes) is published as `Compareel-<version>-source.zip` next to the
program package on the same release page. Paths such as `third_party/...` below refer to that archive.

Compareel uses dynamically linked, unmodified Qt 6.8.3 QtBase and QtSvg (Core, Gui,
Widgets, Concurrent, Sql, Network and selected runtime plugins, including the platform's own TLS backend for the
update check: Schannel on Windows, Secure Transport on macOS) under LGPL version 3.
Qt's copyright notices, license texts and third-party attribution files from
the matching official sources are in `third_party/qt-6.8.3/`.
QtSvg XSVG attribution/license is in its `qtsvg/` subdirectory.
The SDK's SPDX software bill of materials is also included in packaged builds.

Qt source for this exact version:
https://download.qt.io/archive/qt/6.8/6.8.3/submodules/qtbase-everywhere-src-6.8.3.tar.xz

QtSvg exact source: https://download.qt.io/archive/qt/6.8/6.8.3/submodules/qtsvg-everywhere-src-6.8.3.tar.xz

Qt Image Formats 6.8.3, dynamically linked and unmodified (including the GIF, ICNS, ICO, TGA, TIFF, WBMP, and WebP plugins), is also distributed under LGPLv3. Matching source and license notices (including bundled libtiff and libwebp 1.5.0 notices) are in `third_party/qt-6.8.3/qtimageformats/`; the SDK's SPDX JSON is included. Its bundled libwebp 1.5.0 is under the BSD 3-clause license, with `COPYING`, `PATENTS` and attribution in that source directory.

Exact source: https://download.qt.io/archive/qt/6.8/6.8.3/submodules/qtimageformats-everywhere-src-6.8.3.tar.xz
SHA256: 049bfb99845e4801672aca07c3c4fc4c140f932a3a33faa899419579e33ef1c8.
Qt 6.8.3 MSVC2022 Image Formats binary archive SHA256: 55f4d6aeb3270da0d3f88318e704aadf73dbd4c64b6dd17811cc5988062a5cd0.

Lucide SVG icons: https://github.com/lucide-icons/lucide
Copyright and ISC license: `third_party/lucide/LICENSE`. Original geometry is embedded; color follows the application theme.

Qt can be rebuilt and its DLLs replaced with compatible modified versions.
Compareel does not restrict modification, replacement, or reverse engineering
for debugging modifications to these LGPL libraries. Its build instructions
and source are supplied in this workspace. No Qt source modifications were made.

Qt license guidance: https://www.qt.io/development/open-source-lgpl-obligations
Qt deployment guidance: https://doc.qt.io/qt-6.8/windows-deployment.html

The Windows package includes Microsoft's x64 Visual C++ runtime DLLs from
the installed Visual Studio redistributable directory.
Windows Media Foundation, Windows Imaging Component (WIC / `windowscodecs.dll`), and other Windows system components are supplied by
Windows, and codec availability depends on the installed operating system.


FFmpeg 8.1 (source commit 92611900bb112123e9bf997fdcc2d78e4c4922fc, release/8.1), dynamically linked LGPLv3 shared libraries built by Compareel from the unmodified source archive in third_party/ffmpeg. The build is decode-only: no encoders, muxers, devices, network protocols or GPL components; only the file protocol and the filters the player uses are enabled. The exact configure options are in third_party/ffmpeg/BUILD_CONFIGURATION.txt and the build script is tools/ffmpeg-minimal/build.sh (run by tools/Build-FFmpegMinimal.ps1). Library replacement and reverse engineering for debugging modifications to these libraries are permitted.

Upstream: https://ffmpeg.org/ and https://github.com/FFmpeg/FFmpeg/tree/92611900bb112123e9bf997fdcc2d78e4c4922fc
Toolchain image (MinGW-w64 cross compiler and prebuilt external libraries): ghcr.io/btbn/ffmpeg-builds/win64-lgpl-8.1@sha256:3b58d55fe69e8ea35951dbacffc86f5e396b58cf530e205e7536e00f065cb872, recipes at https://github.com/BtbN/FFmpeg-Builds
Statically included in the FFmpeg libraries:
- dav1d 1.5.4 (AV1 decoding), BSD-2-Clause, https://code.videolan.org/videolan/dav1d
- zimg 3.0.6 (zscale filter for HDR tone mapping), WTFPL, https://github.com/sekrit-twc/zimg
- libjxl 0.13.0 (JPEG XL decoding), BSD-3-Clause, https://github.com/libjxl/libjxl; with Highway 1.2.0 (Apache-2.0 or BSD-3-Clause) and Brotli 1.2.0 (MIT)
- XZ Utils liblzma 5.8.4, 0BSD; zlib 1.3.2.1, zlib license
FFmpeg source also incorporates third-party code; its source archive contains the corresponding notices.
# libheif 1.23.7

HEIC/HEIF and AVIF container parsing and still-image conversion use libheif 1.23.7, dynamically linked under LGPL-3.0-or-later. Official source: https://github.com/strukturag/libheif/releases/tag/v1.23.7 . The matching source archive, complete upstream COPYING, and local build recipes are distributed under `third_party/libheif-1.23.7`. Source archive SHA-256: `f7483e818503ea6224c8863e4a56ac02c7089021a98472c811c12aa19faf2475`.

Build: shared library, existing pinned FFmpeg 8.1 decoder backend; external plugin loading, encoder plugins, examples and parallel tile decoding disabled. This does not install system codecs. To rebuild, run `tools/Build-FFmpegMinimal.ps1` and `tools/Setup-Heif.ps1` (or `Setup-FFmpeg.ps1` and `Setup-Heif.ps1`/`Build-Heif.cmd` in `third_party/libheif-1.23.7`). Rebuilt compatible heif.dll may replace the distributed DLL. The source archive is unmodified; configuration is in Build-Heif.cmd. FFmpeg's separate notices and corresponding source information also apply.

# miniaudio 0.11.22

Linux and macOS builds play audio through miniaudio 0.11.22 (https://github.com/mackron/miniaudio), compiled into the program from the unmodified single-file source `third_party/miniaudio/miniaudio.h`. It is available under your choice of public domain (Unlicense) or MIT No Attribution (MIT-0); the license statements are at the end of that file. Windows builds do not include miniaudio.

# macOS package

The macOS package contains the official Qt 6.8.3 macOS binaries (QtCore, QtGui, QtWidgets, QtConcurrent, QtSql,
QtSvg, QtDBus as a QtGui dependency, and the Cocoa, SQLite and image format plugins listed above), dynamically linked and unmodified, inside
`Compareel.app/Contents/Frameworks` and `Contents/PlugIns`; they can be replaced with compatible modified builds.
FFmpeg is built from the same source archive and with the same decode-only options, with VideoToolbox in place of
D3D11VA/DXVA2; libheif is built from the same archive with the same configuration. The recipe is
`tools/Setup-MacDeps.sh` and the packaging script `build-macos.sh`. Statically included in the FFmpeg libraries:
dav1d 1.5.4, zimg 3.0.6, libjxl 0.12.0 with Highway 1.2.0, Brotli 1.2.0 and skcms, and XZ Utils liblzma 5.8.4;
zlib is the one supplied by macOS. Their license texts are in `licenses.txt`. VideoToolbox, ImageIO and other
system frameworks are supplied by macOS. The package is signed ad hoc and is not notarized.

# Linux package (Flatpak)

The Flatpak runs on the KDE 6.8 runtime from Flathub, which provides Qt 6.8 (QtBase, QtSvg, QtDBus, QtNetwork and the
image format plugins) and the system libraries; the runtime carries its own license texts. FFmpeg and libheif are built
from the same source archives with the same decode-only options as above, without a hardware decoder. dav1d 1.5.4,
zimg 3.0.6 and libjxl 0.12.0 (with Highway, Brotli and skcms) are built from their release tags as shared libraries
inside the package. The recipe is `packaging/linux/io.github.turxlab.Compareel.yml` with `tools/build-flatpak.sh`. Compareel's
license, this file and the license texts of the components above are installed in `share/licenses/io.github.turxlab.Compareel`
inside the package. The Flatpak does not check for updates itself; Flathub updates it.
