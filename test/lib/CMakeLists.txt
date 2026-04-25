cmake_minimum_required(VERSION 3.19)

# Create test executable
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
)

target_include_directories(tests PRIVATE include)

# Link libraries. TBB is linked directly so the concurrent KeyIndex test can call
# tbb::parallel_for; loglib itself keeps TBB PRIVATE because no public loglib
# header exposes a TBB type.
target_link_libraries(tests PRIVATE loglib Catch2::Catch2WithMain project_warnings TBB::tbb)

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
