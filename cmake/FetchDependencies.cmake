include(FetchContent)

# Option to use system libraries
option(USE_SYSTEM_NLOHMANN_JSON "Use system nlohmann_json" OFF)
option(USE_SYSTEM_DATE "Use system date" OFF)
option(USE_SYSTEM_FMT "Use system fmt" OFF)

if(not USE_SYSTEM_NLOHMANN_JSON)
    fetchcontent_declare(
        nlohmann_json
        git_repository https://github.com/nlohmann/json.git
        git_tag v3.11.3)
    fetchcontent_makeavailable(nlohmann_json)
else()
    find_package(nlohmann_json required)
endif()

if(not USE_SYSTEM_DATE)
    fetchcontent_declare(
        date
        git_repository https://github.com/HowardHinnant/date.git
        git_tag 1a4f424659d39c2a222729bd2b1ccd8f857b3221)
    set(BUILD_TZ_LIB ON) # Enable building the time zone library
    set(MANUAL_TZ_DB ON) # Provide time zone database manually
    fetchcontent_makeavailable(date)

    # Directories to store tzdata and windowsZones.xml in
    set(TZDATA tzdata)
    set(TZDATA_DIR ${CMAKE_BINARY_DIR}/${TZDATA})
    set(TZDATA_FILE tzdata.tar.gz)
    set(WINDOWS_ZONES_FILE windowsZones.xml)

    # Create the directories if they don't exist
    file(make_directory ${TZDATA_DIR})

    # Download the latest windowsZones.xml
    file(download https://raw.githubusercontent.com/unicode-org/cldr/master/common/supplemental/windowsZones.xml
         ${TZDATA_DIR}/${WINDOWS_ZONES_FILE} SHOW_PROGRESS)

    # Download the latest tzdata
    file(download https://www.iana.org/time-zones/repository/tzdata-latest.tar.gz ${TZDATA_DIR}/${TZDATA_FILE}
         SHOW_PROGRESS)

    # Extract the latest tzdata
    execute_process(command ${CMAKE_COMMAND} -E tar xzf ${TZDATA_DIR}/${TZDATA_FILE}
                    working_directory ${TZDATA_DIR} COMMAND_ERROR_IS_FATAL ANY)
else()
    find_package(date required)
endif()

if(not USE_SYSTEM_FMT)
    fetchcontent_declare(
        fmt
        git_repository https://github.com/fmtlib/fmt.git
        git_tag 11.0.1)
    fetchcontent_makeavailable(fmt)
else()
    find_package(fmt required)
endif()
