cmake_minimum_required(VERSION 3.28)

project(structured_log_viewer VERSION 0.9.0 LANGUAGES CXX)

# IPO/LTO on for Release / RelWithDebInfo, off for Debug. `NOT DEFINED` gates
# let sanitizer presets override via the cache; a plain `set(... ON)` would
# shadow that and re-enable thin-LTO, which breaks ASan+UBSan+PIE links
# (PC-relative relocs into libstdc++ from `log_compare.cpp` thread_local dtors).
# This gate previously also defaulted OFF on Linux+GCC because IPO+STATIC was
# reproducing the LLVM-189203 PIC/non-PIC LTO copy-relocation bug against Qt's
# staticMetaObject (apptest_theme SegFault during QApplication construction);
# the global CMAKE_POSITION_INDEPENDENT_CODE=ON set below (before any target
# is created via FetchContent / add_subdirectory) eliminates that bug at the
# source by ensuring no PIC/non-PIC mix ever reaches the LTO link. See
# doc/linux-gcc-lto-investigation.md section 11 for the full evidence.
include(CheckIPOSupported)
check_ipo_supported(RESULT loglib_ipo_supported OUTPUT loglib_ipo_error LANGUAGES CXX)
if(loglib_ipo_supported)
    if(NOT DEFINED CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    endif()
    if(NOT DEFINED CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
    endif()
    message(
        STATUS
        "IPO/LTO Release=${CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE} "
        "RelWithDebInfo=${CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO}"
    )
else()
    message(STATUS "IPO/LTO not supported: ${loglib_ipo_error}")
endif()

# Force every static input -- loglib (no Qt dep, so CMake doesn't auto-pick
# PIC), every FetchContent third-party static lib (simdjson, fmt, efsw,
# date-tz, ...), every test executable -- into the same PIC class as the
# Qt-linked logapp / StructuredLogViewer / apptest_* targets that already get
# PIC via Qt's INTERFACE_POSITION_INDEPENDENT_CODE. Mixing PIC and non-PIC
# inputs into an LTO link triggers the toolchain-wide LLVM-189203 /
# Arch FS#78006 defect where R_X86_64_COPY is wrongly emitted for data symbols
# referenced from const globals -- Qt's staticMetaObject / qt_meta_data_*
# tables are exactly such symbols, so the resulting binary has zero-initialised
# metaobjects and SegFaults during QApplication construction in
# QGuiApplication::screenAdded. Setting this BEFORE the FetchContent / library
# / app subdirectories so every target picks up the property at creation time.
# No-op on Windows (PE has no copy-relocations) and MSVC silently ignores the
# property; default on macOS shared libraries; ~1 % cost on Linux x86-64
# static deps, repaid many times over by the restored cross-TU LTO inlining
# inside loglib. See doc/linux-gcc-lto-investigation.md section 11.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

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
