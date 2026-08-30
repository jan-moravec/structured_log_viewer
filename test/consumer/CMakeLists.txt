cmake_minimum_required(VERSION 3.28)

# External-consumer smoke test for the installed loglib package.
# Do not add repository include paths or find_package() loglib's
# third-party dependencies; `loglib::loglib` supplies the public
# interface and private link archives.
project(loglib_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(loglib CONFIG REQUIRED)

add_executable(loglib_consumer src/main.cpp)
target_link_libraries(loglib_consumer PRIVATE loglib::loglib)
