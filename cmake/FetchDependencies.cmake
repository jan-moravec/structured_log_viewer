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
