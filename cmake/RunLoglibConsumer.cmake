# Installs the loglib CMake package into a staging prefix, then configures,
# builds, and runs test/consumer against that prefix. The consumer project
# must not name loglib's third-party packages or add repository include paths.

if(NOT DEFINED LOGLIB_BUILD_DIR OR NOT DEFINED LOGLIB_SOURCE_DIR)
    message(FATAL_ERROR "RunLoglibConsumer.cmake requires LOGLIB_BUILD_DIR and LOGLIB_SOURCE_DIR")
endif()

set(_consumer_src "${LOGLIB_SOURCE_DIR}/test/consumer")
set(_stage "${LOGLIB_BUILD_DIR}/loglib_sdk_stage")
set(_consumer_build "${LOGLIB_BUILD_DIR}/loglib_consumer_build")

file(READ "${_consumer_src}/CMakeLists.txt" _consumer_cmake)
foreach(
    _forbidden
    IN
    ITEMS fmt simdjson OpenSSL TBB glaze mio PCRE2 date robin_map zlib zstd lzma BZip2 efsw asio
)
    if(_consumer_cmake MATCHES "find_package\\([ \t]*${_forbidden}")
        message(FATAL_ERROR "test/consumer/CMakeLists.txt must not call find_package(${_forbidden})")
    endif()
endforeach()
if(NOT _consumer_cmake MATCHES "loglib::loglib")
    message(FATAL_ERROR "test/consumer/CMakeLists.txt must link loglib::loglib")
endif()

file(REMOVE_RECURSE "${_stage}" "${_consumer_build}")

set(_install_cmd
    "${CMAKE_COMMAND}"
    --install
    "${LOGLIB_BUILD_DIR}"
    --prefix
    "${_stage}"
    --component
    loglib
)
if(LOGLIB_BUILD_TYPE)
    list(APPEND _install_cmd --config "${LOGLIB_BUILD_TYPE}")
endif()
execute_process(COMMAND ${_install_cmd} COMMAND_ERROR_IS_FATAL ANY)

if(EXISTS "${_stage}/include/loglib/internal")
    message(FATAL_ERROR "installed loglib package contains include/loglib/internal")
endif()
if(NOT EXISTS "${_stage}/include/loglib/log_table.hpp")
    message(FATAL_ERROR "installed loglib package is missing include/loglib/log_table.hpp")
endif()
if(NOT EXISTS "${_stage}/include/tsl/robin_map.h")
    message(FATAL_ERROR "installed loglib package is missing public robin-map headers")
endif()

set(_cfg
    "${CMAKE_COMMAND}"
    -S
    "${_consumer_src}"
    -B
    "${_consumer_build}"
    "-DCMAKE_PREFIX_PATH=${_stage}"
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
)
if(LOGLIB_GENERATOR)
    list(APPEND _cfg -G "${LOGLIB_GENERATOR}")
endif()
if(LOGLIB_MAKE_PROGRAM)
    list(APPEND _cfg "-DCMAKE_MAKE_PROGRAM=${LOGLIB_MAKE_PROGRAM}")
endif()
if(LOGLIB_CXX_COMPILER)
    list(APPEND _cfg "-DCMAKE_CXX_COMPILER=${LOGLIB_CXX_COMPILER}")
endif()
if(LOGLIB_C_COMPILER)
    list(APPEND _cfg "-DCMAKE_C_COMPILER=${LOGLIB_C_COMPILER}")
endif()
if(LOGLIB_BUILD_TYPE)
    list(APPEND _cfg "-DCMAKE_BUILD_TYPE=${LOGLIB_BUILD_TYPE}")
endif()
if(DEFINED LOGLIB_IPO AND NOT LOGLIB_IPO STREQUAL "")
    list(APPEND _cfg "-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=${LOGLIB_IPO}")
endif()

execute_process(COMMAND ${_cfg} COMMAND_ERROR_IS_FATAL ANY)

set(_cc_path "${_consumer_build}/compile_commands.json")
if(EXISTS "${_cc_path}")
    file(READ "${_cc_path}" _cc)
    string(REPLACE "\\" "/" _cc "${_cc}")
    set(_repo "${LOGLIB_SOURCE_DIR}")
    string(REPLACE "\\" "/" _repo "${_repo}")
    string(REPLACE "+" "\\\\+" _repo_re "${_repo}")
    if(_cc MATCHES "${_repo_re}/library/include")
        message(FATAL_ERROR "consumer compile commands include the repository library/include path")
    endif()
    set(_build "${LOGLIB_BUILD_DIR}")
    string(REPLACE "\\" "/" _build "${_build}")
    string(REPLACE "+" "\\\\+" _build_re "${_build}")
    if(_cc MATCHES "${_build_re}/_deps")
        message(FATAL_ERROR "consumer compile commands include FetchContent _deps include paths")
    endif()
    # Unique flag from cmake/CompilerWarnings.cmake; must not leak via project_warnings.
    if(_cc MATCHES "/w14242" OR _cc MATCHES "-Wnon-virtual-dtor")
        message(FATAL_ERROR "consumer compile commands inherit repository project_warnings flags")
    endif()
endif()

execute_process(COMMAND "${CMAKE_COMMAND}" --build "${_consumer_build}" COMMAND_ERROR_IS_FATAL ANY)

set(_exe "${_consumer_build}/loglib_consumer")
if(WIN32)
    set(_exe "${_exe}.exe")
endif()
if(NOT EXISTS "${_exe}" AND LOGLIB_BUILD_TYPE)
    set(_exe "${_consumer_build}/${LOGLIB_BUILD_TYPE}/loglib_consumer")
    if(WIN32)
        set(_exe "${_exe}.exe")
    endif()
endif()
if(NOT EXISTS "${_exe}")
    message(FATAL_ERROR "consumer executable not found after build")
endif()

if(WIN32)
    set(ENV{PATH} "${_stage}/bin;$ENV{PATH}")
elseif(APPLE)
    set(ENV{DYLD_LIBRARY_PATH} "${_stage}/lib:$ENV{DYLD_LIBRARY_PATH}")
else()
    set(ENV{LD_LIBRARY_PATH} "${_stage}/lib:$ENV{LD_LIBRARY_PATH}")
endif()

execute_process(COMMAND "${_exe}" COMMAND_ERROR_IS_FATAL ANY)
