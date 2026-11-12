#!/bin/bash
# Builds the Linux package: a Flatpak bundle from packaging/linux/io.github.turxlab.Compareel.yml, written to
# release/v<version>/Compareel-<version>-x86_64.flatpak next to the GPL/LGPL sources zip.
# Needs flatpak and flatpak-builder; the KDE runtime and SDK are installed from Flathub on first use.
# On Windows, tools/Build-Flatpak.ps1 runs this inside Docker.
# Usage: tools/build-flatpak.sh <version>
set -euo pipefail
VERSION=${1:?usage: tools/build-flatpak.sh <version>}
[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+([-.][A-Za-z0-9.-]+)?$ ]] || { echo "Invalid version." >&2; exit 2; }
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK=${FLATPAK_WORK:-$ROOT/.deps/flatpak}
OUT="$ROOT/release/v$VERSION"
MANIFEST="$ROOT/packaging/linux/.flatpak-manifest.yml"
trap 'rm -f "$MANIFEST"' EXIT
sed "s/-DCOMPAREEL_DISPLAY_VERSION=[^], ]*/-DCOMPAREEL_DISPLAY_VERSION=$VERSION/" \
    "$ROOT/packaging/linux/io.github.turxlab.Compareel.yml" > "$MANIFEST"
mkdir -p "$WORK" "$OUT"
flatpak remote-add --user --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo
flatpak-builder --user --install-deps-from=flathub --force-clean --disable-rofiles-fuse --ccache \
    --state-dir="$WORK/state" --repo="$WORK/repo" "$WORK/build" "$MANIFEST"
flatpak build-bundle "$WORK/repo" "$OUT/Compareel-$VERSION-x86_64.flatpak" io.github.turxlab.Compareel \
    --runtime-repo=https://dl.flathub.org/repo/flathub.flatpakrepo
echo "Flatpak ready: $OUT/Compareel-$VERSION-x86_64.flatpak"
