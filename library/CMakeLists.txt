cmake_minimum_required(VERSION 3.19)

project(library VERSION 0.1 LANGUAGES CXX)

add_library(${PROJECT_NAME} STATIC
  src/log_message.cpp
)

target_include_directories(${PROJECT_NAME} PUBLIC ./include)

target_link_libraries(${PROJECT_NAME} PUBLIC nlohmann_json::nlohmann_json date::date date::date-tz fmt::fmt)
