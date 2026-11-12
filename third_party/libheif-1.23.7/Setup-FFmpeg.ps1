$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$destination = Join-Path $root '.deps/ffmpeg-8.1-min'
if (Test-Path -LiteralPath (Join-Path $destination 'include/libavformat/avformat.h')) { Write-Output 'Pinned minimal FFmpeg SDK already exists.'; return }
& (Join-Path $root 'tools/Build-FFmpegMinimal.ps1')
if ($LASTEXITCODE -ne 0) { throw "FFmpeg build failed ($LASTEXITCODE)." }
