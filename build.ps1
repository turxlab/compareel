param([switch]$Package, [string]$Version = '1.0.0', [string]$BuildDirectory = '.build')
$ErrorActionPreference = 'Stop'
$projectRoot = $PSScriptRoot
if ($Version -notmatch '^\d+\.\d+\.\d+([-.][A-Za-z0-9.-]+)?$') { throw 'Invalid version.' }
$destination = Join-Path $projectRoot "release\v$Version"
if ($Package -and (Test-Path -LiteralPath $destination)) { throw "Package already exists: $destination. Choose a new -Version; existing releases are preserved." }
$previousBuildDirectory = $env:COMPAREEL_BUILD_DIR
$previousDisplayVersion = $env:COMPAREEL_DISPLAY_VERSION
$env:COMPAREEL_BUILD_DIR = Join-Path $projectRoot $BuildDirectory
$env:COMPAREEL_DISPLAY_VERSION = if ($Package) { $Version } else { "$Version-dev" }
Push-Location -LiteralPath $projectRoot
try {
    & (Join-Path $projectRoot 'tools\build.cmd')
    if ($LASTEXITCODE -ne 0) { throw 'CMake build failed.' }
    if (-not $Package) { Write-Host "Build ready: $env:COMPAREEL_BUILD_DIR\Compareel.exe"; return }
    $staging = Join-Path $projectRoot ('.build\package-' + [guid]::NewGuid().ToString('N'))
    $app = Join-Path $staging 'Compareel'
    New-Item -ItemType Directory -Path $app | Out-Null
    Copy-Item -LiteralPath (Join-Path $env:COMPAREEL_BUILD_DIR 'Compareel.exe') -Destination $app
    # avdevice is capture-device support; none of the shipped libraries imports it.
    foreach ($library in @('avcodec-62','avformat-62','avutil-60','avfilter-11','swresample-6','swscale-9')) {
        Copy-Item -LiteralPath (Join-Path $projectRoot ".deps/ffmpeg-8.1-min/bin/$library.dll") -Destination $app
    }
    Copy-Item -LiteralPath (Join-Path $projectRoot '.deps/libheif-1.23.7/bin/heif.dll') -Destination $app
    $qt = Join-Path $projectRoot '.deps\Qt\6.8.3\msvc2022_64'
    & (Join-Path $qt 'bin\windeployqt.exe') --release --no-translations --no-compiler-runtime --no-opengl-sw --no-system-d3d-compiler --skip-plugin-types generic,tls,networkinformation,sqldrivers,iconengines --dir $app (Join-Path $app 'Compareel.exe')
    if ($LASTEXITCODE -ne 0) { throw 'Qt deployment failed; staging is preserved.' }
    # QSQLITE is selected by name, so dependency discovery cannot infer it from imports.
    $sqlPlugins = Join-Path $app 'sqldrivers'
    New-Item -ItemType Directory -Force -Path $sqlPlugins | Out-Null
    Copy-Item -LiteralPath (Join-Path $qt 'plugins\sqldrivers\qsqlite.dll') -Destination $sqlPlugins
    # The update check reaches GitHub over HTTPS through Windows' own TLS (Schannel); no OpenSSL is shipped.
    $tlsPlugins = Join-Path $app 'tls'
    New-Item -ItemType Directory -Force -Path $tlsPlugins | Out-Null
    Copy-Item -LiteralPath (Join-Path $qt 'plugins/tls/qschannelbackend.dll') -Destination $tlsPlugins
    $imagePlugins = Join-Path $app 'imageformats'
    New-Item -ItemType Directory -Force -Path $imagePlugins | Out-Null
    foreach ($plugin in @('qgif.dll','qicns.dll','qico.dll','qjpeg.dll','qsvg.dll','qtga.dll','qtiff.dll','qwbmp.dll','qwebp.dll')) {
        Copy-Item -LiteralPath (Join-Path $qt "plugins/imageformats/$plugin") -Destination $imagePlugins
    }
    # Languages are also embedded; the folder lets users add or correct a translation without rebuilding.
    Copy-Item -LiteralPath (Join-Path $projectRoot 'languages') -Destination $app -Recurse
    $vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vsPath = & $vsWhere -latest -products * -property installationPath
    $runtime = Get-ChildItem -LiteralPath (Join-Path $vsPath 'VC\Redist\MSVC') -Directory |
        Where-Object { $_.Name -match '^\d+\.' } | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
    if (-not $runtime) { throw 'Microsoft redistributable runtime was not found.' }
    $crt = Get-ChildItem -LiteralPath (Join-Path $runtime.FullName 'x64') -Directory -Filter '*.CRT' | Select-Object -First 1
    if (-not $crt) { throw 'Microsoft x64 CRT was not found.' }
    # Only the CRT DLLs that Compareel, Qt and libheif import; concrt/vccorlib/threads/codecvt are unused.
    foreach ($runtimeDll in @('vcruntime140.dll','vcruntime140_1.dll','msvcp140.dll','msvcp140_1.dll','msvcp140_2.dll','msvcp140_atomic_wait.dll')) {
        Copy-Item -LiteralPath (Join-Path $crt.FullName $runtimeDll) -Destination $app
    }
    # License texts travel with the program as one file; the source archives go into the separate sources zip
    # published next to the packages, as HandBrake does with its source tarball.
    $licenseSections = [Collections.Generic.List[string]]::new()
    $licenseFiles = @(Get-ChildItem -LiteralPath (Join-Path $projectRoot 'third_party\qt-6.8.3\LICENSES') -File | Sort-Object Name | ForEach-Object { @("Qt 6.8.3 - $($_.BaseName)", $_.FullName) }) +
        @('FFmpeg - LGPL-3.0', (Join-Path $projectRoot 'third_party\ffmpeg\COPYING.LGPLv3'),
          'libheif 1.23.7 - LGPL-3.0', (Join-Path $projectRoot 'third_party\libheif-1.23.7\COPYING'),
          'Lucide icons - ISC', (Join-Path $projectRoot 'third_party\lucide\LICENSE'))
    for ($i = 0; $i -lt $licenseFiles.Count; $i += 2) {
        $licenseSections.Add(('=' * 78) + "`r`n" + $licenseFiles[$i] + "`r`n" + ('=' * 78) + "`r`n`r`n" + (Get-Content -LiteralPath $licenseFiles[$i + 1] -Raw).TrimEnd())
    }
    $licenseText = $licenseSections -join "`r`n`r`n`r`n"
    Set-Content -LiteralPath (Join-Path $app 'licenses.txt') -Value $licenseText -Encoding utf8
    Copy-Item -LiteralPath (Join-Path $projectRoot 'THIRD_PARTY_NOTICES.md') -Destination $app
    Copy-Item -LiteralPath (Join-Path $projectRoot 'LICENSE') -Destination (Join-Path $app 'LICENSE.txt')
    $packageReadme = Get-Content -LiteralPath (Join-Path $projectRoot 'packaging\PACKAGE_README.md') -Raw
    Set-Content -LiteralPath (Join-Path $app 'README.md') -Value ($packageReadme.Replace('{{VERSION}}',$Version)) -Encoding utf8
    Copy-Item -LiteralPath (Join-Path $projectRoot 'packaging\SUPPORTED_FORMATS.md') -Destination $app
    $oldPath = $env:PATH
    $oldPluginPath = $env:QT_PLUGIN_PATH
    try {
        $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
        $env:QT_PLUGIN_PATH = ''
        $process = Start-Process -FilePath (Join-Path $app 'Compareel.exe') -ArgumentList '--smoke-check' -WindowStyle Hidden -PassThru -Wait
        if ($process.ExitCode -ne 0) { throw "Packaged startup failed: $app ($($process.ExitCode))" }
    } finally { $env:PATH = $oldPath; $env:QT_PLUGIN_PATH = $oldPluginPath }
    # Corresponding source: every tracked or new (not ignored) file of the working tree, so it matches the
    # build even before a commit, plus the Qt SBOMs from the SDK. Third-party source archives are under third_party.
    $sourceName = "Compareel-$Version-source"
    $sourceRoot = Join-Path $staging $sourceName
    $sourceFiles = & git -C $projectRoot -c core.quotepath=off ls-files --cached --others --exclude-standard
    if ($LASTEXITCODE -ne 0) { throw 'Source file list failed.' }
    foreach ($file in $sourceFiles) {
        if (-not (Test-Path -LiteralPath (Join-Path $projectRoot $file) -PathType Leaf)) { continue } # deleted but not yet committed
        $target = Join-Path $sourceRoot $file
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
        Copy-Item -LiteralPath (Join-Path $projectRoot $file) -Destination $target
    }
    $sbom = Join-Path $sourceRoot 'third_party\qt-6.8.3\sbom'
    New-Item -ItemType Directory -Force -Path $sbom | Out-Null
    foreach ($module in @('qtbase','qtsvg','qtimageformats')) { Copy-Item -LiteralPath (Join-Path $qt "sbom\$module-6.8.3.spdx.json") -Destination $sbom }
    # Windows' tar writes standard '/' entry names; Compress-Archive in PowerShell 5.1 writes '\', which other platforms misread.
    & "$env:SystemRoot\System32\tar.exe" -a -c -f (Join-Path $staging "$sourceName.zip") -C $staging $sourceName
    if ($LASTEXITCODE -ne 0) { throw 'Source archive failed.' }
    Remove-Item -LiteralPath $sourceRoot -Recurse -Force
    # No recursive delete or replacement of another release.
    $resolvedStaging = (Resolve-Path -LiteralPath $staging).Path
    $allowedStaging = Join-Path $projectRoot '.build\package-'
    if (-not $resolvedStaging.StartsWith($allowedStaging,[StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid staging path.' }
    # A fresh checkout (GitHub Actions) has no release folder yet.
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
    Move-Item -LiteralPath $resolvedStaging -Destination $destination
    # The downloads, named as HandBrake names its own: a portable zip of the folder and an installer of the same files.
    $download = "Compareel-$Version-x86_64-Win"
    & "$env:SystemRoot\System32\tar.exe" -a -c -f (Join-Path $destination "$download.zip") -C $destination Compareel
    if ($LASTEXITCODE -ne 0) { throw 'Portable archive failed.' }
    $iscc = @("${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:ProgramFiles\Inno Setup 6\ISCC.exe", "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe") |
        Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if ($iscc) {
        # Numeric file version as for the program: 1.0.0-rc3 -> 1.0.0.3.
        $fileVersion = ($Version -replace '^(\d+\.\d+\.\d+).*$', '$1') + '.' + $(if ($Version -match '-' -and $Version -match '(\d+)$') { $Matches[1] } else { '0' })
        & $iscc /Q "/DVersion=$Version" "/DFileVersion=$fileVersion" "/DSource=$(Join-Path $destination 'Compareel')" "/DOutput=$destination" "/DOutputName=$download" (Join-Path $projectRoot 'packaging\windows\Compareel.iss')
        if ($LASTEXITCODE -ne 0) { throw 'Installer build failed.' }
    } else { Write-Warning 'Inno Setup 6 was not found; the installer was skipped (winget install JRSoftware.InnoSetup).' }
    Get-ChildItem -LiteralPath $destination -File | Select-Object Name,Length
} finally { $env:COMPAREEL_BUILD_DIR = $previousBuildDirectory; $env:COMPAREEL_DISPLAY_VERSION = $previousDisplayVersion; Pop-Location }
