cmake_minimum_required(VERSION 3.19)

project(library VERSION 0.1 LANGUAGES CXX)

add_library(${PROJECT_NAME} STATIC
  src/json_line.cpp
  src/json_parser.cpp
  src/log_data.cpp
  include/log_table.hpp
  include/log_time.hpp
  include/log_configuration.hpp
  src/log_configuration.cpp
)

target_include_directories(${PROJECT_NAME} PUBLIC ./include)

target_link_libraries(${PROJECT_NAME} PUBLIC nlohmann_json::nlohmann_json date::date date::date-tz fmt::fmt)
