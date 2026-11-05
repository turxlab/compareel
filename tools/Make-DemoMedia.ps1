# Builds a publishable demo media library (videos + photos + CREDITS.txt) for Compareel.
# Re-running keeps cached source files in "$Destination\_downloads" and replaces generated media.

param(
    [string]$Destination = 'C:\programs\compareel-demo',
    [string]$FFmpeg = ''
)

$ErrorActionPreference = 'Stop'

# Ensure TLS 1.2 is enabled for HTTPS downloads in Windows PowerShell 5.1.
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

$repoRoot = if ($PSScriptRoot) {
    Split-Path $PSScriptRoot -Parent
} else {
    (Get-Location).Path
}

if ([string]::IsNullOrWhiteSpace($FFmpeg)) {
    $FFmpeg = Join-Path "$repoRoot" '.deps\ffmpeg-8.1\bin\ffmpeg.exe'
}

if (!(Test-Path -LiteralPath "$FFmpeg" -PathType Leaf)) {
    throw "FFmpeg executable was not found at '$FFmpeg'. Ensure the bundled build exists at '.deps\ffmpeg-8.1\bin\ffmpeg.exe' or pass -FFmpeg."
}

function Ensure-Directory([string]$DirectoryPath) {
    if (!(Test-Path -LiteralPath "$DirectoryPath" -PathType Container)) {
        [System.IO.Directory]::CreateDirectory("$DirectoryPath") | Out-Null
    }
}

function Invoke-FFmpeg([string]$Description, [string[]]$Arguments) {
    Write-Host "  [ffmpeg] $Description"
    & "$FFmpeg" -hide_banner -loglevel error -nostdin -y @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "FFmpeg failed while generating '$Description' (exit code $LASTEXITCODE)."
    }
}

# Source manifest. Leave Sha256 empty ('') if unknown in advance: the script prints the hash
# and warns instead of failing; populate Sha256 after the first verified download to lock it.
$Sources = @(
    [PSCustomObject]@{
        Id       = 'bbb_480p'
        Kind     = 'Video'
        FileName = 'BigBuckBunny_640x360.m4v.zip'
        Extract  = 'BigBuckBunny_640x360.m4v'
        Title    = 'Big Buck Bunny (640x360)'
        Url      = 'https://download.blender.org/peach/bigbuckbunny_movies/BigBuckBunny_640x360.m4v.zip'
        Author   = '© Blender Foundation | www.blender.org'
        License  = 'CC BY 3.0 (https://creativecommons.org/licenses/by/3.0/)'
        Sha256   = ''
    },
    [PSCustomObject]@{
        Id       = 'sintel_trailer_720p'
        Kind     = 'Video'
        FileName = 'sintel_trailer-720p.mp4'
        Title    = 'Sintel Trailer (720p H.264)'
        Url      = 'https://download.blender.org/durian/trailer/sintel_trailer-720p.mp4'
        Author   = '© Blender Foundation | www.blender.org'
        License  = 'CC BY 3.0 (https://creativecommons.org/licenses/by/3.0/)'
        Sha256   = ''
    },
    [PSCustomObject]@{
        Id         = 'space_blue_marble'
        Kind       = 'Photo'
        FileName   = 'space_earth_apollo17.jpg'
        CameraName = 'IMG_20250304_110812.jpg'
        Title      = 'The Blue Marble — Earth from Apollo 17 (as17-148-22727)'
        Url        = 'https://images-assets.nasa.gov/image/as17-148-22727/as17-148-22727~large.jpg'
        Author     = 'NASA / Apollo 17 Crew'
        License    = 'Public domain, NASA'
        Sha256     = ''
    },
    [PSCustomObject]@{
        Id         = 'space_ngc4414'
        Kind       = 'Photo'
        FileName   = 'space_spiral_galaxy_ngc4414.jpg'
        CameraName = 'IMG_20250315_221409.jpg'
        Title      = 'Spiral Galaxy NGC 4414 (Hubble Space Telescope)'
        Url        = 'https://upload.wikimedia.org/wikipedia/commons/c/c3/NGC_4414_%28NASA-med%29.jpg'
        Author     = 'NASA / Hubble Heritage Team (STScI/AURA)'
        License    = 'Public domain, NASA'
        Sha256     = ''
    },
    [PSCustomObject]@{
        Id         = 'landscape_prismatic_spring'
        Kind       = 'Photo'
        FileName   = 'landscape_grand_prismatic_spring.jpg'
        CameraName = 'IMG_20250614_134522.jpg'
        Title      = 'Grand Prismatic Spring, Yellowstone National Park'
        Url        = 'https://commons.wikimedia.org/wiki/Special:FilePath/Grand_prismatic_spring.jpg'
        Author     = 'Jim Peaco, U.S. National Park Service'
        License    = 'Public domain, U.S. National Park Service (PD-USGov-NPS)'
        Sha256     = ''
    },
    [PSCustomObject]@{
        Id         = 'landscape_tetons_river'
        Kind       = 'Photo'
        FileName   = 'landscape_tetons_snake_river.jpg'
        CameraName = 'IMG_20250618_075130.jpg'
        Title      = 'The Tetons and the Snake River, Grand Teton National Park (NARA 519904)'
        Url        = 'https://commons.wikimedia.org/wiki/Special:FilePath/Adams_The_Tetons_and_the_Snake_River.jpg'
        Author     = 'U.S. Department of the Interior / National Park Service (Ansel Adams, 1942)'
        License    = 'Public domain, U.S. National Archives / National Park Service (PD-USGov-Interior)'
        Sha256     = ''
    },
    [PSCustomObject]@{
        Id         = 'animal_bison'
        Kind       = 'Photo'
        FileName   = 'animal_american_bison.jpg'
        CameraName = 'IMG_20250703_162045.jpg'
        Title      = 'American Bison in Pasture (ARS k5680-1)'
        Url        = 'https://upload.wikimedia.org/wikipedia/commons/8/8d/American_bison_k5680-1.jpg'
        Author     = 'Jack Dykinga, USDA Agricultural Research Service'
        License    = 'Public domain, USDA Agricultural Research Service (PD-USGov-USDA-ARS)'
        Sha256     = ''
    },
    [PSCustomObject]@{
        Id         = 'animal_kodiak_bear'
        Kind       = 'Photo'
        FileName   = 'animal_brown_bear_katmai.jpg'
        CameraName = 'IMG_20250709_101533.jpg'
        Title      = 'Brown Bear, Katmai National Park and Preserve'
        Url        = 'https://upload.wikimedia.org/wikipedia/commons/4/4c/Brown_Bear%2C_Katmai_Perserve_NPS_Photo_Russ_Taylor_%2843713410994%29.jpg'
        Author     = 'NPS Photo / Russ Taylor, Katmai National Park and Preserve'
        License    = 'Public domain, U.S. National Park Service'
        Sha256     = ''
    },
    [PSCustomObject]@{
        Id         = 'city_arlington_aerial'
        Kind       = 'Photo'
        FileName   = 'city_baton_rouge_waterfront.jpg'
        CameraName = 'IMG_20250822_145018.jpg'
        Title      = 'Aerial view of the Baton Rouge riverfront, Louisiana'
        Url        = 'https://upload.wikimedia.org/wikipedia/commons/b/b2/Baton_Rouge_Louisiana_waterfront_aerial_view.jpg'
        Author     = 'Michael Maples, U.S. Army Corps of Engineers'
        License    = 'Public domain, U.S. Army Corps of Engineers'
        Sha256     = ''
    },
    [PSCustomObject]@{
        Id         = 'city_capitol_plaza'
        Kind       = 'Photo'
        FileName   = 'city_rochester_downtown.jpg'
        CameraName = 'IMG_20250823_173204.jpg'
        Title      = 'Aerial view of downtown Rochester, Minnesota'
        Url        = 'https://upload.wikimedia.org/wikipedia/commons/5/59/Rochester_Minnesota_downtown_aerial_view.jpg'
        Author     = 'U.S. Army Corps of Engineers'
        License    = 'Public domain, U.S. Army Corps of Engineers'
        Sha256     = ''
    }
)

Write-Host "Building Compareel demo media library in '$Destination'..."
Write-Host "Using FFmpeg: '$FFmpeg'"

Ensure-Directory "$Destination"
$downloadsDir = Join-Path "$Destination" '_downloads'
Ensure-Directory "$downloadsDir"

# Step 1: Download source media once into _downloads and verify SHA-256 hashes.
Write-Host ''
Write-Host '[1/4] Checking and downloading source media...'
$sourcePaths = @{}
$computedHashes = @{}
$httpHeaders = @{
    'User-Agent' = 'CompareelDemoMediaBuilder/1.0 (local publishable demo dataset script)'
}

foreach ($src in $Sources) {
    $cachedPath = Join-Path "$downloadsDir" $src.FileName
    $tempPath = "$cachedPath.part"

    if (Test-Path -LiteralPath "$cachedPath" -PathType Leaf) {
        Write-Host "  [cached] $($src.FileName)"
    } else {
        Write-Host "  [download] $($src.FileName) <- $($src.Url)"
        if (Test-Path -LiteralPath "$tempPath" -PathType Leaf) {
            Remove-Item -LiteralPath "$tempPath" -Force
        }

        $previousProgress = $ProgressPreference
        try {
            $ProgressPreference = 'SilentlyContinue'
            # Wikimedia answers 429 to quick successive requests; wait and retry a few times.
            for ($attempt = 1; ; $attempt++) {
                try {
                    Invoke-WebRequest -Uri $src.Url -OutFile "$tempPath" -UseBasicParsing -Headers $httpHeaders
                    break
                } catch {
                    $status = $_.Exception.Response.StatusCode.value__
                    if ($status -ne 429 -or $attempt -ge 5) { throw }
                    Write-Host "    rate limited, retrying in $(10 * $attempt) s"
                    Start-Sleep -Seconds (10 * $attempt)
                }
            }
            Start-Sleep -Seconds 2
        } catch {
            if (Test-Path -LiteralPath "$tempPath" -PathType Leaf) {
                Remove-Item -LiteralPath "$tempPath" -Force
            }
            throw "Download failed for '$($src.FileName)' from '$($src.Url)': $($_.Exception.Message)"
        } finally {
            $ProgressPreference = $previousProgress
        }

        if (!(Test-Path -LiteralPath "$tempPath" -PathType Leaf) -or ((Get-Item -LiteralPath "$tempPath").Length -le 0)) {
            if (Test-Path -LiteralPath "$tempPath" -PathType Leaf) {
                Remove-Item -LiteralPath "$tempPath" -Force
            }
            throw "Download produced an empty or missing file for '$($src.FileName)' from '$($src.Url)'."
        }

        Move-Item -LiteralPath "$tempPath" -Destination "$cachedPath" -Force
    }

    $actualHash = (Get-FileHash -LiteralPath "$cachedPath" -Algorithm SHA256).Hash.ToUpperInvariant()
    $computedHashes[$src.Id] = $actualHash

    if ([string]::IsNullOrWhiteSpace($src.Sha256)) {
        Write-Host "    SHA-256 ($($src.FileName)): $actualHash"
        Write-Warning "No expected SHA-256 set for '$($src.FileName)'; verified presence only. Computed SHA-256: $actualHash"
    } else {
        $expectedHash = $src.Sha256.Trim().ToUpperInvariant()
        if ($actualHash -ne $expectedHash) {
            throw "SHA-256 mismatch for '$($src.FileName)'. Expected $expectedHash but got $actualHash."
        }
        Write-Host "    SHA-256 verified: $actualHash"
    }

    if ($src.PSObject.Properties['Extract']) {
        $extracted = Join-Path "$downloadsDir" $src.Extract
        if (!(Test-Path -LiteralPath "$extracted" -PathType Leaf)) {
            Write-Host "  [extract] $($src.Extract)"
            Expand-Archive -LiteralPath "$cachedPath" -DestinationPath "$downloadsDir" -Force
            if (!(Test-Path -LiteralPath "$extracted" -PathType Leaf)) { throw "'$($src.Extract)' was not found inside '$($src.FileName)'." }
        }
        $sourcePaths[$src.Id] = $extracted
    } else {
        $sourcePaths[$src.Id] = $cachedPath
    }
}

# Step 2: Reset generated folders (preserving _downloads) and create target directory structure.
Write-Host ''
Write-Host '[2/4] Preparing clean output folders (keeping _downloads)...'

$videosRoot = Join-Path "$Destination" 'Videos'
$photosRoot = Join-Path "$Destination" 'Photos'
$creditsPath = Join-Path "$Destination" 'CREDITS.txt'

foreach ($generatedPath in @("$videosRoot", "$photosRoot", "$creditsPath")) {
    if (Test-Path -LiteralPath "$generatedPath") {
        Remove-Item -LiteralPath "$generatedPath" -Recurse -Force
    }
}

$videosCameraDir    = Join-Path "$videosRoot" 'Camera'
$videosBackupDir    = Join-Path "$videosRoot" 'Backup'
$videosDownloadsDir = Join-Path "$videosRoot" 'Downloads'
$videosEditsDir     = Join-Path "$videosRoot" 'Edits'

$photosCameraDir = Join-Path "$photosRoot" 'Camera'
$photosBackupDir = Join-Path "$photosRoot" 'Backup'
$photosSharedDir = Join-Path "$photosRoot" 'Shared'
$photosEditedDir = Join-Path "$photosRoot" 'Edited'

foreach ($dir in @(
    "$videosCameraDir", "$videosBackupDir", "$videosDownloadsDir", "$videosEditsDir",
    "$photosCameraDir", "$photosBackupDir", "$photosSharedDir", "$photosEditedDir"
)) {
    Ensure-Directory "$dir"
}

# Step 3: Build video library (OpenH264 + AAC, the H.264 encoder in the bundled FFmpeg).
Write-Host ''
Write-Host '[3/4] Generating video clips and variants...'

$cameraClips = @(
    [PSCustomObject]@{ FileName = 'VID_20250412_091520.mp4'; SourceId = 'bbb_480p';            Start = 35;  Duration = 30 },
    [PSCustomObject]@{ FileName = 'VID_20250412_104218.mp4'; SourceId = 'bbb_480p';            Start = 105; Duration = 36 },
    [PSCustomObject]@{ FileName = 'VID_20250519_153044.mp4'; SourceId = 'bbb_480p';            Start = 210; Duration = 40 },
    [PSCustomObject]@{ FileName = 'VID_20250608_171205.mp4'; SourceId = 'bbb_480p';            Start = 370; Duration = 32 },
    [PSCustomObject]@{ FileName = 'VID_20250721_194830.mp4'; SourceId = 'sintel_trailer_720p'; Start = 5;   Duration = 38 }
)

foreach ($clip in $cameraClips) {
    $srcVideo = $sourcePaths[$clip.SourceId]
    $dstVideo = Join-Path "$videosCameraDir" $clip.FileName
    Invoke-FFmpeg "Videos\Camera\$($clip.FileName)" @(
        '-ss', "$($clip.Start)",
        '-i', "$srcVideo",
        '-t', "$($clip.Duration)",
        '-vf', 'setsar=1',
        '-c:v', 'libopenh264',
        '-b:v', '2000k',
        '-pix_fmt', 'yuv420p',
        '-c:a', 'aac',
        '-ac', '2',
        '-b:a', '128k',
        '-movflags', '+faststart',
        "$dstVideo"
    )
}

# Videos\Backup: exact byte copies of two originals under different names.
$videoBackups = @(
    [PSCustomObject]@{ Source = 'VID_20250412_091520.mp4'; Target = 'Forest Stream 2025-04-12.mp4' },
    [PSCustomObject]@{ Source = 'VID_20250721_194830.mp4'; Target = 'Mountain Ridge Backup.mp4' }
)
foreach ($copy in $videoBackups) {
    $srcCopy = Join-Path "$videosCameraDir" $copy.Source
    $dstCopy = Join-Path "$videosBackupDir" $copy.Target
    Write-Host "  [copy] Videos\Backup\$($copy.Target)"
    Copy-Item -LiteralPath "$srcCopy" -Destination "$dstCopy" -Force
}

# Videos\Downloads: two smaller (lower resolution & bitrate) re-encodes, one .mkv container variant,
# and one middle-50% trimmed clip (seconds 10..30 of the 40-second original).
$smallerVideos = @(
    [PSCustomObject]@{ Source = 'VID_20250412_104218.mp4'; Target = 'Meadow Clip (360p).mp4' },
    [PSCustomObject]@{ Source = 'VID_20250519_153044.mp4'; Target = 'Forest Woods (360p).mp4' }
)
foreach ($small in $smallerVideos) {
    $srcClip = Join-Path "$videosCameraDir" $small.Source
    $dstClip = Join-Path "$videosDownloadsDir" $small.Target
    Invoke-FFmpeg "Videos\Downloads\$($small.Target)" @(
        '-i', "$srcClip",
        '-vf', 'scale=trunc(iw/4)*2:trunc(ih/4)*2,setsar=1',
        '-c:v', 'libopenh264',
        '-b:v', '400k',
        '-pix_fmt', 'yuv420p',
        '-c:a', 'aac',
        '-ac', '2',
        '-b:a', '96k',
        '-movflags', '+faststart',
        "$dstClip"
    )
}

$mkvSrc = Join-Path "$videosCameraDir" 'VID_20250608_171205.mp4'
$mkvDst = Join-Path "$videosDownloadsDir" 'Sunny Clearing Archive.mkv'
Invoke-FFmpeg 'Videos\Downloads\Sunny Clearing Archive.mkv' @(
    '-i', "$mkvSrc",
    '-vf', 'scale=trunc(iw/4)*2:trunc(ih/4)*2,setsar=1',
    '-c:v', 'libopenh264',
    '-b:v', '600k',
    '-pix_fmt', 'yuv420p',
    '-c:a', 'aac',
    '-ac', '2',
    '-b:a', '96k',
    "$mkvDst"
)

# Middle 50% of 40s VID_20250519_153044.mp4 -> start at 10s (25%), duration 20s (50%).
$trimMidSrc = Join-Path "$videosCameraDir" 'VID_20250519_153044.mp4'
$trimMidDst = Join-Path "$videosDownloadsDir" 'Forest Woods Excerpt.mp4'
Invoke-FFmpeg 'Videos\Downloads\Forest Woods Excerpt.mp4' @(
    '-ss', '10',
    '-i', "$trimMidSrc",
    '-t', '20',
    '-vf', 'setsar=1',
    '-c:v', 'libopenh264',
    '-b:v', '2000k',
    '-pix_fmt', 'yuv420p',
    '-c:a', 'aac',
    '-ac', '2',
    '-b:a', '128k',
    '-movflags', '+faststart',
    "$trimMidDst"
)

# Videos\Edits: start-trimmed clip, horizontally mirrored clip, and completely unrelated clip.
$editTrimSrc = Join-Path "$videosCameraDir" 'VID_20250412_091520.mp4'
$editTrimDst = Join-Path "$videosEditsDir" 'Stream Morning Intro Cut.mp4'
Invoke-FFmpeg 'Videos\Edits\Stream Morning Intro Cut.mp4' @(
    '-ss', '4',
    '-i', "$editTrimSrc",
    '-vf', 'setsar=1',
    '-c:v', 'libopenh264',
    '-b:v', '2000k',
    '-pix_fmt', 'yuv420p',
    '-c:a', 'aac',
    '-ac', '2',
    '-b:a', '128k',
    '-movflags', '+faststart',
    "$editTrimDst"
)

$editMirrorSrc = Join-Path "$videosCameraDir" 'VID_20250721_194830.mp4'
$editMirrorDst = Join-Path "$videosEditsDir" 'Mountain Ridge Mirrored.mp4'
Invoke-FFmpeg 'Videos\Edits\Mountain Ridge Mirrored.mp4' @(
    '-i', "$editMirrorSrc",
    '-vf', 'hflip,setsar=1',
    '-c:v', 'libopenh264',
    '-b:v', '2000k',
    '-pix_fmt', 'yuv420p',
    '-c:a', 'aac',
    '-ac', '2',
    '-b:a', '128k',
    '-movflags', '+faststart',
    "$editMirrorDst"
)

# Non-overlapping segment (08:00-08:28 of Big Buck Bunny) unused in Camera/Backup/Downloads.
$unrelatedSrc = $sourcePaths['bbb_480p']
$unrelatedDst = Join-Path "$videosEditsDir" 'Tree Canopy Glide.mp4'
Invoke-FFmpeg 'Videos\Edits\Tree Canopy Glide.mp4' @(
    '-ss', '480',
    '-i', "$unrelatedSrc",
    '-t', '28',
    '-vf', 'setsar=1',
    '-c:v', 'libopenh264',
    '-b:v', '2000k',
    '-pix_fmt', 'yuv420p',
    '-c:a', 'aac',
    '-ac', '2',
    '-b:a', '128k',
    '-movflags', '+faststart',
    "$unrelatedDst"
)

$editGraySrc = Join-Path "$videosCameraDir" 'VID_20250412_104218.mp4'
$editGrayDst = Join-Path "$videosEditsDir" 'Meadow Grayscale.mp4'
Invoke-FFmpeg 'Videos\Edits\Meadow Grayscale.mp4' @(
    '-i', "$editGraySrc",
    '-vf', 'format=gray,format=yuv420p,setsar=1',
    '-c:v', 'libopenh264',
    '-b:v', '2000k',
    '-pix_fmt', 'yuv420p',
    '-c:a', 'aac',
    '-ac', '2',
    '-b:a', '128k',
    '-movflags', '+faststart',
    "$editGrayDst"
)

$editLetterboxSrc = Join-Path "$videosCameraDir" 'VID_20250608_171205.mp4'
$editLetterboxDst = Join-Path "$videosEditsDir" 'Sunny Clearing Letterboxed.mp4'
Invoke-FFmpeg 'Videos\Edits\Sunny Clearing Letterboxed.mp4' @(
    '-i', "$editLetterboxSrc",
    '-vf', 'scale=512:288,pad=640:360:(ow-iw)/2:(oh-ih)/2,setsar=1',
    '-c:v', 'libopenh264',
    '-b:v', '2000k',
    '-pix_fmt', 'yuv420p',
    '-c:a', 'aac',
    '-ac', '2',
    '-b:a', '128k',
    '-movflags', '+faststart',
    "$editLetterboxDst"
)

$editFpsSrc = Join-Path "$videosCameraDir" 'VID_20250519_153044.mp4'
$editFpsDst = Join-Path "$videosEditsDir" 'Forest Woods 15fps.mp4'
Invoke-FFmpeg 'Videos\Edits\Forest Woods 15fps.mp4' @(
    '-i', "$editFpsSrc",
    '-vf', 'fps=15,setsar=1',
    '-c:v', 'libopenh264',
    '-b:v', '2000k',
    '-pix_fmt', 'yuv420p',
    '-c:a', 'aac',
    '-ac', '2',
    '-b:a', '128k',
    '-movflags', '+faststart',
    "$editFpsDst"
)

$editBlurSrc = Join-Path "$videosCameraDir" 'VID_20250412_091520.mp4'
$editBlurDst = Join-Path "$videosEditsDir" 'Forest Stream Blurred.mp4'
Invoke-FFmpeg 'Videos\Edits\Forest Stream Blurred.mp4' @(
    '-i', "$editBlurSrc",
    '-vf', 'gblur=sigma=2,setsar=1',
    '-c:v', 'libopenh264',
    '-b:v', '2000k',
    '-pix_fmt', 'yuv420p',
    '-c:a', 'aac',
    '-ac', '2',
    '-b:a', '128k',
    '-movflags', '+faststart',
    "$editBlurDst"
)

# Step 4: Build photo library and write CREDITS.txt.
Write-Host ''
Write-Host '[4/4] Generating photo variants and writing CREDITS.txt...'

$photoSources = $Sources | Where-Object { $_.Kind -eq 'Photo' }
foreach ($photo in $photoSources) {
    $srcPhoto = $sourcePaths[$photo.Id]
    $dstPhoto = Join-Path "$photosCameraDir" $photo.CameraName
    Write-Host "  [copy] Photos\Camera\$($photo.CameraName)"
    Copy-Item -LiteralPath "$srcPhoto" -Destination "$dstPhoto" -Force
}

# Photos\Backup: exact byte copies of three originals.
$photoBackups = @(
    [PSCustomObject]@{ Source = 'IMG_20250304_110812.jpg'; Target = 'Blue Earth Copy.jpg' },
    [PSCustomObject]@{ Source = 'IMG_20250614_134522.jpg'; Target = 'Hot Spring Backup.jpg' },
    [PSCustomObject]@{ Source = 'IMG_20250703_162045.jpg'; Target = 'Prairie Bison Copy.jpg' }
)
foreach ($copy in $photoBackups) {
    $srcCopy = Join-Path "$photosCameraDir" $copy.Source
    $dstCopy = Join-Path "$photosBackupDir" $copy.Target
    Write-Host "  [copy] Photos\Backup\$($copy.Target)"
    Copy-Item -LiteralPath "$srcCopy" -Destination "$dstCopy" -Force
}

# Photos\Shared: three resized (50%) + recompressed (JPEG quality ~70, mjpeg -q:v 9) copies,
# plus one PNG conversion and one WebP conversion.
$sharedJpegs = @(
    [PSCustomObject]@{ Source = 'IMG_20250614_134522.jpg'; Target = 'Hot Spring Small.jpg' },
    [PSCustomObject]@{ Source = 'IMG_20250709_101533.jpg'; Target = 'Wildlife Bear Small.jpg' },
    [PSCustomObject]@{ Source = 'IMG_20250822_145018.jpg'; Target = 'Riverfront Small.jpg' }
)
foreach ($shared in $sharedJpegs) {
    $srcImg = Join-Path "$photosCameraDir" $shared.Source
    $dstImg = Join-Path "$photosSharedDir" $shared.Target
    Invoke-FFmpeg "Photos\Shared\$($shared.Target)" @(
        '-i', "$srcImg",
        '-vf', 'scale=trunc(iw/4)*2:trunc(ih/4)*2,format=yuvj420p',
        '-frames:v', '1',
        '-update', '1',
        '-q:v', '9',
        "$dstImg"
    )
}

$pngSrc = Join-Path "$photosCameraDir" 'IMG_20250315_221409.jpg'
$pngDst = Join-Path "$photosSharedDir" 'Spiral Galaxy Shared.png'
Invoke-FFmpeg 'Photos\Shared\Spiral Galaxy Shared.png' @(
    '-i', "$pngSrc",
    '-frames:v', '1',
    '-update', '1',
    "$pngDst"
)

$webpSrc = Join-Path "$photosCameraDir" 'IMG_20250823_173204.jpg'
$webpDst = Join-Path "$photosSharedDir" 'Downtown Shared.webp'
Invoke-FFmpeg 'Photos\Shared\Downtown Shared.webp' @(
    '-i', "$webpSrc",
    '-frames:v', '1',
    '-c:v', 'libwebp',
    '-q:v', '75',
    "$webpDst"
)

# Photos\Edited: one cropped by ~10% (centered) and one slightly brightened.
$cropSrc = Join-Path "$photosCameraDir" 'IMG_20250618_075130.jpg'
$cropDst = Join-Path "$photosEditedDir" 'Mountain River Cropped.jpg'
Invoke-FFmpeg 'Photos\Edited\Mountain River Cropped.jpg' @(
    '-i', "$cropSrc",
    '-vf', 'crop=trunc(iw*0.9/2)*2:trunc(ih*0.9/2)*2:(iw-ow)/2:(ih-oh)/2,format=yuvj420p',
    '-frames:v', '1',
    '-update', '1',
    '-q:v', '3',
    "$cropDst"
)

$brightSrc = Join-Path "$photosCameraDir" 'IMG_20250703_162045.jpg'
$brightDst = Join-Path "$photosEditedDir" 'Prairie Bison Brightened.jpg'
Invoke-FFmpeg 'Photos\Edited\Prairie Bison Brightened.jpg' @(
    '-i', "$brightSrc",
    '-vf', 'lutyuv=y=val*1.08,format=yuvj420p',
    '-frames:v', '1',
    '-update', '1',
    '-q:v', '3',
    "$brightDst"
)

$blurSrc = Join-Path "$photosCameraDir" 'IMG_20250709_101533.jpg'
$blurDst = Join-Path "$photosEditedDir" 'Wildlife Bear Blurred.jpg'
Invoke-FFmpeg 'Photos\Edited\Wildlife Bear Blurred.jpg' @(
    '-i', "$blurSrc",
    '-vf', 'scale=trunc(iw/2)*2:trunc(ih/2)*2,gblur=sigma=3,format=yuvj420p',
    '-frames:v', '1',
    '-update', '1',
    '-q:v', '3',
    "$blurDst"
)

$graySrc = Join-Path "$photosCameraDir" 'IMG_20250822_145018.jpg'
$grayDst = Join-Path "$photosEditedDir" 'Riverfront Grayscale.jpg'
Invoke-FFmpeg 'Photos\Edited\Riverfront Grayscale.jpg' @(
    '-i', "$graySrc",
    '-vf', 'scale=trunc(iw/2)*2:trunc(ih/2)*2,format=gray,format=yuvj420p',
    '-frames:v', '1',
    '-update', '1',
    '-q:v', '3',
    "$grayDst"
)

$rotateSrc = Join-Path "$photosCameraDir" 'IMG_20250304_110812.jpg'
$rotateDst = Join-Path "$photosEditedDir" 'Blue Earth Rotated.jpg'
Invoke-FFmpeg 'Photos\Edited\Blue Earth Rotated.jpg' @(
    '-i', "$rotateSrc",
    '-vf', 'scale=trunc(iw/2)*2:trunc(ih/2)*2,transpose=1,format=yuvj420p',
    '-frames:v', '1',
    '-update', '1',
    '-q:v', '3',
    "$rotateDst"
)

$warmSrc = Join-Path "$photosCameraDir" 'IMG_20250823_173204.jpg'
$warmDst = Join-Path "$photosEditedDir" 'Downtown Warm.jpg'
Invoke-FFmpeg 'Photos\Edited\Downtown Warm.jpg' @(
    '-i', "$warmSrc",
    '-vf', 'scale=trunc(iw/2)*2:trunc(ih/2)*2,colorbalance=rs=0.08:gs=0.02:bs=-0.08:rm=0.15:gm=0.05:bm=-0.15,format=yuvj420p',
    '-frames:v', '1',
    '-update', '1',
    '-q:v', '3',
    "$warmDst"
)

$noiseSrc = Join-Path "$photosCameraDir" 'IMG_20250614_134522.jpg'
$noiseDst = Join-Path "$photosEditedDir" 'Hot Spring Noisy.jpg'
Invoke-FFmpeg 'Photos\Edited\Hot Spring Noisy.jpg' @(
    '-i', "$noiseSrc",
    '-vf', 'scale=trunc(iw/2)*2:trunc(ih/2)*2,noise=alls=12:allf=t,format=yuvj420p',
    '-frames:v', '1',
    '-update', '1',
    '-q:v', '3',
    "$noiseDst"
)

# Write CREDITS.txt in destination root.
$creditsLines = [System.Collections.Generic.List[string]]::new()
$creditsLines.Add('Compareel Demo Media Library - Attribution & License Credits')
$creditsLines.Add('============================================================')
$creditsLines.Add('')
$creditsLines.Add('All media in this folder is derived exclusively from Creative Commons Attribution (CC BY)')
$creditsLines.Add('Blender Foundation open movies and public-domain U.S. federal government photographs
(license of each photo checked on its Wikimedia Commons or NASA page).')
$creditsLines.Add('No personal media or personal file names are included.')
$creditsLines.Add('')

foreach ($src in $Sources) {
    $creditsLines.Add("Source File : _downloads\$($src.FileName)")
    if ($src.Kind -eq 'Photo') {
        $creditsLines.Add("Camera Copy : Photos\Camera\$($src.CameraName)")
    }
    $creditsLines.Add("Title       : $($src.Title)")
    $creditsLines.Add("URL         : $($src.Url)")
    $creditsLines.Add("Author      : $($src.Author)")
    $creditsLines.Add("License     : $($src.License)")
    $creditsLines.Add("SHA-256     : $($computedHashes[$src.Id])")
    $creditsLines.Add('')
}

$creditsLines | Set-Content -LiteralPath "$creditsPath" -Encoding utf8

Write-Host ''
Write-Host "Done. Demo media library and CREDITS.txt created at '$Destination'."
Write-Host "Cached downloads preserved in '$downloadsDir'."
