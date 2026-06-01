cmake_minimum_required(VERSION 3.28)

project(structured_log_viewer VERSION 0.9.0 LANGUAGES CXX)

# IPO/LTO on for Release / RelWithDebInfo, off for Debug. `NOT DEFINED` gates
# let sanitizer presets override via the cache; a plain `set(... ON)` would
# shadow that and re-enable thin-LTO, which breaks ASan+UBSan+PIE links
# (PC-relative relocs into libstdc++ from `log_compare.cpp` thread_local dtors).
#
# Linux+GCC special case: default IPO OFF. GCC 13+ thin-LTO miscompiles
# moc-generated Q_OBJECT staticMetaObject / qt_static_metacall machinery, so
# any Qt-Widgets executable linking our `logapp.a` (transitively or directly)
# SegFaults during QApplication construction when built with IPO ON. The new
# `apptest_theme` unit test is the first thing in our suite that constructs a
# real QApplication, so it tripped the latent bug. Disabling IPO on this
# toolchain is the minimal workaround; a follow-up PR will replace this gate
# with a per-target SHARED-libloglib.so containment that recovers full LTO --
# see doc/linux-gcc-lto-investigation.md (untracked working note, intended to
# be moved into the follow-up PR) for the full bisect + benchmark record.
# macOS (Apple Clang) and Windows (MSVC /GL) are unaffected and keep IPO ON.
# Linux+Clang sanitizer presets already override IPO via the cache.
include(CheckIPOSupported)
check_ipo_supported(RESULT loglib_ipo_supported OUTPUT loglib_ipo_error LANGUAGES CXX)
if(loglib_ipo_supported)
    set(loglib_default_ipo ON)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        set(loglib_default_ipo OFF)
        message(STATUS "IPO/LTO defaulted OFF on Linux+GCC (GCC 13+ moc-LTO miscompile).")
    endif()
    if(NOT DEFINED CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ${loglib_default_ipo})
    endif()
    if(NOT DEFINED CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ${loglib_default_ipo})
    endif()
    message(
        STATUS
        "IPO/LTO Release=${CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE} "
        "RelWithDebInfo=${CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO}"
    )
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
