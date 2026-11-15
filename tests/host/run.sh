#!/bin/sh
# Compiles Jellyfin, downloads and playback-control tests with a host compiler (GCC 13+ for
# <format>), with Horizon-specific pieces swapped for tests/host/stubs.
# Usage (inside a Linux container, repo at /src): sh /src/tests/host/run.sh
set -eu

SRC=/src
OUT=${KARAFIN_HOST_OUT:-/tmp/karafin-host}
mkdir -p "$OUT/include/nlohmann"
cp "$SRC/build/third_party/json.hpp" "$OUT/include/nlohmann/json.hpp"

# Platform-specific sources replaced by stubs.
EXCLUDE='core/(app|main|platform|thread|libinfo|third_party_impl)\.cpp|jellyfin/detail/random\.cpp|downloads/detail/sdcard\.cpp'
SOURCES=$(cd "$SRC/src" && ls core/*.cpp jellyfin/*.cpp jellyfin/detail/*.cpp downloads/*.cpp downloads/detail/*.cpp |
          grep -Ev "^($EXCLUDE)$" | sed "s|^|$SRC/src/|")
STUBS=$(ls "$SRC"/tests/host/stubs/*.cpp)

CXX=${CXX:-g++}
export CXX OUT SRC
FILES="$SOURCES $STUBS $SRC/tests/host/main.cpp"
# nlohmann/json translation units take ~0.5-1 GiB each; stay well inside a small WSL VM.
JOBS=$(nproc); [ "$JOBS" -gt 6 ] && JOBS=6
echo "== $($CXX --version | head -1)"
echo "== compiling $(echo $FILES | wc -w) files with $JOBS jobs"
mkdir -p "$OUT/obj"
# One object per source, in parallel; the object name keeps the module path unique.
# shellcheck disable=SC2086
printf '%s\n' $FILES | xargs -P "$JOBS" -I{} sh -c '
    obj="$OUT/obj/$(echo "{}" | sed "s|^$SRC/||; s|/|_|g").o"
    $CXX -std=c++20 -O0 -Wall -Wextra -Werror -Wno-missing-field-initializers -pthread \
        -DKARAFIN_VERSION="\"host\"" -DKARAFIN_HOST_TEST=1 \
        -I"$SRC/src" -isystem "$OUT/include" -c "{}" -o "$obj" || { echo "FAILED: {}"; exit 255; }'
$CXX -pthread "$OUT"/obj/*.o -lcurl -o "$OUT/karafin-host-tests"

# Tests run in a scratch dir: "sdmc:/..." and "romfs:/..." are plain relative paths on Linux.
WORK=$(mktemp -d)
mkdir -p "$WORK/sdmc:/switch/karafin" "$WORK/romfs:"
cd "$WORK"
echo "== running"
"$OUT/karafin-host-tests"
