cmake_minimum_required(version 3.19)

project(
    loglib
    version 0.1
    languages CXX)

add_library(
    ${PROJECT_NAME} static
    src/json_line.cpp
    src/json_parser.cpp
    src/log_configuration.cpp
    src/log_data.cpp
    src/log_factory.cpp
    src/log_line.cpp
    src/log_processing.cpp
    src/log_table.cpp)

target_include_directories(${PROJECT_NAME} public ./include)

target_link_libraries(${PROJECT_NAME} public nlohmann_json::nlohmann_json date::date date::date-tz fmt::fmt)
