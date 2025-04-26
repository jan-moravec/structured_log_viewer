cmake_minimum_required(VERSION 3.19)

project(tests)

# Create test executable
add_executable(
    ${PROJECT_NAME}
    "src/common.cpp"
    "src/test_json_parser.cpp"
    "src/test_log_configuration.cpp"
    "src/test_log_data.cpp"
    "src/test_log_factory.cpp"
    "src/test_log_file.cpp"
    "src/test_log_line.cpp"
    "src/test_log_processing.cpp")

target_include_directories(${PROJECT_NAME} PRIVATE include)

# Link libraries
target_link_libraries(${PROJECT_NAME} PRIVATE loglib Catch2::Catch2WithMain project_warnings)

# Discover tests
catch_discover_tests(${PROJECT_NAME})
