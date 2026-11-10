cmake_minimum_required(VERSION 3.28)

# Fuzz targets for the code that reads untrusted input. Each target is one file with an LLVMFuzzerTestOneInput function
# and no call into libFuzzer, so the same file builds three ways:
#
# - fuzz_replay_<name>: always built with the tests. It runs the saved corpus and regression inputs as an ordinary
#   CTest test with the label fuzz-replay, on every platform and compiler.
# - fuzz_<name>: built only with -DLOGLIB_FUZZ=ON and Clang, linked with libFuzzer, ASan and UBSan. Not a CTest test.
# - any other engine can link the same file against its own driver.
option(LOGLIB_FUZZ "Build the libFuzzer fuzz targets (Clang only)" OFF)

set(LOGLIB_FUZZ_TARGETS
    query
    json
    logfmt
    csv
    regex
    regex_template
    log_configuration
    timestamp
    decompress
)

set(LOGLIB_FUZZ_BUILD OFF)
if(LOGLIB_FUZZ)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT MSVC)
        set(LOGLIB_FUZZ_BUILD ON)
    else()
        message(WARNING "LOGLIB_FUZZ needs Clang with libFuzzer. The fuzz targets are not built.")
    endif()
endif()

foreach(fuzz_name IN LISTS LOGLIB_FUZZ_TARGETS)
    set(fuzz_source "${CMAKE_CURRENT_SOURCE_DIR}/fuzz_${fuzz_name}.cpp")

    add_executable(fuzz_replay_${fuzz_name} "${fuzz_source}" replay_main.cpp)
    target_link_libraries(fuzz_replay_${fuzz_name} PRIVATE loglib project_warnings)
    target_include_directories(fuzz_replay_${fuzz_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(
        NAME fuzz_replay_${fuzz_name}
        COMMAND
            fuzz_replay_${fuzz_name} "${CMAKE_CURRENT_SOURCE_DIR}/corpus/${fuzz_name}"
            "${CMAKE_CURRENT_SOURCE_DIR}/regressions/${fuzz_name}"
    )
    set_tests_properties(fuzz_replay_${fuzz_name} PROPERTIES LABELS "fuzz-replay")

    # The TBB runtime DLL must sit next to each executable on Windows.
    if(WIN32)
        add_custom_command(
            TARGET fuzz_replay_${fuzz_name}
            POST_BUILD
            COMMAND
                ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:TBB::tbb>
                $<TARGET_FILE_DIR:fuzz_replay_${fuzz_name}>
            COMMENT "Copying TBB runtime DLL next to fuzz_replay_${fuzz_name}.exe"
            VERBATIM
        )
    endif()

    if(LOGLIB_FUZZ_BUILD)
        add_executable(fuzz_${fuzz_name} "${fuzz_source}")
        target_link_libraries(fuzz_${fuzz_name} PRIVATE loglib project_warnings)
        target_include_directories(fuzz_${fuzz_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_compile_options(fuzz_${fuzz_name} PRIVATE -fsanitize=fuzzer,address,undefined)
        target_link_options(fuzz_${fuzz_name} PRIVATE -fsanitize=fuzzer,address,undefined)
    endif()
endforeach()
