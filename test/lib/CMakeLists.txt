cmake_minimum_required(VERSION 3.19)

# Create test executable
add_executable(
    tests
    "src/benchmark_json.cpp"
    "src/common.cpp"
    "src/test_json_parser.cpp"
    "src/test_log_configuration.cpp"
    "src/test_log_data.cpp"
    "src/test_log_factory.cpp"
    "src/test_log_file.cpp"
    "src/test_log_line.cpp"
    "src/test_log_processing.cpp"
    "src/test_log_table.cpp")

target_include_directories(tests PRIVATE include)

# Link libraries
target_link_libraries(tests PRIVATE loglib Catch2::Catch2WithMain project_warnings)

# Discover tests
catch_discover_tests(tests)
