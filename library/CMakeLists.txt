cmake_minimum_required(VERSION 3.19)

project(
    loglib
    VERSION 0.1
    LANGUAGES CXX)

add_library(
    ${PROJECT_NAME} STATIC
    src/json_parser.cpp
    src/log_configuration.cpp
    src/log_data.cpp
    src/log_factory.cpp
    src/log_file.cpp
    src/log_line.cpp
    src/log_processing.cpp
    src/log_table.cpp)

target_include_directories(${PROJECT_NAME} PUBLIC ./include)

target_link_libraries(
    ${PROJECT_NAME}
    PUBLIC nlohmann_json::nlohmann_json
           date::date
           date::date-tz
           fmt::fmt
           tl::expected
           project_warnings)
