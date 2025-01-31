include(FetchContent)

# Option to use system libraries
option(USE_SYSTEM_NLOHMANN_JSON "Use system nlohmann_json" OFF)
option(USE_SYSTEM_DATE "Use system date" OFF)
option(USE_SYSTEM_FMT "Use system fmt" OFF)

if (NOT USE_SYSTEM_NLOHMANN_JSON)
    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3
    )
    FetchContent_MakeAvailable(nlohmann_json)
else()
    find_package(nlohmann_json REQUIRED)
endif()

if (NOT USE_SYSTEM_DATE)
    FetchContent_Declare(
        date
        GIT_REPOSITORY https://github.com/HowardHinnant/date.git
        GIT_TAG 1a4f424659d39c2a222729bd2b1ccd8f857b3221
    )
    set(BUILD_TZ_LIB ON) # Enable building the time zone library
    set(MANUAL_TZ_DB ON) # Provide time zone database manually
    FetchContent_MakeAvailable(date)

    # Directories to store tzdata and windowsZones.xml in
    set(TZDATA tzdata)
    set(TZDATA_DIR ${CMAKE_BINARY_DIR}/${TZDATA})
    set(TZDATA_FILE tzdata.tar.gz)
    set(WINDOWS_ZONES_FILE windowsZones.xml)

    # Create the directories if they don't exist
    file(MAKE_DIRECTORY ${TZDATA_DIR})

    # Download the latest windowsZones.xml
    file(DOWNLOAD
        https://raw.githubusercontent.com/unicode-org/cldr/master/common/supplemental/windowsZones.xml
        ${TZDATA_DIR}/${WINDOWS_ZONES_FILE}
        SHOW_PROGRESS
    )

    # Download the latest tzdata
    file(DOWNLOAD
        https://www.iana.org/time-zones/repository/tzdata-latest.tar.gz
        ${TZDATA_DIR}/${TZDATA_FILE}
        SHOW_PROGRESS
    )

    # Extract the latest tzdata
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar xzf ${TZDATA_DIR}/${TZDATA_FILE}
        WORKING_DIRECTORY ${TZDATA_DIR}
        COMMAND_ERROR_IS_FATAL ANY
    )
else()
    find_package(date REQUIRED)
endif()

if (NOT USE_SYSTEM_FMT)
    FetchContent_Declare(
        fmt
        GIT_REPOSITORY https://github.com/fmtlib/fmt.git
        GIT_TAG 11.0.1
    )
    FetchContent_MakeAvailable(fmt)
else()
    find_package(fmt REQUIRED)
endif()
