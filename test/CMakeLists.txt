cmake_minimum_required(VERSION 3.19)

project(tests)

# Add test files
file(GLOB TEST_SOURCES "src/*.cpp")

# Create test executable
add_executable(tests ${TEST_SOURCES})

# Link libraries
target_link_libraries(tests PRIVATE loglib Catch2::Catch2WithMain project_warnings)

# Enable CTest
include(CTest)
include(Catch)
catch_discover_tests(tests)
