cmake_minimum_required(VERSION 3.28)

project(structured_log_viewer VERSION 0.17.0 LANGUAGES CXX)

# IPO/LTO on for Release / RelWithDebInfo, off for Debug. `NOT DEFINED` gates
# let sanitizer presets override via the cache; a plain `set(... ON)` would
# shadow that and re-enable thin-LTO, which breaks ASan+UBSan+PIE links
# (PC-relative relocs into libstdc++ from `log_compare.cpp` thread_local dtors).
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

# Set globally before any target exists so loglib + FetchContent third-party
# static libs (simdjson, fmt, efsw, date-tz, ...) pick up PIC, matching the
# PIC that Qt-linked targets already get via INTERFACE_POSITION_INDEPENDENT_CODE.
# Mixing PIC and non-PIC bitcode in an LTO link triggers LLVM #189203:
# spurious R_X86_64_COPY relocations against Qt's staticMetaObject leave the
# metaobject zero-initialised at runtime -> SIGSEGV in QApplication
# construction (QGuiApplication::screenAdded). No-op on Windows / MSVC.
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

# The GUI and the headless CLI build independently. Both default ON.
# `find_package(Qt6)` stays inside `app/` and `test/app`, so
# `-DSLV_BUILD_GUI=OFF` configures on a machine with no Qt.
option(SLV_BUILD_GUI "Build the StructuredLogViewer GUI" ON)
option(SLV_BUILD_CLI "Build the slp command-line tool" ON)

add_subdirectory(library)
if(SLV_BUILD_GUI)
    add_subdirectory(app)
endif()
if(SLV_BUILD_CLI)
    add_subdirectory(cli)
endif()

# `BUILD_TESTING` (from `include(CTest)`, default ON) also scopes
# `LOGLIB_KEY_INDEX_INSTRUMENTATION` from `test/lib` to test builds.
include(CTest)
if(BUILD_TESTING)
    if(WIN32)
        # Empties the folder that the `local` test preset uses as TEMP before
        # each CTest run. Windows cannot delete a memory-mapped file, so tests
        # that remove a fixture while a window still maps it leave files behind.
        set(SLV_TEST_TMP_DIR "${CMAKE_BINARY_DIR}/test-tmp")
        file(
            WRITE "${CMAKE_BINARY_DIR}/CTestCustom.cmake"
            "set(CTEST_CUSTOM_PRE_TEST\n"
            "    \"\\\"${CMAKE_COMMAND}\\\" -E rm -rf \\\"${SLV_TEST_TMP_DIR}\\\"\"\n"
            "    \"\\\"${CMAKE_COMMAND}\\\" -E make_directory \\\"${SLV_TEST_TMP_DIR}\\\"\")\n"
        )
    endif()
    include(Catch)
    # `test/common` ships the helpers used by `test/lib` and `test/log_generator`;
    # must come before `test/lib`.
    add_subdirectory(test/common)
    add_subdirectory(test/lib)
    if(SLV_BUILD_GUI)
        add_subdirectory(test/app)
    endif()
    if(SLV_BUILD_CLI)
        add_subdirectory(test/cli)
    endif()
    add_subdirectory(test/log_generator)
endif()
