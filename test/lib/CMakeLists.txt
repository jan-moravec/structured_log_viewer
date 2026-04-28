cmake_minimum_required(VERSION 3.28)

# Create test executable. Unit tests and benchmarks share a single binary so
# `ctest` can schedule both kinds of cases. Benchmark cases keep their
# Catch2 `[.][benchmark][...]` hidden-by-default tags, so invoking the
# binary directly without a filter still runs only the unit-test subset
# (the previous local-iteration workflow). `catch_discover_tests` below
# enumerates them by exact name regardless, so `ctest` picks them up too —
# under the `benchmark` label, which lets `ctest -LE benchmark` skip them
# when iterating on the unit tests.
add_executable(
    tests
    "src/benchmark_json.cpp"
    "src/common.cpp"
    "src/test_json_parser.cpp"
    "src/test_key_index.cpp"
    "src/test_log_configuration.cpp"
    "src/test_log_data.cpp"
    "src/test_log_factory.cpp"
    "src/test_log_file.cpp"
    "src/test_log_line.cpp"
    "src/test_log_processing.cpp"
    "src/test_log_table.cpp"
    "src/test_parser_pipeline.cpp"
)

target_include_directories(tests PRIVATE include)

# Link libraries. TBB is linked directly so the concurrent KeyIndex test can call
# tbb::parallel_for; loglib itself keeps TBB PRIVATE because no public loglib
# header exposes a TBB type. `test_common` provides the shared `JsonLogLine`
# type aliased as `TestJsonLogFile::Line` plus the `GenerateRandomJsonLogs` /
# `GenerateWideJsonLogs` helpers used by the benchmark TU.
target_link_libraries(
    tests
    PRIVATE loglib test_common Catch2::Catch2WithMain project_warnings TBB::tbb
)

# Enable the KeyIndex call-count instrumentation so unit tests and
# benchmarks can assert that the per-worker key cache eats canonical
# `KeyIndex::GetOrInsert` / `KeyIndex::Find` calls. The macro is off in
# shipped distributions; the in-repo `tests.exe` binary needs both the
# test TU *and* the `loglib` TU that defines the static counter members
# to see the same definition (otherwise we get unresolved-symbol link
# errors). Setting it PUBLIC on the `loglib` target also lets the
# increments fire during benchmark runs — two `memory_order_relaxed`
# atomic increments per call (~1 ns each), negligible for our purposes.
target_compile_definitions(loglib PUBLIC LOGLIB_KEY_INDEX_INSTRUMENTATION)

# On Windows the TBB runtime DLL must sit next to the test executable for both
# `catch_discover_tests` (which runs `tests.exe --list-test-names-only` at build
# time) and `ctest` itself to find it.
if(WIN32)
    add_custom_command(
        TARGET tests
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:TBB::tbb> $<TARGET_FILE_DIR:tests>
        COMMENT "Copying TBB runtime DLL next to tests.exe"
        VERBATIM
    )
endif()

# Discover tests. Two passes so benchmarks land under a CTest label without
# changing test names: pass 1 lists every non-benchmark case, pass 2 lists
# only the `[benchmark]`-tagged cases and tags those CTest entries with the
# `benchmark` label. The two `--list-tests` invocations are disjoint, so no
# CTest entry is registered twice. Users can then opt out of the long-
# running parse fixtures with `ctest -LE benchmark`, or run only those with
# `ctest -L benchmark`.
catch_discover_tests(tests TEST_SPEC "~[benchmark]")
catch_discover_tests(tests TEST_SPEC "[benchmark]" PROPERTIES LABELS "benchmark")
