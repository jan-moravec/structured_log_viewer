include(FetchContent)

# Option to use system libraries
option(USE_SYSTEM_DATE "Use system HowardHinnant date library" OFF)
option(USE_SYSTEM_FMT "Use system fmtlib fmt library" OFF)
option(USE_SYSTEM_CATCH2 "Use system catchorg Catch2 library" OFF)
option(USE_SYSTEM_MIO "Use system mandreyel mio library" OFF)
option(USE_SYSTEM_GLAZE "Use system stephenberry Glaze library" OFF)
option(USE_SYSTEM_SIMDJSON "Use system simdjson library" OFF)
option(USE_SYSTEM_ROBIN_MAP "Use system Tessil robin-map library" OFF)

if(NOT USE_SYSTEM_DATE)
    fetchcontent_declare(
        date
        GIT_REPOSITORY https://github.com/HowardHinnant/date.git
        GIT_TAG v3.0.4
        SYSTEM
        EXCLUDE_FROM_ALL)
    block()
        set(BUILD_TZ_LIB ON) # Enable building the time zone library
        set(MANUAL_TZ_DB ON) # Provide time zone database manually
        fetchcontent_makeavailable(date)
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
        file(DOWNLOAD https://raw.githubusercontent.com/unicode-org/cldr/master/common/supplemental/windowsZones.xml
             ${TZDATA_DIR}/${WINDOWS_ZONES_FILE} SHOW_PROGRESS)
    endif()

    # Download the latest tzdata
    if(NOT EXISTS ${CMAKE_BINARY_DIR}/${TZDATA_FILE})
        file(DOWNLOAD https://www.iana.org/time-zones/repository/tzdata-latest.tar.gz ${CMAKE_BINARY_DIR}/${TZDATA_FILE}
             SHOW_PROGRESS)
        # Extract the latest tzdata
        execute_process(COMMAND ${CMAKE_COMMAND} -E tar xzf ${CMAKE_BINARY_DIR}/${TZDATA_FILE}
                        WORKING_DIRECTORY ${TZDATA_DIR} COMMAND_ERROR_IS_FATAL ANY)
    endif()

    # Copy the timezone data next to the binary
    execute_process(COMMAND ${CMAKE_COMMAND} -E copy_directory ${TZDATA_DIR}
                            ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${TZDATA} COMMAND_ERROR_IS_FATAL ANY)
else()
    find_package(date REQUIRED)
endif()

if(NOT USE_SYSTEM_FMT)
    fetchcontent_declare(
        fmt
        GIT_REPOSITORY https://github.com/fmtlib/fmt.git
        GIT_TAG 12.1.0
        SYSTEM
        EXCLUDE_FROM_ALL)
    fetchcontent_makeavailable(fmt)

    # SYSTEM includes don't apply to fmt's own translation units, so silence
    # C4834 (discarded [[nodiscard]] in fmt/base.h) when fmt compiles itself.
    if(MSVC)
        target_compile_options(fmt PRIVATE /wd4834)
    endif()
else()
    find_package(fmt REQUIRED)
endif()

if(NOT USE_SYSTEM_CATCH2)
    fetchcontent_declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.14.0
        SYSTEM
        EXCLUDE_FROM_ALL)
    fetchcontent_makeavailable(Catch2)
else()
    find_package(Catch2 REQUIRED)
endif()

if(NOT USE_SYSTEM_MIO)
    fetchcontent_declare(
        mio
        GIT_REPOSITORY https://github.com/mandreyel/mio.git
        GIT_TAG 8b6b7d8
        SYSTEM
        EXCLUDE_FROM_ALL)
    fetchcontent_makeavailable(mio)
else()
    find_package(mio REQUIRED)
endif()

if(NOT USE_SYSTEM_GLAZE)
    fetchcontent_declare(
        glaze
        GIT_REPOSITORY https://github.com/stephenberry/glaze.git
        GIT_TAG v7.4.0
        SYSTEM
        EXCLUDE_FROM_ALL)
    fetchcontent_makeavailable(glaze)
else()
    find_package(glaze REQUIRED)
endif()

if(NOT USE_SYSTEM_SIMDJSON)
    fetchcontent_declare(
        simdjson
        GIT_REPOSITORY https://github.com/simdjson/simdjson.git
        GIT_TAG v4.6.3
        SYSTEM
        EXCLUDE_FROM_ALL)
    block()
        set(SIMDJSON_BUILD_STATIC ON)
        set(SIMDJSON_ENABLE_THREADS ON)
        fetchcontent_makeavailable(simdjson)
    endblock()

    # simdjson's PCH unconditionally includes <bit> (C++20); bump to C++20 to
    # avoid MSVC STL4038.
    set_target_properties(simdjson PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON)
else()
    find_package(simdjson REQUIRED)
endif()

if(NOT USE_SYSTEM_ROBIN_MAP)
    fetchcontent_declare(
        robin_map
        GIT_REPOSITORY https://github.com/Tessil/robin-map.git
        GIT_TAG v1.4.1
        SYSTEM
        EXCLUDE_FROM_ALL)
    fetchcontent_makeavailable(robin_map)
else()
    find_package(robin_map REQUIRED)
endif()
