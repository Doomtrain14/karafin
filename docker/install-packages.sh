#!/bin/sh
# Installs the pinned Switchfin packages over the base image and verifies the
# official package versions listed in packages.lock. Runs inside `docker build`.
set -eu

dir="$1"
lock="$dir/packages.lock"

files=""
grep -v '^[[:space:]]*#' "$lock" | while read -r kind name version file sha url; do
    [ "$kind" = "pkg" ] || continue
    echo "$sha  $dir/$file" | sha256sum -c - >/dev/null || { echo "checksum mismatch: $file" >&2; exit 1; }
done

for f in $(grep -v '^[[:space:]]*#' "$lock" | awk '$1 == "pkg" { print $4 }'); do
    files="$files $dir/$f"
done

# Switchfin's mpv 0.36 replaces the official 0.39 build (which pulls libplacebo);
# mbedtls 3.x replaces 2.28 for every consumer, so let pacman overwrite files.
# shellcheck disable=SC2086
dkp-pacman -U --noconfirm --needed --overwrite '*' $files

fail=0
grep -v '^[[:space:]]*#' "$lock" | awk '$1 == "base" || $1 == "pkg" { print $2, $3 }' > /tmp/expected.txt
while read -r name version; do
    have=$(dkp-pacman -Q "$name" 2>/dev/null | awk '{ print $2 }') || have=""
    if [ "$have" != "$version" ]; then
        echo "package $name: expected $version, found '${have:-missing}'" >&2
        fail=1
    fi
done < /tmp/expected.txt
rm -f /tmp/expected.txt
[ "$fail" = 0 ] || exit 1

dkp-pacman -Scc --noconfirm >/dev/null 2>&1 || true
echo "karafin-builder packages OK"
