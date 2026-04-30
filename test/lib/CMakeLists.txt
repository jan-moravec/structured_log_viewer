cmake_minimum_required(VERSION 3.28)

# Tests + benchmarks share one binary; benchmarks land under the `benchmark`
# CTest label (see `catch_discover_tests` below).
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
    "src/test_log_source.cpp"
    "src/test_log_table.cpp"
    "src/test_parser_pipeline.cpp"
)

target_include_directories(tests PRIVATE include)

# TBB linked directly here for the concurrent KeyIndex test; loglib keeps it PRIVATE.
target_link_libraries(
    tests
    PRIVATE loglib test_common Catch2::Catch2WithMain project_warnings TBB::tbb
)

# `benchmark_json.cpp` calls `GetProcessMemoryInfo` to report peak working
# set; on Windows that lives in `psapi.lib` (Windows 7+ also re-exports it
# from `kernel32.dll`, but linking the import library is the portable form
# across SDK versions).
if(WIN32)
    target_link_libraries(tests PRIVATE Psapi)
endif()

# Enable KeyIndex call-count instrumentation; PUBLIC so the test TU and
# `loglib` see the same static counter definitions. Gated by `BUILD_TESTING`
# at the top level, so `-DBUILD_TESTING=OFF` strips it from shipped builds.
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

# Two disjoint discovery passes so benchmark cases land under the `benchmark`
# CTest label without renaming. Use `ctest -LE benchmark` / `-L benchmark`
# to filter.
catch_discover_tests(tests TEST_SPEC "~[benchmark]")
catch_discover_tests(tests TEST_SPEC "[benchmark]" PROPERTIES LABELS "benchmark")
