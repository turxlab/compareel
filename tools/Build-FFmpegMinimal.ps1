# Builds the decode-only FFmpeg that Compareel ships, into .deps/ffmpeg-8.1-min.
# Needs Docker Desktop (Linux engine). The toolchain and its prebuilt libraries (dav1d, zimg, libjxl, zlib, lzma)
# come from BtbN's win64-lgpl-8.1 image (the recipe behind the full LGPL build); the FFmpeg source is the pinned tarball in third_party.
# The full SDK in .deps/ffmpeg-8.1 stays: its ffmpeg.exe encodes the fixtures used by the self-checks.
# Pinned by digest so a rebuild uses the same compiler and library versions as the shipped DLLs.
param([string]$Image = 'ghcr.io/btbn/ffmpeg-builds/win64-lgpl-8.1@sha256:cba7a58bfca46a17ca41400d462580c8e364dbf27da7b15c0eb1fb607054f74a')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$work = Join-Path $root '.deps/ffmpeg-min-build'
$destination = Join-Path $root '.deps/ffmpeg-8.1-min'
New-Item -ItemType Directory -Force -Path $work | Out-Null
$script = Join-Path $PSScriptRoot 'ffmpeg-minimal/build.sh'
$source = Join-Path $root 'third_party/ffmpeg'
docker run --rm -v "${work}:/ffbuild" -v "${source}:/source:ro" -v "${script}:/build.sh:ro" $Image bash /build.sh
if ($LASTEXITCODE -ne 0) { throw "FFmpeg build failed ($LASTEXITCODE)." }
if (Test-Path -LiteralPath $destination) { Remove-Item -LiteralPath $destination -Recurse -Force }
Copy-Item -LiteralPath (Join-Path $work 'out') -Destination $destination -Recurse
Get-ChildItem (Join-Path $destination 'bin') -Filter *.dll | ForEach-Object { '{0,-22} {1,6:N1} MB' -f $_.Name, ($_.Length / 1MB) }
