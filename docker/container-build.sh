#!/bin/sh
# Runs inside karafin-builder with the repo mounted at /src.
# Env: KARAFIN_BUILD_TYPE (Release|Debug|RelWithDebInfo), KARAFIN_JOBS, KARAFIN_VERBOSE (0|1),
#      KARAFIN_BUILD_DIR (repo-relative, default build/switch), KARAFIN_COPY_OUT (1: copy to build/out)
set -eu

cd /src
build_type="${KARAFIN_BUILD_TYPE:-Release}"
jobs="${KARAFIN_JOBS:-$(nproc)}"
bdir="${KARAFIN_BUILD_DIR:-build/switch}"

if [ ! -f "$bdir/build.ninja" ] || [ "$(cat "$bdir/.build_type" 2>/dev/null || true)" != "$build_type" ]; then
    cmake -S . -B "$bdir" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake \
        -DCMAKE_BUILD_TYPE="$build_type" \
        -DKARAFIN_THIRD_PARTY_DIR=/src/build/third_party
    echo "$build_type" > "$bdir/.build_type"
fi

if [ "${KARAFIN_VERBOSE:-0}" = "1" ]; then
    cmake --build "$bdir" -j "$jobs" -- -v
else
    cmake --build "$bdir" -j "$jobs"
fi

if [ "${KARAFIN_COPY_OUT:-1}" = "1" ]; then
    mkdir -p build/out
    cp -f "$bdir/karafin.nro" build/out/karafin.nro
    ls -l build/out/karafin.nro
else
    ls -l "$bdir/karafin.nro"
fi
