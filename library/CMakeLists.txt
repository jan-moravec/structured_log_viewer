cmake_minimum_required(VERSION 3.19)

add_library(
    loglib STATIC
    src/json_parser.cpp
    src/log_configuration.cpp
    src/log_data.cpp
    src/log_factory.cpp
    src/log_file.cpp
    src/log_line.cpp
    src/log_processing.cpp
    src/log_table.cpp)

target_include_directories(loglib PUBLIC ./include)

target_link_libraries(
    loglib
    PUBLIC mio
           simdjson::simdjson
           glaze::glaze
           date::date
           date::date-tz
           fmt::fmt
           project_warnings)
