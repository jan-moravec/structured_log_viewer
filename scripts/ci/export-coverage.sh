#!/usr/bin/env bash
# Merges the `clang-coverage` test profiles and writes coverage.lcov plus a
# summary report. `_deps/` (FetchContent), `build/` (generated headers), and
# `test/` are dropped so the report covers only library and app sources.
set -euo pipefail

BUILD_DIR=build/clang-coverage
BIN_DIR="$BUILD_DIR/bin/RelWithDebInfo"
IGNORE='(_deps|build|test)/'

llvm-profdata-23 merge -sparse "$BUILD_DIR"/profiles/*.profraw -o "$BUILD_DIR/merged.profdata"
OBJECTS=(
    "$BIN_DIR/tests"
    -object "$BIN_DIR/apptest"
    -object "$BIN_DIR/apptest_queue"
)
llvm-cov-23 export -format=lcov \
    -instr-profile="$BUILD_DIR/merged.profdata" \
    -ignore-filename-regex="$IGNORE" \
    "${OBJECTS[@]}" >coverage.lcov
echo "::group::Coverage summary (library + app only)"
llvm-cov-23 report \
    -instr-profile="$BUILD_DIR/merged.profdata" \
    -ignore-filename-regex="$IGNORE" \
    "${OBJECTS[@]}"
echo "::endgroup::"
