#!/bin/sh
# Run inside a pre-provisioned x86-64 Linux network namespace with proc mounted.
set -eu
export LC_ALL=C
repo=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
export CC=${CC:-gcc-4.8} CXX=/nonexistent/brw-cxx
test "$(uname -m)" = x86_64
test "$("$CC" --version | head -1 | awk '{print $NF}')" = 4.8.4
test "$(cmake --version | head -1)" = 'cmake version 3.10.3'
test "$(getconf GNU_LIBC_VERSION | cut -d' ' -f2)" = 2.19
test "$(awk 'NR > 2 {print $1}' /proc/net/dev)" = 'lo:'
if command -v g++ || command -v g++-4.8; then exit 1; fi
"$CC" --version
cmake --version
getconf GNU_LIBC_VERSION
cat /proc/net/dev
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/.local/deps/sources" "$work/build"
cp -R "$repo/brw" "$work/"
cd "$repo/.local/deps/sources"
sha256sum -c <<'HASHES'
fa62384995e8aa4f5a901c184fb5c91e56a29e24c05b6881a7f8fd5bbea694d2  libarchive-3.7.7.tar.gz
17e88863f3600672ab49182f217281b6fc4d3c762bde361935e436a95214d05c  zlib-1.3.1.tar.gz
HASHES
for archive in libarchive-3.7.7.tar.gz zlib-1.3.1.tar.gz; do
    tar xf "$archive" -C "$work/.local/deps/sources"
done
cd "$work/build"
cmake -DBRW_MINIMAL=ON -DCMAKE_BUILD_TYPE=MinSizeRel ../brw
cmake --build . -- -j4
ctest --output-on-failure
if grep -q '^CMAKE_CXX_COMPILER:' CMakeCache.txt; then exit 1; fi
readelf -lW brw > segments
readelf -dW brw > dynamic
if grep INTERP segments || grep NEEDED dynamic; then exit 1; fi
bytes=$(wc -c <brw)
echo "$bytes bytes"
test "$bytes" -le 2097152
sha256sum brw
