include(FetchContent)

# Option to use system libraries
option(USE_SYSTEM_DATE "Use system HowardHinnant date library" OFF)
option(USE_SYSTEM_FMT "Use system fmtlib fmt library" OFF)
option(USE_SYSTEM_CATCH2 "Use system catchorg Catch2 library" OFF)
option(USE_SYSTEM_MIO "Use system mandreyel mio library" OFF)
option(USE_SYSTEM_GLAZE "Use system stephenberry Glaze library" OFF)
option(USE_SYSTEM_SIMDJSON "Use system simdjson library" OFF)
option(USE_SYSTEM_ROBIN_MAP "Use system Tessil robin-map library" OFF)
option(USE_SYSTEM_TBB "Use system uxlfoundation oneTBB library" OFF)
option(USE_SYSTEM_ARGPARSE "Use system p-ranav/argparse library" OFF)
option(USE_SYSTEM_EFSW "Use system SpartanJ/efsw library" OFF)
option(USE_SYSTEM_PCRE2 "Use system PCRE2 library" OFF)
option(USE_SYSTEM_ZLIB "Use system zlib library" OFF)
option(USE_SYSTEM_BZIP2 "Use system bzip2 library" OFF)
option(USE_SYSTEM_XZ "Use system xz/liblzma library" OFF)
option(USE_SYSTEM_ZSTD "Use system zstd library" OFF)

if(NOT USE_SYSTEM_DATE)
    FetchContent_Declare(
        date
        GIT_REPOSITORY https://github.com/HowardHinnant/date.git
        GIT_TAG v3.0.4
        SYSTEM
        EXCLUDE_FROM_ALL
    )
    block()
        set(BUILD_TZ_LIB ON) # Enable building the time zone library
        set(MANUAL_TZ_DB ON) # Provide time zone database manually
        set(CMAKE_POLICY_DEFAULT_CMP0069 NEW) # honour our IPO/LTO request on date-tz
        set(CMAKE_POLICY_VERSION_MINIMUM 3.28) # silence date's `cmake_minimum_required(VERSION 3.7)` deprecation
        FetchContent_MakeAvailable(date)
    endblock()

    # Stage tzdata and windowsZones.xml next to the binary so the date library
    # can locate them at runtime via MANUAL_TZ_DB.
    set(TZDATA tzdata)
    set(TZDATA_DIR ${CMAKE_BINARY_DIR}/${TZDATA})
    set(TZDATA_FILE tzdata.tar.gz)
    set(WINDOWS_ZONES_FILE windowsZones.xml)

    file(MAKE_DIRECTORY ${TZDATA_DIR})

    if(NOT EXISTS ${TZDATA_DIR}/${WINDOWS_ZONES_FILE})
        file(
            DOWNLOAD https://raw.githubusercontent.com/unicode-org/cldr/master/common/supplemental/windowsZones.xml
            ${TZDATA_DIR}/${WINDOWS_ZONES_FILE}
            SHOW_PROGRESS
        )
    endif()

    if(NOT EXISTS ${CMAKE_BINARY_DIR}/${TZDATA_FILE})
        file(
            DOWNLOAD https://www.iana.org/time-zones/repository/tzdata-latest.tar.gz
            ${CMAKE_BINARY_DIR}/${TZDATA_FILE}
            SHOW_PROGRESS
        )
        execute_process(
            COMMAND ${CMAKE_COMMAND} -E tar xzf ${CMAKE_BINARY_DIR}/${TZDATA_FILE}
            WORKING_DIRECTORY ${TZDATA_DIR}
            COMMAND_ERROR_IS_FATAL ANY
        )
    endif()

    execute_process(
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${TZDATA_DIR} ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${TZDATA}
        COMMAND_ERROR_IS_FATAL ANY
    )
else()
    find_package(date REQUIRED)
endif()

if(NOT USE_SYSTEM_FMT)
    FetchContent_Declare(fmt GIT_REPOSITORY https://github.com/fmtlib/fmt.git GIT_TAG 12.1.0 SYSTEM EXCLUDE_FROM_ALL)
    FetchContent_MakeAvailable(fmt)

    # silence MSVC C4834 (discarded [[nodiscard]] in fmt/base.h) when fmt compiles itself
    if(MSVC)
        target_compile_options(fmt PRIVATE /wd4834)
    endif()
else()
    find_package(fmt REQUIRED)
endif()

if(NOT USE_SYSTEM_CATCH2)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.14.0
        SYSTEM
        EXCLUDE_FROM_ALL
    )
    FetchContent_MakeAvailable(Catch2)
else()
    find_package(Catch2 REQUIRED)
endif()

if(NOT USE_SYSTEM_MIO)
    FetchContent_Declare(
        mio
        GIT_REPOSITORY https://github.com/mandreyel/mio.git
        GIT_TAG 8b6b7d8
        SYSTEM
        EXCLUDE_FROM_ALL
    )
    block()
        set(CMAKE_POLICY_VERSION_MINIMUM 3.28) # silence mio's `cmake_minimum_required(VERSION 3.0)` deprecation
        FetchContent_MakeAvailable(mio)
    endblock()
else()
    find_package(mio REQUIRED)
endif()

if(NOT USE_SYSTEM_GLAZE)
    FetchContent_Declare(
        glaze
        GIT_REPOSITORY https://github.com/stephenberry/glaze.git
        GIT_TAG v7.4.0
        SYSTEM
        EXCLUDE_FROM_ALL
    )
    FetchContent_MakeAvailable(glaze)
else()
    find_package(glaze REQUIRED)
endif()

if(NOT USE_SYSTEM_SIMDJSON)
    FetchContent_Declare(
        simdjson
        GIT_REPOSITORY https://github.com/simdjson/simdjson.git
        GIT_TAG v4.6.3
        SYSTEM
        EXCLUDE_FROM_ALL
    )
    block()
        set(BUILD_SHARED_LIBS OFF) # static link; replaces deprecated SIMDJSON_BUILD_STATIC
        set(SIMDJSON_ENABLE_THREADS ON)
        FetchContent_MakeAvailable(simdjson)
    endblock()

    # simdjson's PCH includes <bit> (C++20); bump to C++20 to avoid MSVC STL4038
    set_target_properties(simdjson PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
else()
    find_package(simdjson REQUIRED)
endif()

if(NOT USE_SYSTEM_ROBIN_MAP)
    FetchContent_Declare(
        robin_map
        GIT_REPOSITORY https://github.com/Tessil/robin-map.git
        GIT_TAG v1.4.1
        SYSTEM
        EXCLUDE_FROM_ALL
    )
    FetchContent_MakeAvailable(robin_map)
else()
    find_package(robin_map REQUIRED)
endif()

if(NOT USE_SYSTEM_TBB)
    FetchContent_Declare(
        tbb
        GIT_REPOSITORY https://github.com/uxlfoundation/oneTBB.git
        GIT_TAG v2022.3.0
        SYSTEM
        EXCLUDE_FROM_ALL
    )
    block()
        set(TBB_TEST OFF)
        set(TBB_EXAMPLES OFF)
        set(TBB_STRICT OFF) # don't let oneTBB's -Werror gate our build
        set(BUILD_SHARED_LIBS ON) # parallel_pipeline relies on dynamic library state; static is unsupported upstream
        set(TBB_VERIFY_DEPENDENCY_SIGNATURE OFF) # explicit value silences TBB's "disabled by default" WARNING
        set(CMAKE_WARN_DEPRECATED OFF) # silence TBB's `cmake_policy(SET CMP0148 OLD)` deprecation
        set(CMAKE_MESSAGE_LOG_LEVEL NOTICE) # hide chatty "HWLOC target ... doesn't exist" STATUS noise
        FetchContent_MakeAvailable(tbb)
    endblock()
else()
    # 2021.5 is the first release with the oneAPI-style tbb::filter_mode / tbb::parallel_pipeline API
    find_package(TBB 2021.5 REQUIRED)
endif()

# argparse is only needed by the test/log_generator console helper (header-only)
if(NOT USE_SYSTEM_ARGPARSE)
    FetchContent_Declare(
        argparse
        GIT_REPOSITORY https://github.com/p-ranav/argparse.git
        GIT_TAG v3.1
        SYSTEM
        EXCLUDE_FROM_ALL
    )
    FetchContent_MakeAvailable(argparse)
else()
    find_package(argparse REQUIRED)
endif()

# efsw provides the cross-platform filesystem watcher used by the live tail
# (TailingBytesProducer) — native backends (ReadDirectoryChangesW / inotify /
# FSEvents) plus its own polling fallback. We always link the static library
# variant (efsw-static) so on Windows MSVC there is no extra runtime DLL to
# stage next to the executable and the /MD vs /MT runtime-library setting
# matches the rest of the project.
if(NOT USE_SYSTEM_EFSW)
    FetchContent_Declare(efsw GIT_REPOSITORY https://github.com/SpartanJ/efsw.git GIT_TAG 1.6.1 SYSTEM EXCLUDE_FROM_ALL)
    block()
        # With BUILD_SHARED_LIBS=OFF, the bare `efsw` target is itself a
        # static library (efsw's CMakeLists uses `add_library(efsw)` without
        # an explicit type, which honours BUILD_SHARED_LIBS). We disable the
        # parallel `efsw-static` target to avoid building the same TU twice.
        # BUILD_TEST_APP / EFSW_INSTALL default to OFF when efsw is consumed
        # as a sub-project, but we set them explicitly for clarity.
        set(BUILD_SHARED_LIBS OFF)
        set(BUILD_STATIC_LIBS OFF)
        set(BUILD_TEST_APP OFF)
        set(EFSW_INSTALL OFF)
        set(VERBOSE OFF)
        set(CMAKE_POLICY_VERSION_MINIMUM 3.28) # silence efsw's older cmake_minimum_required
        FetchContent_MakeAvailable(efsw)
    endblock()
    # Silence third-party warnings. efsw 1.6.1 emits -Wunused-result.
    if(TARGET efsw)
        if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
            target_compile_options(efsw PRIVATE /w)
        else()
            target_compile_options(efsw PRIVATE -w)
        endif()
    endif()
else()
    find_package(efsw REQUIRED)
endif()

# PCRE2 (8-bit + JIT) backs the regex-template log parser
# (`loglib::RegexParser`). Per the engine evaluation in
# CONTRIBUTING.md, PCRE2's JIT outperforms RE2 on capture-heavy
# patterns (rebar geomean) and runs upstream lnav / logstash-grok
# patterns verbatim. We build the 8-bit variant only (we never
# feed UTF-16 / UTF-32 to the parser), with JIT enabled; upstream
# tests / pcre2grep are disabled to keep configure time short.
# `PCRE2_STATIC_PIC=ON` matches the project-wide
# `CMAKE_POSITION_INDEPENDENT_CODE=ON`; otherwise linking `loglib`
# into the PIC executable fails on Linux. The compiled
# `pcre2_code*` is shared read-only across Stage B workers, each
# owning its own `pcre2_match_data*` (see
# `library/src/parsers/regex_parser.cpp`).
if(NOT USE_SYSTEM_PCRE2)
    FetchContent_Declare(
        pcre2
        GIT_REPOSITORY https://github.com/PCRE2Project/pcre2.git
        GIT_TAG pcre2-10.45
        SYSTEM
        EXCLUDE_FROM_ALL
    )
    block()
        set(PCRE2_BUILD_PCRE2_8 ON)
        set(PCRE2_BUILD_PCRE2_16 OFF)
        set(PCRE2_BUILD_PCRE2_32 OFF)
        set(PCRE2_SUPPORT_JIT ON)
        set(PCRE2_BUILD_TESTS OFF)
        set(PCRE2_BUILD_PCRE2GREP OFF)
        set(PCRE2_SHOW_REPORT OFF)
        set(BUILD_SHARED_LIBS OFF) # static link; no extra DLL to stage next to the GUI
        set(PCRE2_STATIC_PIC ON)
        set(CMAKE_POLICY_VERSION_MINIMUM 3.28) # silence pcre2's older cmake_minimum_required
        FetchContent_MakeAvailable(pcre2)
    endblock()

    # PCRE2 names its static 8-bit target `pcre2-8-static`. Alias it
    # under a stable namespaced name so the consumer link line reads
    # the same regardless of whether the dep came from FetchContent
    # or `find_package`.
    if(NOT TARGET PCRE2::pcre2-8 AND TARGET pcre2-8-static)
        add_library(PCRE2::pcre2-8 ALIAS pcre2-8-static)
    endif()

    # PCRE2's CMake emits a few warnings under MSVC and Clang (signed/
    # unsigned mismatches, deprecated decls). Silence them so the
    # third-party build doesn't dominate the configure log.
    if(TARGET pcre2-8-static)
        if(MSVC)
            target_compile_options(pcre2-8-static PRIVATE /w)
        else()
            target_compile_options(pcre2-8-static PRIVATE -w)
        endif()
    endif()
else()
    find_package(PCRE2 REQUIRED COMPONENTS 8BIT)
    # Normalise the consumer-side target name to `PCRE2::pcre2-8`
    # whether the system package exposes it under that name or as
    # a non-namespaced `pcre2-8` / `PCRE2::8BIT`.
    if(NOT TARGET PCRE2::pcre2-8)
        if(TARGET PCRE2::8BIT)
            add_library(PCRE2::pcre2-8 ALIAS PCRE2::8BIT)
        elseif(TARGET pcre2-8)
            add_library(PCRE2::pcre2-8 ALIAS pcre2-8)
        endif()
    endif()
endif()

# Standalone Asio header-only build, used by `TcpServerProducer` /
# `UdpServerProducer` and the `test_common::TcpLogClient` /
# `UdpLogClient` helpers. Boost.Asio is intentionally *not* used: we
# avoid a transitive Boost dependency. `ASIO_STANDALONE` selects the
# standalone path; `ASIO_NO_DEPRECATED` keeps us off the legacy API
# surface so future Asio bumps are less disruptive.
option(USE_SYSTEM_ASIO "Use system chriskohlhoff/asio library" OFF)
if(NOT USE_SYSTEM_ASIO)
    FetchContent_Declare(
        asio
        GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
        GIT_TAG asio-1-30-2
        SYSTEM
        EXCLUDE_FROM_ALL
    )
    FetchContent_MakeAvailable(asio)

    # Asio's CMakeLists is non-canonical; expose an INTERFACE target
    # ourselves rather than relying on `asio::asio` from the upstream
    # CMake (which only exists in select forks).
    if(NOT TARGET asio::asio)
        add_library(asio_headers INTERFACE)
        target_include_directories(asio_headers SYSTEM INTERFACE ${asio_SOURCE_DIR}/asio/include)
        target_compile_definitions(asio_headers INTERFACE ASIO_STANDALONE ASIO_NO_DEPRECATED)
        # POSIX hosts need pthreads; Windows links ws2_32 / mswsock implicitly via Asio headers.
        if(UNIX)
            find_package(Threads REQUIRED)
            target_link_libraries(asio_headers INTERFACE Threads::Threads)
        endif()
        if(WIN32)
            # Pin to Windows 10 (0x0A00). Without this Asio's detail
            # headers print "Please define _WIN32_WINNT" and assume
            # Windows 7, which loses access to a few modern winsock
            # APIs the SSL paths can use.
            target_compile_definitions(asio_headers INTERFACE _WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN)
        endif()
        add_library(asio::asio ALIAS asio_headers)
    endif()
else()
    find_package(asio REQUIRED)
endif()

# Compression codecs used by `loglib::internal::DecompressingByteSource`
# for transparent decompression of `.gz` / `.bz2` / `.xz` / `.zst` in the
# static open path. Each codec is linked statically (`BUILD_SHARED_LIBS
# OFF`) so no runtime DLL is staged next to the executable on Windows.
# All four are PRIVATE deps of `loglib`; no public header exposes any
# codec type (all codec-specific state lives inside the .cpp).
if(NOT USE_SYSTEM_ZLIB)
    FetchContent_Declare(
        zlib
        GIT_REPOSITORY https://github.com/madler/zlib.git
        GIT_TAG v1.3.1
        SYSTEM
        EXCLUDE_FROM_ALL
    )
    block()
        set(ZLIB_BUILD_EXAMPLES OFF)
        set(BUILD_SHARED_LIBS OFF)
        set(CMAKE_POLICY_VERSION_MINIMUM 3.28) # silence zlib's older cmake_minimum_required
        FetchContent_MakeAvailable(zlib)
    endblock()
    # zlib exports a `zlibstatic` target and (if BUILD_SHARED_LIBS was
    # somehow flipped by another dep) a `zlib` shared target too. We
    # always consume the static one. Normalise to `ZLIB::ZLIB` so the
    # consumer link line reads the same whether we fetched or found.
    if(NOT TARGET ZLIB::ZLIB AND TARGET zlibstatic)
        add_library(ZLIB::ZLIB ALIAS zlibstatic)
    endif()
    if(TARGET zlibstatic)
        if(MSVC)
            target_compile_options(zlibstatic PRIVATE /w)
        else()
            target_compile_options(zlibstatic PRIVATE -w)
        endif()
        # zlib's public headers live in the source and build dirs.
        target_include_directories(
            zlibstatic INTERFACE $<BUILD_INTERFACE:${zlib_SOURCE_DIR}> $<BUILD_INTERFACE:${zlib_BINARY_DIR}>
        )
    endif()
else()
    find_package(ZLIB REQUIRED)
endif()

if(NOT USE_SYSTEM_BZIP2)
    # Upstream bzip2 is distributed as raw .c sources with a hand-rolled
    # Makefile — no CMakeLists. We fetch the libarchive mirror (which is
    # a straight source copy of the sourceware release) and compile the
    # canonical 7-file library manually into a static target. Same shape
    # `find_package(BZip2)` produces so the consumer link line matches.
    FetchContent_Declare(
        bzip2
        GIT_REPOSITORY https://github.com/libarchive/bzip2.git
        GIT_TAG bzip2-1.0.8
        SYSTEM
        EXCLUDE_FROM_ALL
    )
    FetchContent_MakeAvailable(bzip2)

    # The top-level project() only enables CXX; enable C now so the
    # bzip2 .c files can be compiled. Idempotent when called twice.
    enable_language(C)

    add_library(
        bz2_static STATIC
        ${bzip2_SOURCE_DIR}/blocksort.c
        ${bzip2_SOURCE_DIR}/huffman.c
        ${bzip2_SOURCE_DIR}/crctable.c
        ${bzip2_SOURCE_DIR}/randtable.c
        ${bzip2_SOURCE_DIR}/compress.c
        ${bzip2_SOURCE_DIR}/decompress.c
        ${bzip2_SOURCE_DIR}/bzlib.c
    )
    target_include_directories(bz2_static SYSTEM PUBLIC $<BUILD_INTERFACE:${bzip2_SOURCE_DIR}>)
    set_target_properties(bz2_static PROPERTIES POSITION_INDEPENDENT_CODE ON)
    if(MSVC)
        target_compile_options(bz2_static PRIVATE /w)
        # BZ_UNIX is picked automatically on POSIX via bzlib_private.h's
        # `unix` macro; on Windows we need to disable UNIX quirks.
        target_compile_definitions(bz2_static PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(bz2_static PRIVATE -w)
    endif()

    if(NOT TARGET BZip2::BZip2)
        add_library(BZip2::BZip2 ALIAS bz2_static)
    endif()
else()
    find_package(BZip2 REQUIRED)
endif()

if(NOT USE_SYSTEM_XZ)
    # SECURITY: do NOT downgrade below v5.6.2. Versions 5.6.0 and
    # 5.6.1 shipped the CVE-2024-3094 "xz-utils" build-system
    # backdoor (a `liblzma` payload activated when the library was
    # loaded into `sshd`'s dependency chain, via `ifunc` hijacking);
    # v5.6.2 was the first upstream release with the backdoor
    # artefacts + infrastructure fully purged. Any dependabot /
    # manual bump that lowers this pin must be treated as a
    # supply-chain regression, not a routine downgrade.
    FetchContent_Declare(
        xz
        GIT_REPOSITORY https://github.com/tukaani-project/xz.git
        GIT_TAG v5.6.3
        SYSTEM
        EXCLUDE_FROM_ALL
    )
    block()
        set(BUILD_SHARED_LIBS OFF)
        set(XZ_TOOL_XZ OFF)
        set(XZ_TOOL_XZDEC OFF)
        set(XZ_TOOL_LZMADEC OFF)
        set(XZ_TOOL_LZMAINFO OFF)
        set(XZ_TOOL_SCRIPTS OFF)
        set(XZ_DOC OFF)
        set(XZ_MICROLZMA_ENCODER OFF)
        set(XZ_MICROLZMA_DECODER ON)
        set(XZ_LZIP_DECODER ON)
        set(CMAKE_POLICY_VERSION_MINIMUM 3.28) # silence xz's older cmake_minimum_required
        FetchContent_MakeAvailable(xz)
    endblock()
    # xz exposes the C library as `liblzma`. Normalise to `LibLZMA::LibLZMA`
    # so the consumer link line matches the `find_package(LibLZMA)` naming.
    # xz's CMakeLists doesn't set INTERFACE_INCLUDE_DIRECTORIES for external
    # consumers — its own build uses `-I` flags baked into add_library — so
    # we attach the public API dir ourselves.
    if(TARGET liblzma)
        target_include_directories(liblzma SYSTEM INTERFACE $<BUILD_INTERFACE:${xz_SOURCE_DIR}/src/liblzma/api>)
        if(MSVC)
            target_compile_options(liblzma PRIVATE /w)
        else()
            target_compile_options(liblzma PRIVATE -w)
        endif()
    endif()
    if(NOT TARGET LibLZMA::LibLZMA AND TARGET liblzma)
        add_library(LibLZMA::LibLZMA ALIAS liblzma)
    endif()
else()
    find_package(LibLZMA REQUIRED)
endif()

if(NOT USE_SYSTEM_ZSTD)
    FetchContent_Declare(
        zstd
        GIT_REPOSITORY https://github.com/facebook/zstd.git
        GIT_TAG v1.5.6
        SYSTEM
        EXCLUDE_FROM_ALL
        SOURCE_SUBDIR build/cmake
    )
    block()
        set(ZSTD_BUILD_PROGRAMS OFF)
        set(ZSTD_BUILD_SHARED OFF)
        set(ZSTD_BUILD_STATIC ON)
        set(ZSTD_BUILD_TESTS OFF)
        set(ZSTD_BUILD_CONTRIB OFF)
        set(ZSTD_MULTITHREAD_SUPPORT OFF)
        set(ZSTD_LEGACY_SUPPORT OFF)
        set(BUILD_SHARED_LIBS OFF)
        set(CMAKE_POLICY_VERSION_MINIMUM 3.28) # silence zstd's older cmake_minimum_required
        FetchContent_MakeAvailable(zstd)
    endblock()
    # Upstream exposes the static library as `libzstd_static`. Alias to
    # `zstd::libzstd` so the consumer link line matches the pkg-config
    # target the system find_package produces.
    if(NOT TARGET zstd::libzstd)
        if(TARGET libzstd_static)
            add_library(zstd::libzstd ALIAS libzstd_static)
        elseif(TARGET zstd_static)
            add_library(zstd::libzstd ALIAS zstd_static)
        endif()
    endif()
    foreach(_zstd_target libzstd_static zstd_static)
        if(TARGET ${_zstd_target})
            if(MSVC)
                target_compile_options(${_zstd_target} PRIVATE /w)
            else()
                target_compile_options(${_zstd_target} PRIVATE -w)
            endif()
        endif()
    endforeach()
else()
    find_package(zstd REQUIRED)
    # zstd's CMake config exports `zstd::libzstd_static` and
    # `zstd::libzstd_shared` in modern releases; older ones only expose
    # `zstd::libzstd`. Normalise.
    if(NOT TARGET zstd::libzstd)
        if(TARGET zstd::libzstd_static)
            add_library(zstd::libzstd ALIAS zstd::libzstd_static)
        elseif(TARGET zstd::libzstd_shared)
            add_library(zstd::libzstd ALIAS zstd::libzstd_shared)
        endif()
    endif()
endif()

# TLS support for `TcpServerProducer`. Default ON so packaged binaries
# (Linux/Windows/macOS) ship working TLS; developers iterating on
# non-network code can pass `-DLOGLIB_NETWORK_TLS=OFF` to skip the
# OpenSSL dependency entirely. We use the system OpenSSL via
# `find_package(OpenSSL REQUIRED)` rather than fetching/building
# OpenSSL from source -- the build cost (5-15 min on first configure)
# and the wrapper-maintenance risk outweighed the convenience for a
# single dep. CONTRIBUTING.md documents the per-platform install
# command (apt / dnf / brew / winget / vcpkg).
option(LOGLIB_NETWORK_TLS "Build TLS support for TcpServerProducer (requires system OpenSSL)" ON)
if(LOGLIB_NETWORK_TLS)
    find_package(OpenSSL REQUIRED)
    message(STATUS "TLS support enabled (OpenSSL ${OPENSSL_VERSION} from ${OPENSSL_INCLUDE_DIR})")
else()
    message(STATUS "TLS support disabled (LOGLIB_NETWORK_TLS=OFF) -- TcpServerProducer will be plaintext-only")
endif()
