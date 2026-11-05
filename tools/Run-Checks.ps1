# Runs every Compareel self-check against a local build and prints one line per check (exit code 0 = pass).
#
# Fixtures are not in the repository; by default they are read from .deps\test-fixtures. Expected layout:
#   portrait.mp4          a 4-second 1080x1920 clip (ui-check, appearance-check, CoreChecks)
#   media\plain.mp4       any short H.264 clip (generic UI checks)
#   media\subtitled.mp4   a clip with an embedded subtitle track (subtitle-check)
#   media\short.mp4       a ~0.2 s clip (CoreChecks --short)
#   matching\main12.mp4    a 12 fps clip, next to encode.mp4 (a re-encode) and different.mp4 (other content) (matching-check)
#   playback\cfr.mp4      output of tools\New-PlaybackFixture.ps1 (playback-check)
#   heifdata\             libheif-x.y.z\tests\data extracted from third_party\libheif-*\libheif-*.tar.gz (HEIF checks)
# Harness programs are built on demand (they are EXCLUDE_FROM_ALL targets).
# Checks that need the user's own files (image-false-positive, archive-acceptance) are not run here.
param(
    [string]$Fixtures = (Join-Path (Split-Path -Parent $PSScriptRoot) '.deps\test-fixtures'),
    [string]$BuildDirectory = '.build-ui',
    [string]$OutDir = (Join-Path $env:TEMP 'compareel-checks'),
    [string[]]$Only,
    [switch]$SkipHarnesses
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root $BuildDirectory
$exe = Join-Path $build 'Compareel.exe'
$ffmpeg = Join-Path $root '.deps\ffmpeg-8.1\bin\ffmpeg.exe'
$Fixtures = (Resolve-Path -LiteralPath $Fixtures).Path
$portrait = Join-Path $Fixtures 'portrait.mp4'
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$env:QT_FORCE_STDERR_LOGGING = '1'
$env:COMPAREEL_CHECK_SHOTS = $OutDir
$Only = @($Only | ForEach-Object { $_ -split ',' } | Where-Object { $_ })

# Runs one process with a timeout so a stuck check never leaves a window behind. Standard output is not
# redirected: with it, media checks fail at "private copies".
function Invoke-Check([string]$Name, [string]$File, [string[]]$Arguments) {
    if ($Only -and $Only -notcontains $Name) { return }
    $quoted = $Arguments | ForEach-Object { if ($_ -match '\s') { "`"$_`"" } else { $_ } }
    $log = Join-Path $OutDir "$Name.txt"
    $launch = @{ FilePath = $File; NoNewWindow = $true; PassThru = $true; RedirectStandardError = $log }
    if ($quoted) { $launch.ArgumentList = $quoted }
    $process = Start-Process @launch
    $null = $process.Handle
    if (-not $process.WaitForExit(300000)) { $process.Kill(); '{0,-8} {1}' -f 'TIMEOUT', $Name; return }
    '{0,-8} {1}' -f $process.ExitCode, $Name
}

# UI self-checks: every *-check option registered in main.cpp, minus those that need the user's files or a harness.
$main = Get-Content (Join-Path $root 'src\qt\main.cpp') -Raw
$names = [regex]::Matches($main, '(?:addOption|internal)\(\{"([a-z0-9-]+-check)"') | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique
$viaHarness = 'audio-evidence-ui-check','partial-search-ui-check','image-guard-check','open-pair-check','heif-ui-check'
$userFiles = 'image-false-positive-check','archive-acceptance-check'
$noMedia = 'smoke-check','criteria-check','branding-check','image-check','evidence-check','cache-policy-check','batch-transfer-check','session-scale-check','session-snapshot-check'
foreach ($name in $names | Where-Object { $viaHarness -notcontains $_ -and $userFiles -notcontains $_ }) {
    $media = switch ($name) {
        'subtitle-check' { Join-Path $Fixtures 'media\subtitled.mp4' }
        'ui-check' { $portrait }
        'appearance-check' { $portrait }
        'matching-check' { Join-Path $Fixtures 'matching\main12.mp4' }
        'playback-check' { Join-Path $Fixtures 'playback\cfr.mp4' }
        default { Join-Path $Fixtures 'media\plain.mp4' }
    }
    $arguments = @("--$name")
    if ($noMedia -notcontains $name) { $arguments += @('--media', $media) }
    $arguments += @('--screenshot', (Join-Path $OutDir "$name.png"))
    Invoke-Check $name $exe $arguments
}
if ($SkipHarnesses) { return }

# Harness programs drive the same exe with generated or public fixtures.
$heif = (Get-ChildItem (Join-Path $Fixtures 'heifdata') -Recurse -Filter 'rainbow-451x461.heic' | Select-Object -First 1).DirectoryName
$harnesses = [ordered]@{
    'CoreChecks' = @('--media', $portrait, '1080', '1920', '--no-artifacts')
    'CoreChecks-short' = @('--short', (Join-Path $Fixtures 'media\short.mp4'))
    'ShellChecks' = @($exe)
    'AudioEvidenceChecks' = @($ffmpeg, $exe)
    'PartialSearchChecks' = @($ffmpeg, $exe)
    'HeifChecks' = @($heif, $ffmpeg)
    'ImageGuardChecks' = @($ffmpeg, $exe)
}
foreach ($entry in $harnesses.GetEnumerator()) {
    $target = $entry.Key -replace '-short$', ''
    if ($Only -and $Only -notcontains $entry.Key) { continue }
    $program = Join-Path $build "$target.exe"
    if (-not (Test-Path -LiteralPath $program)) {
        $env:COMPAREEL_BUILD_DIR = $BuildDirectory
        Push-Location $root; try { cmd /c "tools\build.cmd --target $target" | Out-Null } finally { Pop-Location }
    }
    Invoke-Check $entry.Key $program $entry.Value
}
Invoke-Check 'heif-ui-check' $exe @('--heif-ui-check', '--', (Join-Path $heif 'rainbow-451x461.heic'), (Join-Path $heif 'simple_osm_tile_meta.avif'))
