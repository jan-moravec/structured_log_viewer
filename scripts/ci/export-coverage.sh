#!/usr/bin/env bash
# Merges the `clang-coverage` test profiles into coverage.lcov and prints a summary.
# Only library and app sources are counted: `_deps/`, `build/`, and `test/` are excluded.
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
