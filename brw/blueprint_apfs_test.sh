#!/bin/sh
set -eu
format=$1
brw=$2
case "$format" in
    APFS) folded=ON ;;
    APFSX) folded=OFF ;;
    *) echo "Unknown APFS format: $format" >&2; exit 2 ;;
esac

fixture=$(cd "$(dirname "$0")" && pwd)/blueprint_test.cmake
scratch=$(mktemp -d "${TMPDIR:-/tmp}/brw-apfs.XXXXXXXX")
volume="$scratch/volume"
image="$scratch/fixture.dmg"
attached=false
cleanup() {
    if "$attached"; then hdiutil detach "$volume" || return 1; fi
    rm -f "$image"
    rmdir "$volume" "$scratch"
}
mkdir "$volume"
trap cleanup EXIT
hdiutil create -size 128m -fs "$format" -volname "brw-$format" "$image"
hdiutil attach -nobrowse -mountpoint "$volume" "$image"
attached=true
(
    cd "$volume"
    cmake "-DBRW=$brw" -DBLUEPRINT_COPY_CASE_ONLY=ON "-DBLUEPRINT_EXPECT_FOLDED=$folded" -P "$fixture"
)
