cmake_minimum_required(VERSION 3.19)

project(
    structured_log_viewer
    VERSION 0.6.0
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Configure the output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/${CMAKE_BUILD_TYPE})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib/${CMAKE_BUILD_TYPE})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib/${CMAKE_BUILD_TYPE})

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Download and use dependencies if required
include(cmake/FetchDependencies.cmake)

include(cmake/CompilerWarnings.cmake)
add_library(project_warnings INTERFACE)
set_project_warnings(project_warnings False)

# Add subdirectories
add_subdirectory(library)
add_subdirectory(app)

include(CTest)
include(Catch)
add_subdirectory(test/lib)
add_subdirectory(test/app)
