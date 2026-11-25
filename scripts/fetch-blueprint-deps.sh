#!/bin/sh
# Prepare only the sources used by the native POSIX Blueprint build.
set -eu
repo=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
sources="$repo/.local/deps/sources"
mkdir -p "$sources"
scratch=$(mktemp -d "$sources/.blueprint.XXXXXX")
trap 'rm -rf "$scratch"' EXIT
trap 'exit 1' HUP INT TERM

while read -r archive digest url; do
    input="$sources/$archive"
    if [ ! -f "$input" ]; then
        input="$scratch/$archive"
        curl -fL --retry 2 --output "$input" "$url"
    fi
    result=$(cmake -E sha256sum "$input")
    if [ "${result%% *}" != "$digest" ]; then
        echo "Dependency hash mismatch: $archive" >&2
        exit 1
    fi
    if [ "$input" != "$sources/$archive" ]; then
        mv "$input" "$sources/$archive"
    fi
    case "$archive" in
        libgit2-*) tar --no-same-owner -xf "$sources/$archive" -C "$sources" "${archive%%.tar.*}/src/util/hash/rfc6234" ;;
        quickjs-*) tar --no-same-owner -xf "$sources/$archive" -C "$sources" ;;
        *) echo "Unknown Blueprint dependency: $archive" >&2; exit 1 ;;
    esac
done < "$repo/scripts/blueprint-sources.txt"
