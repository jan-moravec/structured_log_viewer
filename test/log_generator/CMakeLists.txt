cmake_minimum_required(VERSION 3.28)

# Standalone multi-format log generator. Lives in the test tree because its
# only consumers are local fixture generation and manual Stream Mode smoke
# tests; not run by CTest. Everything except `main.cpp` is a library so the
# unit tests in `test/lib` can cover option parsing, outputs, and rotation.
add_library(
    log_generator_core
    STATIC
    src/formats.cpp
    src/generator.cpp
    src/options.cpp
    src/pacer.cpp
    src/rotation.cpp
    src/sinks.cpp
    src/target.cpp
)
target_include_directories(log_generator_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(log_generator_core PUBLIC test_common CLI11::CLI11 PRIVATE project_warnings)

add_executable(log_generator src/main.cpp)
target_link_libraries(log_generator PRIVATE log_generator_core project_warnings)
