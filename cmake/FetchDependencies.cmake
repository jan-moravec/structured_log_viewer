include(FetchContent)

# Option to use system libraries
option(USE_SYSTEM_NLOHMANN_JSON "Use system nlohmann_json" OFF)
option(USE_SYSTEM_DATE "Use system date" OFF)
option(USE_SYSTEM_FMT "Use system fmt" OFF)
option(USE_SYSTEM_CATCH2 "Use system Catch2" OFF)
option(USE_SYSTEM_TL_EXPECTED "Use system TartanLlama expected" OFF)

if(NOT USE_SYSTEM_NLOHMANN_JSON)
    fetchcontent_declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3)
    fetchcontent_makeavailable(nlohmann_json)
else()
    find_package(nlohmann_json REQUIRED)
endif()

if(NOT USE_SYSTEM_DATE)
    fetchcontent_declare(
        date
        GIT_REPOSITORY https://github.com/HowardHinnant/date.git
        GIT_TAG 1a4f424659d39c2a222729bd2b1ccd8f857b3221)
    set(BUILD_TZ_LIB ON) # Enable building the time zone library
    set(MANUAL_TZ_DB ON) # Provide time zone database manually
    fetchcontent_makeavailable(date)

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
        GIT_TAG 11.0.1)
    fetchcontent_makeavailable(fmt)
else()
    find_package(fmt REQUIRED)
endif()

if(NOT USE_SYSTEM_CATCH2)
    fetchcontent_declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.8.0)
    fetchcontent_makeavailable(Catch2)
else()
    find_package(Catch2 REQUIRED)
endif()

if(NOT USE_SYSTEM_TL_EXPECTED)
    fetchcontent_declare(
        tl_expected
        GIT_REPOSITORY https://github.com/TartanLlama/expected.git
        GIT_TAG v1.1.0)
    set(EXPECTED_BUILD_PACKAGE OFF)
    set(EXPECTED_BUILD_TESTS OFF)
    fetchcontent_makeavailable(tl_expected)
else()
    find_package(tl_expected REQUIRED)
endif()
