# Installs the loglib component and compiles a consumer against the result.
#
# Run as: cmake -DBUILD_DIR=<built tree> -DGENERATOR=<generator> -DCXX_COMPILER=<compiler> -DBUILD_TYPE=<type>
#               -DWORK_DIR=<scratch dir> -P check-loglib-install.cmake
#
# The check installs to a scratch prefix, fails if an internal header was installed or core/row_batch.hpp is missing,
# then builds one translation unit per installed header, so a public header that needs a header that is not installed
# fails here and not for a user. The consumer links loglib::loglib to prove that the package works.
cmake_minimum_required(VERSION 3.28)

foreach(required BUILD_DIR GENERATOR CXX_COMPILER BUILD_TYPE WORK_DIR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "check-loglib-install: ${required} is not set")
    endif()
endforeach()

set(prefix "${WORK_DIR}/prefix")
set(consumer "${WORK_DIR}/consumer")
file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${consumer}")

execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --prefix "${prefix}" --component loglib
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "install failed:\n${output}")
endif()

file(GLOB_RECURSE headers RELATIVE "${prefix}/include" "${prefix}/include/loglib/*.hpp")
list(LENGTH headers headerCount)
if(headerCount EQUAL 0)
    message(FATAL_ERROR "no loglib headers were installed under ${prefix}/include")
endif()
if(NOT EXISTS "${prefix}/include/loglib/core/row_batch.hpp")
    message(FATAL_ERROR "loglib/core/row_batch.hpp was not installed")
endif()

set(sources main.cpp)
file(WRITE "${consumer}/main.cpp"
    "#include <loglib/core/version.hpp>\n"
    "int main() { return loglib::Version().empty() ? 1 : 0; }\n")
foreach(header IN LISTS headers)
    if(header MATCHES "/internal/")
        message(FATAL_ERROR "an internal header was installed: ${header}")
    endif()
    string(REGEX REPLACE "[/.]" "_" unit "${header}")
    file(WRITE "${consumer}/${unit}.cpp" "#include <${header}>\n")
    list(APPEND sources "${unit}.cpp")
endforeach()

string(REPLACE ";" "\n    " sourceLines "${sources}")
file(WRITE "${consumer}/CMakeLists.txt"
    "cmake_minimum_required(VERSION 3.28)\n"
    "project(loglib_install_consumer LANGUAGES CXX)\n"
    "set(CMAKE_CXX_STANDARD 23)\n"
    "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n"
    "find_package(loglib CONFIG REQUIRED)\n"
    "add_executable(consumer\n    ${sourceLines}\n)\n"
    "target_link_libraries(consumer PRIVATE loglib::loglib)\n")

execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${consumer}" -B "${WORK_DIR}/build" -G "${GENERATOR}"
            "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}" "-DCMAKE_BUILD_TYPE=${BUILD_TYPE}" "-DCMAKE_PREFIX_PATH=${prefix}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "consumer configure failed:\n${output}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${WORK_DIR}/build"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "consumer build failed:\n${output}")
endif()

message(STATUS "installed ${headerCount} headers and built a consumer that includes each one")
