#!/bin/sh
# Run on a macOS host with Apple Clang, CMake, Ninja, Git and Python 3 installed.
set -eu
test "$#" -eq 2 || { echo "Usage: sh macos.sh CANDIDATE_COMMIT NEW_OUTPUT" >&2; exit 2; }
test "$(uname -s)" = Darwin
test "$(git rev-parse HEAD)" = "$1"
test -z "$(git status --porcelain --untracked-files=no)"
mkdir "$2"
out=$(cd "$2" && pwd)
(
    set -ex
    git rev-parse HEAD
    sw_vers
    uname -sm
    clang --version
    clang++ --version
    cmake --version
    ninja --version
    python3 --version
    # Refuse incomplete preseeded material before invoking the shared fetch script.
    awk '{print $2 "  " $1}' scripts/blueprint-sources.txt > "$out/dependencies.sha256"
    (cd .local/deps/sources && shasum -a 256 -c "$out/dependencies.sha256")
    sh scripts/fetch-blueprint-deps.sh
    CC=clang CXX=/nonexistent/brw-cxx cmake -S brw -B "$out/c" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DBRW_TEST_GIT=ON \
        -DBRW_BLUEPRINT=ON -DBRW_ENABLE_CPP_MODULE=OFF
    if grep -q '^CMAKE_CXX_COMPILER:' "$out/c/CMakeCache.txt"; then exit 1; fi
    cmake --build "$out/c" --parallel
    ctest --test-dir "$out/c" --no-tests=error --output-on-failure
    python3 tests/blueprint-acceptance/run.py --build-dir "$out/c" --evidence "$out/blueprint"
    sh brw/blueprint_apfs_test.sh APFS "$out/c/brw"
    sh brw/blueprint_apfs_test.sh APFSX "$out/c/brw"
    CC=clang CXX=clang++ cmake -S brw -B "$out/cpp" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DBRW_BLUEPRINT=OFF -DBRW_ENABLE_CPP_MODULE=ON
    cmake --build "$out/cpp" --parallel --target literal_test literal_offset_test
    ctest --test-dir "$out/cpp" --no-tests=error --output-on-failure -R '^digby_(literal|cpp_abi)$'
    shasum -a 256 "$out/c/brw"
) > "$out/macos.log" 2>&1
echo "Passed. Retain $out/macos.log and $out/blueprint."
