#!/usr/bin/env bash
# Runs every libFuzzer target of the clang-fuzz build for a fixed time, four at a time.
# Usage: scripts/ci/run-fuzz.sh [seconds per target, default 60]
# A crash, leak, hang or sanitizer report fails the script and leaves the input in fuzz-artifacts/.
set -euo pipefail

BIN_DIR=build/clang-fuzz/bin/RelWithDebInfo
TARGETS=(query json logfmt csv regex regex_template log_configuration timestamp decompress)

run_one() {
    local target=$1 seconds=$2
    local work="build/fuzz-work/$target"
    local dict="test/fuzz/dictionaries/$target.dict"
    local args=()
    mkdir -p "$work" fuzz-artifacts
    if [ -f "$dict" ]; then
        args+=("-dict=$dict")
    fi
    # New inputs go to the first directory, so the seed corpus in the repository stays unchanged.
    "$BIN_DIR/fuzz_$target" \
        -max_total_time="$seconds" -timeout=10 -rss_limit_mb=2048 -print_final_stats=1 \
        -artifact_prefix="fuzz-artifacts/$target-" "${args[@]}" \
        "$work" "test/fuzz/corpus/$target" "test/fuzz/regressions/$target" >"fuzz-artifacts/$target.log" 2>&1 ||
        {
            echo "fuzz_$target failed:" >&2
            tail -n 60 "fuzz-artifacts/$target.log" >&2
            return 1
        }
    echo "fuzz_$target ran for $seconds seconds without a finding"
}

if [ "${1:-}" = "--one" ]; then
    run_one "$2" "$3"
    exit
fi

seconds=${1:-60}
export -f run_one
export BIN_DIR
printf '%s\n' "${TARGETS[@]}" | xargs -P 4 -I{} bash -c 'run_one "$@"' _ {} "$seconds"
