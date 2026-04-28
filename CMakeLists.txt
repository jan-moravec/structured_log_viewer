cmake_minimum_required(VERSION 3.28)

project(structured_log_viewer VERSION 0.6.0 LANGUAGES CXX)

# Enable interprocedural optimisation (LTO/IPO) for Release and RelWithDebInfo
# so the linker can inline across translation-unit boundaries (notably simdjson
# value visitors, `tsl::robin_map` probing, and the `LogValue` variant
# visitors). Debug intentionally stays off for fast iteration. When
# `check_ipo_supported` reports a per-platform failure the `else` branch leaves
# the build unchanged so CI jobs that lack a compatible toolchain still succeed
# without IPO.
include(CheckIPOSupported)
check_ipo_supported(RESULT loglib_ipo_supported OUTPUT loglib_ipo_error LANGUAGES CXX)
if(loglib_ipo_supported)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
    message(STATUS "IPO/LTO enabled for Release and RelWithDebInfo")
else()
    message(STATUS "IPO/LTO not supported: ${loglib_ipo_error}")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_COLOR_DIAGNOSTICS ON)

# We do not use C++20 modules. Disable CMake's per-source `clang-scan-deps`
# pre-pass globally — it is otherwise invoked on every C++ TU, and on Windows
# clang targeting `x86_64-pc-windows-msvc` it rejects oneTBB's hardening
# `-fPIC` flag as a hard error (the regular compile silently ignores it). The
# pre-pass is pure overhead for non-modules code paths.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# Configure the output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/${CMAKE_BUILD_TYPE})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib/${CMAKE_BUILD_TYPE})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib/${CMAKE_BUILD_TYPE})

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Download and use dependencies if required
include(cmake/FetchDependencies.cmake)

include(cmake/CompilerWarnings.cmake)
add_library(project_warnings INTERFACE)
set_project_warnings(project_warnings False)

# Add subdirectories
add_subdirectory(library)
add_subdirectory(app)

include(CTest)
include(Catch)
# `test/common` ships the shared, Catch2-free helpers (JSON line abstraction +
# random/wide log generators) consumed by both `test/lib`'s benchmarks and
# the standalone `test/log_generator` console app. Must come before
# `test/lib` so the `test_common` target is available there.
add_subdirectory(test/common)
add_subdirectory(test/lib)
add_subdirectory(test/app)
add_subdirectory(test/log_generator)
