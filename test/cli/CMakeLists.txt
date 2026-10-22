cmake_minimum_required(VERSION 3.28)

add_executable(
    slp_tests
    src/slp_process.cpp
    src/test_options.cpp
    src/test_formats.cpp
    src/test_platform.cpp
    src/test_query_e2e.cpp
    src/test_query_golden.cpp
    src/test_query_sources_e2e.cpp
    src/test_schema_validate_e2e.cpp
    src/test_stats_e2e.cpp
)

# Compression codecs are linked so the source-matrix e2e cases can
# build reference .gz/.bz2/.xz/.zst fixtures in-process.
target_link_libraries(
    slp_tests
    PRIVATE
        slp_core
        Catch2::Catch2WithMain
        project_warnings
        ZLIB::ZLIB
        BZip2::BZip2
        LibLZMA::LibLZMA
        zstd::libzstd
)
target_include_directories(slp_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# E2E and golden cases shell out to the real `slp` binary.
add_dependencies(slp_tests slp)
target_compile_definitions(
    slp_tests
    PRIVATE
        "SLP_EXECUTABLE=\"$<TARGET_FILE:slp>\""
        "SLP_FIXTURES_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/fixtures\""
        "SLP_GOLDEN_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/golden\""
)

if(WIN32)
    add_custom_command(
        TARGET slp_tests
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:TBB::tbb> $<TARGET_FILE_DIR:slp_tests>
        COMMENT "Copying TBB runtime DLL next to slp_tests.exe"
        VERBATIM
    )
endif()

catch_discover_tests(slp_tests PROPERTIES LABELS "cli")

# One-command golden regeneration: sets SLP_UPDATE_GOLDEN and runs the
# golden-tagged cases. Documented in test/cli/README.md.
add_custom_target(
    slp-update-goldens
    COMMAND
        ${CMAKE_COMMAND} -E env SLP_UPDATE_GOLDEN=1 ${CMAKE_CTEST_COMMAND} --preset release -L cli -R golden
        --output-on-failure
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Regenerating slp golden files under test/cli/golden"
    VERBATIM
)
add_dependencies(slp-update-goldens slp_tests)
