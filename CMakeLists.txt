cmake_minimum_required(VERSION 3.28)

project(structured_log_viewer VERSION 0.7.0 LANGUAGES CXX)

# IPO/LTO for Release / RelWithDebInfo so the linker can inline across TUs.
# Debug stays off for iteration speed.
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

# Disable the per-TU `clang-scan-deps` pre-pass: we don't use C++20 modules,
# and on Windows clang targeting MSVC it rejects oneTBB's `-fPIC` as a hard
# error (regular compile ignores it).
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/${CMAKE_BUILD_TYPE})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib/${CMAKE_BUILD_TYPE})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib/${CMAKE_BUILD_TYPE})

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(cmake/FetchDependencies.cmake)

include(cmake/CompilerWarnings.cmake)
add_library(project_warnings INTERFACE)
set_project_warnings(project_warnings False)

# Aggregates third-party license texts into a single
# `THIRD_PARTY_LICENSES.txt` next to the binary. `bundle_third_party_licenses`
# is invoked from `app/CMakeLists.txt` once the StructuredLogViewer
# target exists.
include(cmake/BundleLicenses.cmake)

add_subdirectory(library)
add_subdirectory(app)

# `BUILD_TESTING` (from `include(CTest)`, default ON) also scopes
# `LOGLIB_KEY_INDEX_INSTRUMENTATION` from `test/lib` to test builds.
include(CTest)
if(BUILD_TESTING)
    include(Catch)
    # `test/common` ships the helpers used by `test/lib` and `test/log_generator`;
    # must come before `test/lib`.
    add_subdirectory(test/common)
    add_subdirectory(test/lib)
    add_subdirectory(test/app)
    add_subdirectory(test/log_generator)
endif()
