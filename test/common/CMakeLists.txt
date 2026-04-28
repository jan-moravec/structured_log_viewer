cmake_minimum_required(VERSION 3.19)

# Shared helpers for test-tree binaries: the JSON log line abstraction and
# the random/wide log generators. Intentionally Catch2-free so the
# standalone `log_generator` console app can link it without pulling in the
# unit-test framework.
add_library(test_common STATIC src/json_log_line.cpp src/log_generator.cpp)

target_include_directories(test_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(test_common PUBLIC glaze::glaze date::date PRIVATE project_warnings)
