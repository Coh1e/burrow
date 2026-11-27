#!/bin/sh
# Run inside the isolated Linux build namespace after the SSH transfer.
set -eu
export LC_ALL=C

fail() { printf '%s\n' "$1" >&2; exit 1; }
[ "$#" -eq 2 ] || fail 'Usage: build.sh NEW_OUTPUT MANIFEST_SHA256'
case "$2" in *[!0-9a-fA-F]*) fail 'Invalid Blueprint manifest SHA-256' ;; esac
[ "${#2}" -eq 64 ] || fail 'Invalid Blueprint manifest SHA-256'
manifest_hash=$(printf '%s' "$2" | tr 'A-F' 'a-f')
source_dir=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd -P)

# The SSH session captures its own namespace link before creating this one.
# Reading the link for PID 1 would require permissions unrelated to the build.
transport=${BRW_SSH_TRANSPORT_NETNS:?Missing transport network namespace}
printf '%s\n' "$transport" | grep -Eq '^net:\[[0-9]+\]$' || fail 'Invalid transport network namespace'
isolated=$(readlink /proc/self/ns/net)
[ "$isolated" != "$transport" ] || fail 'Build shares the SSH transport network namespace'
interfaces=$(awk 'NR > 2 {print $1}' /proc/self/net/dev)
[ "$interfaces" = 'lo:' ] || fail 'Build namespace has a non-loopback network interface'
routes4=$(ip -o -4 route show table all)
routes6=$(ip -o -6 route show table all)
printf '%s\n' "$routes4" "$routes6" |
    awk 'NF {for (i=1; i<NF; i++) if ($i=="dev" && $(i+1)=="lo") next; exit 1}' ||
    fail 'Build namespace has a non-loopback route'

mkdir -- "$1" || fail 'Build output must be a new directory'
out=$(CDPATH= cd -- "$1" && pwd -P)
evidence=$out/evidence
mkdir "$evidence"
{
    printf 'transport_netns=%s\nbuild_netns=%s\ninterfaces=lo\n' "$transport" "$isolated"
    printf 'IPv4 routes (all tables):\n%s\nIPv6 routes (all tables):\n%s\n' "$routes4" "$routes6"
} > "$evidence/network.txt"
{
    uname -srm
    cc --version
    cmake --version
    make --version
    python3 --version
} > "$evidence/toolchain.txt"

cd "$source_dir"
printf '%s  tests/blueprint-acceptance/kit/blueprint.manifest\n' "$manifest_hash" > "$evidence/blueprint.sha256"
sha256sum --check --status "$evidence/blueprint.sha256" || fail 'Transferred Blueprint manifest digest mismatch'
{
    sha256sum scripts/blueprint-sources.txt scripts/fetch-blueprint-deps.sh \
        tests/ssh-acceptance/build.sh tests/ssh-acceptance/linux_case.py \
        tests/ssh-acceptance/linux_builder.py tests/blueprint-acceptance/*.py
    find brw -type f -exec sha256sum '{}' +
} > "$evidence/sources.sha256"

# Check every preseeded archive before the existing preparation script runs.
awk '{print $2 "  " $1}' scripts/blueprint-sources.txt > "$evidence/dependencies.sha256"
(cd .local/deps/sources && sha256sum --check --status "$evidence/dependencies.sha256") ||
    fail 'A pinned dependency archive is missing or has the wrong digest'

status=0
(
    set -x
    sh scripts/fetch-blueprint-deps.sh &&
    CC=cc cmake -S brw -B "$out/build" -G 'Unix Makefiles' \
        -DCMAKE_BUILD_TYPE=Release -DBRW_BLUEPRINT=ON -DBUILD_TESTING=OFF &&
    cmake --build "$out/build" --parallel 4 --target brw
) > "$out/cmake.raw.log" 2>&1 || status=$?

# CMake and the compiler print absolute paths even for relative source inputs.
python3 - "$out" "$source_dir" <<'PY'
from pathlib import Path
import sys

out, source = sys.argv[1:]
raw = Path(out, 'cmake.raw.log')
with raw.open(encoding='utf-8', errors='replace') as lines, \
        Path(out, 'evidence/build.log').open('w', encoding='utf-8') as report:
    for line in lines:
        report.write(line.replace(out, '<output>').replace(source, '<source>'))
raw.unlink()
PY
[ "$status" -eq 0 ] || fail 'CMake build failed; see evidence/build.log'
(cd "$out/build" && sha256sum brw) > "$evidence/brw.sha256"
python3 -B tests/ssh-acceptance/linux_case.py --brw "$out/build/brw" --work "$out/case" \
    --evidence "$evidence/linux" --manifest-sha256 "$manifest_hash"
printf 'Isolated QuickJS build and Linux Blueprint acceptance passed.\n'
