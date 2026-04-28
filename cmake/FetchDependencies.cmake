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
        FetchContent_MakeAvailable(date)
    endblock()

    # Directories to store tzdata and windowsZones.xml in
    set(TZDATA tzdata)
    set(TZDATA_DIR ${CMAKE_BINARY_DIR}/${TZDATA})
    set(TZDATA_FILE tzdata.tar.gz)
    set(WINDOWS_ZONES_FILE windowsZones.xml)

    # Create the directories if they don't exist
    file(MAKE_DIRECTORY ${TZDATA_DIR})

    # Download the latest windowsZones.xml
    if(NOT EXISTS ${TZDATA_DIR}/${WINDOWS_ZONES_FILE})
        file(
            DOWNLOAD https://raw.githubusercontent.com/unicode-org/cldr/master/common/supplemental/windowsZones.xml
            ${TZDATA_DIR}/${WINDOWS_ZONES_FILE}
            SHOW_PROGRESS
        )
    endif()

    # Download the latest tzdata
    if(NOT EXISTS ${CMAKE_BINARY_DIR}/${TZDATA_FILE})
        file(
            DOWNLOAD https://www.iana.org/time-zones/repository/tzdata-latest.tar.gz
            ${CMAKE_BINARY_DIR}/${TZDATA_FILE}
            SHOW_PROGRESS
        )
        # Extract the latest tzdata
        execute_process(
            COMMAND ${CMAKE_COMMAND} -E tar xzf ${CMAKE_BINARY_DIR}/${TZDATA_FILE}
            WORKING_DIRECTORY ${TZDATA_DIR}
            COMMAND_ERROR_IS_FATAL ANY
        )
    endif()

    # Copy the timezone data next to the binary
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

    # SYSTEM includes don't apply to fmt's own translation units, so silence
    # C4834 (discarded [[nodiscard]] in fmt/base.h) when fmt compiles itself.
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
    FetchContent_MakeAvailable(mio)
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
        set(SIMDJSON_BUILD_STATIC ON)
        set(SIMDJSON_ENABLE_THREADS ON)
        FetchContent_MakeAvailable(simdjson)
    endblock()

    # simdjson's PCH unconditionally includes <bit> (C++20); bump to C++20 to
    # avoid MSVC STL4038.
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
        # oneTBB compiles its own translation units with -Werror by default; turn that off
        # so its compiler warnings don't gate our build (we still build our own code with
        # CompilerWarnings.cmake's strict settings).
        set(TBB_STRICT OFF)
        # oneTBB upstream strongly discourages building as a static library and
        # `tbb::parallel_pipeline` relies on dynamic library state. Force a shared
        # build regardless of the project-level BUILD_SHARED_LIBS default so the
        # consumer always links against tbb12.dll on Windows.
        set(BUILD_SHARED_LIBS ON)
        FetchContent_MakeAvailable(tbb)
    endblock()
else()
    # 2021.5.0 is the first release that ships the oneAPI-style tbb::filter_mode /
    # tbb::parallel_pipeline API used by the streaming JSON parser. Older legacy-API
    # system packages (TBB 2020.x and earlier) would fail at use sites rather than at
    # find_package; pinning the minimum version here makes the failure mode early and
    # obvious.
    find_package(TBB 2021.5 REQUIRED)
endif()

# `argparse` is only needed by the `test/log_generator` console helper. It is
# header-only, so the FetchContent build cost is negligible compared with the
# heavier deps above.
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
