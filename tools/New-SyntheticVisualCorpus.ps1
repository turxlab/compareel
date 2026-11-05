param(
    [Parameter(Mandatory)][string]$OutputDirectory,
    [string]$FFmpeg = 'C:/ffmpeg/bin/ffmpeg.exe'
)
$ErrorActionPreference = 'Stop'
if (!(Test-Path -LiteralPath $FFmpeg)) { $FFmpeg = 'C:/ffmpeg/bin/ffmpeg.exe' }
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Use a new output directory; existing files will not be overwritten.' }
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
$pairs = [System.Collections.Generic.List[string]]::new()
function Encode([string]$Name, [string[]]$Inputs, [string]$Filter = 'null', [double]$Duration = 6.0, [string[]]$AudioArgs = @('-an')) {
    & $FFmpeg -hide_banner -loglevel error -nostdin -n @Inputs -t $Duration @AudioArgs -vf "$Filter,setsar=1" -r 12 -c:v libx264 -threads 2 -preset fast -crf 23 -pix_fmt yuv420p (Join-Path $OutputDirectory $Name)
    if ($LASTEXITCODE -ne 0) { throw "FFmpeg failed for $Name" }
}
$sources = @(
    'testsrc2=size=320x180:rate=12',
    'mandelbrot=size=320x180:rate=12',
    'life=size=40x22:rate=12:random_seed=17:ratio=0.3:death_color=black:life_color=white,scale=320:180:flags=neighbor',
    'nullsrc=size=320x180:rate=12,geq=lum=128+80*sin(X/13+T)*cos(Y/11-T):cb=90:cr=180',
    'color=c=navy:size=320x180:rate=12,drawgrid=width=37:height=29:thickness=4:color=yellow,rotate=0.3*t:fillcolor=navy',
    'smptehdbars=size=320x180:rate=12'
)
for ($i=0; $i -lt $sources.Count; ++$i) {
    $name="scene-$i"
    Encode "$name.mp4" @('-f','lavfi','-i',$sources[$i])
    foreach ($variant in @(
        @('encode','scale=256:144','same_content_diff_encode'),
        @('letterbox','pad=iw+40:ih+40:20:20:black','letterbox_bars'),
        @('pillarbox','pad=iw+64:ih:32:0:black','pillarbox_bars'),
        @('mirror','hflip','mirror_flip'),
        @('rotate','transpose=1','rotate_90'),
        @('crop','crop=iw-24:ih-14:12:7','mild_crop'),
        @('subtitle','drawbox=x=48:y=ih-20:w=iw-96:h=12:color=white@0.85:t=fill','subtitle_overlay')
    )) {
        Encode "$name-$($variant[0]).mp4" @('-i',(Join-Path $OutputDirectory "$name.mp4")) $variant[1]
        $pairs.Add("$name.mp4`t$name-$($variant[0]).mp4`t1`t0`t$($variant[2])")
    }
    # Different audio track on identical visual content
    Encode "$name-audio.mp4" @('-i',(Join-Path $OutputDirectory "$name.mp4"),'-f','lavfi','-i',"sine=frequency=$(440 + $i*110):sample_rate=48000") 'null' 6.0 @('-c:a','aac','-shortest')
    $pairs.Add("$name.mp4`t$name-audio.mp4`t1`t0`tdifferent_audio")

    # Duration difference (8.5s vs 6.0s exceeds full-clip tolerance)
    Encode "$name-long.mp4" @('-f','lavfi','-i',$sources[$i]) 'null' 8.5
    $pairs.Add("$name.mp4`t$name-long.mp4`t0`t0`tduration_mismatch")

    # Exact copies check the same content under different physical file identities.
    Copy-Item -LiteralPath (Join-Path $OutputDirectory "$name.mp4") -Destination (Join-Path $OutputDirectory "$name-copy.mp4")
    $pairs.Add("$name.mp4`t$name-copy.mp4`t1`t1`texact_copy")
    for ($j=0; $j -lt $i; ++$j) { $pairs.Add("scene-$j.mp4`t$name.mp4`t0`t0`tdifferent_content") }
}
# Embedded mov_text subtitle stream on scene-0
$srtPath = Join-Path $OutputDirectory 'sub.srt'
"1`r`n00:00:00,500 --> 00:00:05,500`r`nCompareel synthetic subtitle check`r`n" | Set-Content -LiteralPath $srtPath -Encoding utf8
& $FFmpeg -hide_banner -loglevel error -nostdin -n -i (Join-Path $OutputDirectory 'scene-0.mp4') -i $srtPath -c:v copy -c:s mov_text (Join-Path $OutputDirectory 'scene-0-softsub.mp4')
if ($LASTEXITCODE -ne 0) { throw 'FFmpeg softsub generation failed' }
Remove-Item -LiteralPath $srtPath -Force
$pairs.Add("scene-0.mp4`tscene-0-softsub.mp4`t1`t0`tembedded_subtitle")

# Solid black frame video (low variance; must never match any scene or itself visually)
Encode 'black.mp4' @('-f','lavfi','-i','color=c=black:size=320x180:rate=12')
for ($i=0; $i -lt $sources.Count; ++$i) { $pairs.Add("scene-$i.mp4`tblack.mp4`t0`t0`tblack_frame") }
$pairs.Add("black.mp4`tblack.mp4`t0`t1`tblack_self")

# Short clip (0.16s = 2 frames at 12fps; < 3 distinct valid samples so must be rejected for visual match)
Encode 'short.mp4' @('-f','lavfi','-i',$sources[0]) 'null' 0.16
$pairs.Add("scene-0.mp4`tshort.mp4`t0`t0`tshort_clip")
$pairs.Add("short.mp4`tshort.mp4`t0`t1`tshort_clip_self")

# Common intro: both clips start with the same 1s intro, followed by 5s of independent content.
foreach ($i in @(1,2,3)) {
    $filter='[0:v]trim=duration=1,setpts=PTS-STARTPTS[a];[1:v]trim=duration=5,setpts=PTS-STARTPTS[b];[a][b]concat=n=2:v=1:a=0[out]'
    & $FFmpeg -hide_banner -loglevel error -nostdin -n -i (Join-Path $OutputDirectory 'scene-0.mp4') -i (Join-Path $OutputDirectory "scene-$i.mp4") -filter_complex $filter -map '[out]' -an -c:v libx264 -threads 2 -preset fast -crf 23 -pix_fmt yuv420p (Join-Path $OutputDirectory "intro-$i.mp4")
    if ($LASTEXITCODE -ne 0) { throw 'FFmpeg common-intro generation failed' }
}
foreach ($pair in @(@(1,2),@(1,3),@(2,3))) { $pairs.Add("intro-$($pair[0]).mp4`tintro-$($pair[1]).mp4`t0`t0`tcommon_intro") }
$pairs | Set-Content -LiteralPath (Join-Path $OutputDirectory 'pairs.tsv') -Encoding utf8
Write-Output "Generated $($sources.Count) independent synthetic scenes and $($pairs.Count) labelled pairs across all 10 required categories. No user media read."
