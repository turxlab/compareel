$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$destination = Join-Path $root '.deps/ffmpeg-8.1'
if (Test-Path -LiteralPath (Join-Path $destination 'include/libavformat/avformat.h')) { Write-Output 'Pinned FFmpeg SDK already exists.'; return }
$name = 'ffmpeg-n8.1.3-24-g92611900bb-win64-lgpl-shared-8.1'
$url = "https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-10-24-13-02/$name.zip"
$expected = '0c2e70f180ae48a748acf2308229ae19eafc734515e9876c75cfe36998981c53'
$downloads = Join-Path $root '.deps/ffmpeg-download'
New-Item -ItemType Directory -Force -Path $downloads | Out-Null
$archive = Join-Path $downloads "$name.zip"
if (!(Test-Path -LiteralPath $archive)) { Invoke-WebRequest -Uri $url -OutFile $archive }
if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $expected) { throw 'FFmpeg archive hash mismatch; SDK not installed.' }
$staging = Join-Path $downloads ('extract-' + [guid]::NewGuid().ToString('N'))
Expand-Archive -LiteralPath $archive -DestinationPath $staging
Copy-Item -LiteralPath (Join-Path $staging $name) -Destination $destination -Recurse
Write-Output 'Pinned FFmpeg 8.1 LGPL SDK installed inside this workspace; system codecs unchanged.'
