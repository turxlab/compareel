# Builds the Linux Flatpak on Windows by running tools/build-flatpak.sh in an Ubuntu container (Docker Desktop).
# The KDE runtime/SDK and the build cache stay in the Docker volume "compareel-flatpak", so later builds are quick.
# flatpak-builder sandboxes each build step, which needs a privileged container.
param([Parameter(Mandatory)][string]$Version)
$ErrorActionPreference = 'Stop'
if ($Version -notmatch '^\d+\.\d+\.\d+([-.][A-Za-z0-9.-]+)?$') { throw 'Invalid version.' }
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$setup = 'command -v flatpak-builder >/dev/null || (apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq flatpak flatpak-builder ccache >/dev/null)'
docker run --rm --privileged -v "${root}:/src" -v compareel-flatpak:/root/.local/share/flatpak -v compareel-flatpak-work:/work `
    -e FLATPAK_WORK=/work ubuntu:24.04 bash -c "$setup && bash /src/tools/build-flatpak.sh $Version"
if ($LASTEXITCODE -ne 0) { throw 'Flatpak build failed.' }
