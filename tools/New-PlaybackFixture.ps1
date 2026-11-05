param([Parameter(Mandatory)][string]$OutputDirectory,[string]$FFmpeg='C:/ffmpeg/bin/ffmpeg.exe')
$ErrorActionPreference='Stop'
if(!(Test-Path -LiteralPath $FFmpeg)) { $FFmpeg='C:/ffmpeg/bin/ffmpeg.exe' }
if(Test-Path -LiteralPath $OutputDirectory) { throw 'Output directory must be new.' }
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
& $FFmpeg -hide_banner -loglevel error -nostdin -n -f lavfi -i 'testsrc2=size=320x180:rate=30' -f lavfi -i 'anullsrc=r=48000:cl=stereo' -t 240 -c:v libx264 -preset ultrafast -crf 25 -c:a aac (Join-Path $OutputDirectory 'cfr.mp4')
if($LASTEXITCODE -ne 0) { throw 'CFR fixture failed.' }
& $FFmpeg -hide_banner -loglevel error -nostdin -n -i (Join-Path $OutputDirectory 'cfr.mp4') -vf 'select=if(lt(mod(t\,10)\,5)\,not(mod(n\,2))\,1)' -fps_mode vfr -c:v libx264 -preset ultrafast -crf 25 -c:a copy (Join-Path $OutputDirectory 'vfr.mp4')
if($LASTEXITCODE -ne 0) { throw 'VFR fixture failed.' }
& $FFmpeg -hide_banner -loglevel error -nostdin -n -f lavfi -i 'testsrc2=size=320x180:rate=24' -f lavfi -i 'anullsrc=r=48000:cl=stereo' -t 30 -c:v libx264 -preset ultrafast -crf 25 -c:a aac (Join-Path $OutputDirectory 'fps24.mp4')
if($LASTEXITCODE -ne 0) { throw '24fps fixture failed.' }
& $FFmpeg -hide_banner -loglevel error -nostdin -n -f lavfi -i 'testsrc2=size=320x180:rate=60' -f lavfi -i 'anullsrc=r=48000:cl=stereo' -t 30 -c:v libx264 -preset ultrafast -crf 25 -c:a aac (Join-Path $OutputDirectory 'fps60.mp4')
if($LASTEXITCODE -ne 0) { throw '60fps fixture failed.' }
Write-Output 'Generated 240-second CFR/VFR videos and 30-second 24fps/60fps videos with silent AAC tracks.'
