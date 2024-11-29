cmake_minimum_required(VERSION 3.19)

project(structured_log_viewer VERSION 0.1 LANGUAGES CXX)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Configure the output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/${CMAKE_BUILD_TYPE})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib/${CMAKE_BUILD_TYPE})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib/${CMAKE_BUILD_TYPE})

# Fetch nlohmann/json, HowardHinnant/date and fmt
cmake_policy(SET CMP0135 NEW)
include(FetchContent)

FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)

FetchContent_Declare(
    date
    GIT_REPOSITORY https://github.com/HowardHinnant/date.git
    GIT_TAG 1a4f424659d39c2a222729bd2b1ccd8f857b3221
)
set(BUILD_TZ_LIB ON) # Enable building the time zone library
set(MANUAL_TZ_DB ON) # Provide time zone database manually

FetchContent_Declare(
    fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG 11.0.1
)

FetchContent_MakeAvailable(nlohmann_json date fmt)

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

add_subdirectory(library)
add_subdirectory(app)
