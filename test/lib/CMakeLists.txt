cmake_minimum_required(VERSION 3.19)

# Create test executable
add_executable(
    tests
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
)

target_include_directories(tests PRIVATE include)

# Link libraries. TBB is linked directly so the concurrent KeyIndex test can call
# tbb::parallel_for; loglib itself keeps TBB PRIVATE because no public loglib
# header exposes a TBB type.
target_link_libraries(tests PRIVATE loglib Catch2::Catch2WithMain project_warnings TBB::tbb)

# Enable the KeyIndex call-count instrumentation surface for unit tests so they
# can assert that the per-worker key cache (PRD §4.1) eats canonical
# `KeyIndex::GetOrInsert` / `KeyIndex::Find` calls. The macro is intentionally
# off in shipped distributions (PRD req. 4.1.8a) — but the in-repo `tests.exe`
# binary needs both the test TU *and* the `loglib` translation unit that
# defines the static counter members to see the same definition, otherwise we
# get unresolved-symbol link errors. Setting the macro PUBLIC on the `loglib`
# target also lets the per-`KeyIndex::GetOrInsert` increment fire so the
# counters reflect real call counts during benchmark runs. The cost is two
# `std::memory_order_relaxed` atomic increments per call (~1 ns each), well
# below the PRD's ±3 % no-regression band.
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

# Discover tests
catch_discover_tests(tests)

# PRD hl-inspired §7.4 — benchmarks live in their own executable so they are
# never picked up by the unit-test runner. The `tests` target finishes in a
# couple of seconds; pulling the long-running parse fixtures (`[large]`,
# `[wide]`, `[stream_to_table]`, `[cancellation]`) into the same binary makes
# `tests.exe` filter passes (e.g. `~[.]`) hang in process-global state long
# enough to look like a deadlock. Splitting them out also lets the benchmark
# binary opt out of `catch_discover_tests` so CTest never schedules them.
add_executable(
    benchmarks
    "src/benchmark_json.cpp"
    "src/common.cpp"
)

target_include_directories(benchmarks PRIVATE include)
target_link_libraries(benchmarks PRIVATE loglib Catch2::Catch2WithMain project_warnings TBB::tbb)

if(WIN32)
    add_custom_command(
        TARGET benchmarks
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:TBB::tbb> $<TARGET_FILE_DIR:benchmarks>
        COMMENT "Copying TBB runtime DLL next to benchmarks.exe"
        VERBATIM
    )
endif()
