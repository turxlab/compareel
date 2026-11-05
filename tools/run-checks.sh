#!/bin/bash
# macOS/Linux counterpart of tools/Run-Checks.ps1: runs every Compareel self-check against a local build and
# prints one line per check (exit code 0 = pass). Logs and screenshots go to the output directory.
# Fixtures are generated once with ffmpeg (a full build with encoders, preferably Homebrew's ffmpeg-full) into
# .deps/test-fixtures;
# the libheif test images come from the libheif source archive in third_party.
# Usage: tools/run-checks.sh [--build DIR] [--out DIR] [--only name,name] [--skip-harnesses]
set -uo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD="$ROOT/.build-mac-arm64" OUT="${TMPDIR:-/tmp}/compareel-checks" ONLY="" HARNESSES=1
while [ $# -gt 0 ]; do
    case "$1" in
        --build) BUILD=$(cd "$2" && pwd); shift 2 ;;
        --out) OUT=$2; shift 2 ;;
        --only) ONLY=",$2,"; shift 2 ;;
        --skip-harnesses) HARNESSES=0; shift ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
done
# Some harnesses encode AV1 with libaom, which Homebrew has only in ffmpeg-full.
FFMPEG=$(brew --prefix ffmpeg-full 2>/dev/null)/bin/ffmpeg
[ -x "$FFMPEG" ] || FFMPEG=$(command -v ffmpeg) || { echo "ffmpeg is needed to make the fixtures." >&2; exit 1; }
EXE="$BUILD/Compareel.app/Contents/MacOS/Compareel"; [ -x "$EXE" ] || EXE="$BUILD/Compareel"
[ -x "$EXE" ] || { echo "No Compareel build in $BUILD." >&2; exit 1; }
FIXTURES="$ROOT/.deps/test-fixtures"
mkdir -p "$OUT"
export QT_FORCE_STDERR_LOGGING=1 COMPAREEL_CHECK_SHOTS="$OUT"

make_fixtures() {
    local f=(-hide_banner -loglevel error -nostdin -y) d="$FIXTURES"
    mkdir -p "$d/media" "$d/matching" "$d/playback"
    "$FFMPEG" "${f[@]}" -f lavfi -i testsrc2=size=1080x1920:rate=30 -f lavfi -i sine=frequency=440:sample_rate=48000 -t 4 -c:v libx264 -g 15 -pix_fmt yuv420p -c:a aac -shortest "$d/portrait.mp4"
    "$FFMPEG" "${f[@]}" -f lavfi -i testsrc2=size=640x360:rate=30 -f lavfi -i sine=frequency=330:sample_rate=48000 -t 6 -c:v libx264 -g 15 -pix_fmt yuv420p -c:a aac -shortest "$d/media/plain.mp4"
    printf '1\n00:00:00,500 --> 00:00:03,000\nCompareel subtitle check\n' > "$d/media/subtitle.srt"
    # subtitle-check expects the Windows fixture's 320x180 at 12 fps.
    "$FFMPEG" "${f[@]}" -f lavfi -i testsrc2=size=320x180:rate=12 -f lavfi -i sine=frequency=330:sample_rate=48000 -i "$d/media/subtitle.srt" -map 0 -map 1 -map 2 -t 4 -c:v libx264 -g 12 -pix_fmt yuv420p -c:a aac -c:s mov_text "$d/media/subtitled.mp4"
    rm -f "$d/media/subtitle.srt"
    "$FFMPEG" "${f[@]}" -f lavfi -i testsrc2=size=320x180:rate=10 -t 0.2 -c:v libx264 -g 15 -pix_fmt yuv420p "$d/media/short.mp4"
    "$FFMPEG" "${f[@]}" -f lavfi -i testsrc2=size=640x360:rate=12 -f lavfi -i sine=frequency=550:sample_rate=48000 -t 12 -c:v libx264 -g 15 -pix_fmt yuv420p -c:a aac -shortest "$d/matching/main12.mp4"
    "$FFMPEG" "${f[@]}" -i "$d/matching/main12.mp4" -c:v libx264 -crf 30 -pix_fmt yuv420p -c:a aac "$d/matching/encode.mp4"
    "$FFMPEG" "${f[@]}" -f lavfi -i mandelbrot=size=640x360:rate=12 -f lavfi -i sine=frequency=220:sample_rate=48000 -t 12 -c:v libx264 -g 15 -pix_fmt yuv420p -c:a aac -shortest "$d/matching/different.mp4"
    # The same four videos as tools/New-PlaybackFixture.ps1.
    "$FFMPEG" "${f[@]}" -f lavfi -i testsrc2=size=320x180:rate=30 -f lavfi -i anullsrc=r=48000:cl=stereo -t 240 -c:v libx264 -preset ultrafast -crf 25 -c:a aac "$d/playback/cfr.mp4"
    "$FFMPEG" "${f[@]}" -i "$d/playback/cfr.mp4" -vf 'select=if(lt(mod(t\,10)\,5)\,not(mod(n\,2))\,1)' -fps_mode vfr -c:v libx264 -preset ultrafast -crf 25 -c:a copy "$d/playback/vfr.mp4"
    for rate in 24 60; do
        "$FFMPEG" "${f[@]}" -f lavfi -i testsrc2=size=320x180:rate=$rate -f lavfi -i anullsrc=r=48000:cl=stereo -t 30 -c:v libx264 -preset ultrafast -crf 25 -c:a aac "$d/playback/fps$rate.mp4"
    done
    mkdir -p "$d/heifdata" && tar xzf "$ROOT"/third_party/libheif-*/libheif-*.tar.gz -C "$d/heifdata" --include '*/tests/data/*'
}
[ -f "$FIXTURES/playback/fps60.mp4" ] && [ -d "$FIXTURES/heifdata" ] || make_fixtures || { echo "Fixture generation failed." >&2; exit 1; }

# Runs one check with a timeout, so a stuck check never leaves a window behind.
check() { # name program args...
    local name=$1; shift
    [ -z "$ONLY" ] || [[ "$ONLY" == *",$name,"* ]] || return 0
    perl -e 'alarm 300; exec @ARGV or exit 127' "$@" > "$OUT/$name.out" 2> "$OUT/$name.txt"
    local code=$?
    [ $code = 142 ] && code=TIMEOUT
    printf '%-8s %s\n' "$code" "$name"
}

# UI self-checks: every *-check option registered in main.cpp, minus those that need the user's files or a harness.
via_harness=" audio-evidence-ui-check partial-search-ui-check image-guard-check open-pair-check heif-ui-check "
user_files=" image-false-positive-check archive-acceptance-check "
no_media=" smoke-check criteria-check branding-check image-check evidence-check cache-policy-check batch-transfer-check session-scale-check session-snapshot-check "
for name in $(grep -oE '(addOption|internal)\(\{"[a-z0-9-]+-check"' "$ROOT/src/qt/main.cpp" | sed -E 's/.*"(.*)"/\1/' | sort -u); do
    [[ "$via_harness$user_files" == *" $name "* ]] && continue
    case "$name" in
        subtitle-check) media="$FIXTURES/media/subtitled.mp4" ;;
        ui-check|appearance-check) media="$FIXTURES/portrait.mp4" ;;
        matching-check) media="$FIXTURES/matching/main12.mp4" ;;
        playback-check) media="$FIXTURES/playback/cfr.mp4" ;;
        *) media="$FIXTURES/media/plain.mp4" ;;
    esac
    args=("--$name")
    [[ "$no_media" == *" $name "* ]] || args+=(--media "$media")
    check "$name" "$EXE" "${args[@]}" --screenshot "$OUT/$name.png"
done
[ "$HARNESSES" = 1 ] || exit 0

# Harness programs, built on demand (they are EXCLUDE_FROM_ALL targets).
harness() { # name target args...
    local name=$1 target=$2; shift 2
    [ -z "$ONLY" ] || [[ "$ONLY" == *",$name,"* ]] || return 0
    [ -x "$BUILD/$target" ] || cmake --build "$BUILD" --target "$target" > "$OUT/$target-build.txt" 2>&1 || { printf '%-8s %s\n' BUILD "$name"; return; }
    check "$name" "$BUILD/$target" "$@"
}
heif=$(dirname "$(find "$FIXTURES/heifdata" -name rainbow-451x461.heic | head -1)")
harness CoreChecks CoreChecks --media "$FIXTURES/portrait.mp4" 1080 1920 --no-artifacts
harness CoreChecks-short CoreChecks --short "$FIXTURES/media/short.mp4"
harness CoreChecks-fileops CoreChecks --fileops
harness CoreChecks-transfers CoreChecks --transfers
harness CoreChecks-matching CoreChecks --matching
harness CoreChecks-duration-filter CoreChecks --duration-filter
harness CoreChecks-resource-budget CoreChecks --resource-budget
harness CoreChecks-audio CoreChecks --audio "$FIXTURES/media/plain.mp4"
harness CoreChecks-scrub CoreChecks --scrub "$FIXTURES/media/plain.mp4"
harness CoreChecks-sampling-resume CoreChecks --sampling-resume "$FIXTURES/media/plain.mp4"
harness FFmpegChecks FFmpegChecks "$FIXTURES/media/plain.mp4" 640 360
harness SubtitleChecks SubtitleChecks "$FIXTURES/media/subtitled.mp4" "$FIXTURES/media/plain.mp4"
harness ScanManifestChecks ScanManifestChecks
harness CacheRenderChecks CacheRenderChecks
harness CacheCrashChecks CacheCrashChecks "$FIXTURES/media/plain.mp4"
harness OperationRecoveryChecks OperationRecoveryChecks
harness ImageChecks ImageChecks
harness AudioEvidenceChecks AudioEvidenceChecks "$FFMPEG" "$EXE"
harness PartialSearchChecks PartialSearchChecks "$FFMPEG" "$EXE"
harness HeifChecks HeifChecks "$heif" "$FFMPEG"
harness ImageGuardChecks ImageGuardChecks "$FFMPEG" "$EXE"
check heif-ui-check "$EXE" --heif-ui-check -- "$heif/rainbow-451x461.heic" "$heif/simple_osm_tile_meta.avif"
