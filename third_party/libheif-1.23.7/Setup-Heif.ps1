$ErrorActionPreference='Stop'
$root=Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$archive=Join-Path $PSScriptRoot 'libheif-1.23.7.tar.gz'
$expected='f7483e818503ea6224c8863e4a56ac02c7089021a98472c811c12aa19faf2475'
if(!(Test-Path -LiteralPath $archive)) {
    $downloads=Join-Path $root '.deps/libheif-download'
    New-Item -ItemType Directory -Force -Path $downloads | Out-Null
    $archive=Join-Path $downloads 'libheif-1.23.7.tar.gz'
    if(!(Test-Path -LiteralPath $archive)) { Invoke-WebRequest -Uri 'https://github.com/strukturag/libheif/releases/download/v1.23.7/libheif-1.23.7.tar.gz' -OutFile $archive }
}
if((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $expected) { throw 'libheif source hash mismatch' }
$sources=Join-Path $root '.deps/libheif-src'
if(!(Test-Path -LiteralPath (Join-Path $sources 'libheif-1.23.7/CMakeLists.txt'))) {
    $names=& tar -tf $archive; if($LASTEXITCODE -ne 0){throw 'Cannot inspect libheif archive'}
    if($names | Where-Object { $_ -notmatch '^libheif-1\.23\.7/' -or $_ -match '(^|/)\.\.(/|$)|^[\/]|:' }){throw 'Unsafe source archive path'}
    if((& tar -tvf $archive) | Where-Object { $_ -match '^[lh]' }){throw 'Unexpected archive link'}
    New-Item -ItemType Directory -Force -Path $sources | Out-Null
    & tar -xf $archive -C $sources; if($LASTEXITCODE -ne 0){throw 'Source extraction failed'}
}
& (Join-Path $PSScriptRoot 'Build-Heif.cmd')
if($LASTEXITCODE -ne 0){throw 'libheif build failed'}
