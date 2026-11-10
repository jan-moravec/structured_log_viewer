cmake_minimum_required(VERSION 3.28)

# Helpers shared by the unit tests and the benchmarks: fixture generation, parse-only drivers and table builders.
add_library(loglib_test_support STATIC "src/common.cpp" "src/parse_only.cpp" "src/table_from_data.cpp")

target_include_directories(loglib_test_support PUBLIC include)

# TBB and fmt are linked PUBLIC because the helper headers include loglib/internal/** pipeline headers (which use
# fmt::format) and the tests use TBB directly; loglib keeps both PRIVATE.
target_link_libraries(
    loglib_test_support
    PUBLIC loglib test_common log_generator_core Catch2::Catch2 TBB::tbb fmt::fmt
    PRIVATE project_warnings
)

# Unit tests. The benchmarks are a separate executable, `loglib_benchmarks`, defined below.
add_executable(
    tests
    "src/test_append_report.cpp"
    "src/test_auto_detect_parser.cpp"
    "src/test_batch_contract.cpp"
    "src/test_column_projection.cpp"
    "src/test_column_key_id_cache.cpp"
    "src/test_column_type_health.cpp"
    "src/test_concurrent_reader.cpp"
    "src/test_correctness_regressions.cpp"
    "src/test_csv_parser.cpp"
    "src/test_csv_tokenize.cpp"
    "src/test_decompressing_byte_source.cpp"
    "src/test_enum_dictionary.cpp"
    "src/test_enum_inference.cpp"
    "src/test_exports.cpp"
    "src/test_explicit_time_inputs.cpp"
    "src/test_file_busy_retry.cpp"
    "src/test_file_identity.cpp"
    "src/test_file_line_source.cpp"
    "src/test_file_loading.cpp"
    "src/test_format_detection.cpp"
    "src/test_histogram_bucket_index.cpp"
    "src/test_json_parser.cpp"
    "src/test_key_index.cpp"
    "src/test_leaf_rule_compile.cpp"
    "src/test_line_bytes_queue.cpp"
    "src/test_log_configuration.cpp"
    "src/test_log_compare.cpp"
    "src/test_log_data.cpp"
    "src/test_arrival_clock.cpp"
    "src/test_log_generator_tool.cpp"
    "src/test_log_filter.cpp"
    "src/test_log_line.cpp"
    "src/test_log_processing.cpp"
    "src/test_log_string_matcher.cpp"
    "src/test_log_table.cpp"
    "src/test_logfmt_parser.cpp"
    "src/test_parse_file.cpp"
    "src/test_parser_pipeline.cpp"
    "src/test_query_parser.cpp"
    "src/test_regex_parser.cpp"
    "src/test_regex_template_generators.cpp"
    "src/test_regex_template_io.cpp"
    "src/test_regex_templates.cpp"
    "src/test_rotation_siblings.cpp"
    "src/test_row_batch.cpp"
    "src/test_row_ordering.cpp"
    "src/test_session_bundle.cpp"
    "src/test_source_plan.cpp"
    "src/test_stdin_bytes_producer.cpp"
    "src/test_stdin_peek.cpp"
    "src/test_stream_line_source.cpp"
    "src/test_stream_sink.cpp"
    "src/test_stream_stop_teardown.cpp"
    "src/test_table_loader.cpp"
    "src/test_tailing_bytes_producer.cpp"
    "src/test_tcp_server_producer.cpp"
    "src/test_theme.cpp"
    "src/test_timestamp_corpus.cpp"
    "src/test_timestamp_promotion.cpp"
    "src/test_udp_server_producer.cpp"
    "src/test_tzdata_search.cpp"
    "src/test_unicode_paths.cpp"
    "src/test_user_paths.cpp"
    "src/test_user_timestamp.cpp"
    "src/test_version.cpp"
    "src/test_worker_threads.cpp"
)

# The TLS test programmatically generates a self-signed cert via
# OpenSSL APIs at startup, so it only compiles when TLS support is
# in. Gated by the same `LOGLIB_NETWORK_TLS` flag the producer uses.
if(LOGLIB_NETWORK_TLS)
    target_sources(tests PRIVATE "src/test_tcp_server_producer_tls.cpp")
    target_link_libraries(tests PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()

# TBB linked directly here for the concurrent KeyIndex test; loglib keeps it PRIVATE.
# fmt is linked directly so TUs that include `loglib/internal/**` pipeline headers
# (which use `fmt::format`) still see the header. loglib keeps fmt PRIVATE.
# The compression codecs are linked directly too so the decompression tests can
# build reference compressed fixtures with the same libraries loglib decodes.
target_link_libraries(
    tests
    PRIVATE
        loglib_test_support
        Catch2::Catch2WithMain
        project_warnings
        ZLIB::ZLIB
        BZip2::BZip2
        LibLZMA::LibLZMA
        zstd::libzstd
)

# Enable KeyIndex call-count instrumentation; PUBLIC so the test TU and
# `loglib` see the same static counter definitions. Gated by `BUILD_TESTING`
# at the top level, so `-DBUILD_TESTING=OFF` strips it from shipped builds.
target_compile_definitions(loglib PUBLIC $<BUILD_INTERFACE:LOGLIB_KEY_INDEX_INSTRUMENTATION>)

# The version the library must report, taken from the same project() call that sets it.
target_compile_definitions(tests PRIVATE "LOGLIB_TEST_PROJECT_VERSION=\"${PROJECT_VERSION}\"")

# Benchmarks: a separate executable on Google Benchmark, so changing one does not relink the unit tests and two builds
# can be compared by copying only this binary. It has its own main (benchmark_main.cpp).
add_executable(
    loglib_benchmarks
    "src/benchmark_csv.cpp"
    "src/benchmark_decompression.cpp"
    "src/benchmark_histogram_bucket_index.cpp"
    "src/benchmark_json.cpp"
    "src/benchmark_log_filter.cpp"
    "src/benchmark_logfmt.cpp"
    "src/benchmark_main.cpp"
    "src/benchmark_regex.cpp"
    "src/benchmark_session_bundle.cpp"
    "src/benchmark_stream.cpp"
)

target_link_libraries(
    loglib_benchmarks
    PRIVATE
        loglib_test_support
        benchmark::benchmark
        project_warnings
        ZLIB::ZLIB
        BZip2::BZip2
        LibLZMA::LibLZMA
        zstd::libzstd
)

# `benchmark_json.cpp` calls `GetProcessMemoryInfo` to report peak working set; on Windows that lives in
# `psapi.lib` (Windows 7+ also re-exports it from `kernel32.dll`, but linking the import library is the portable
# form across SDK versions).
if(WIN32)
    target_link_libraries(loglib_benchmarks PRIVATE Psapi)
endif()

# On Windows the TBB runtime DLL must sit next to each executable for both
# `catch_discover_tests` (which runs `<exe> --list-test-names-only` at build
# time) and `ctest` itself to find it.
if(WIN32)
    foreach(test_target tests loglib_benchmarks)
        add_custom_command(
            TARGET ${test_target}
            POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:TBB::tbb> $<TARGET_FILE_DIR:${test_target}>
            COMMENT "Copying TBB runtime DLL next to ${test_target}.exe"
            VERBATIM
        )
    endforeach()
endif()

# ADD_TAGS_AS_LABELS turns every Catch2 tag into a CTest label, so `[network]`, `[fs-events]` and the per-unit tags
# are labels too. Select with `ctest -L <tag>`, exclude with `-LE <tag>`.
catch_discover_tests(tests ADD_TAGS_AS_LABELS)

# All library benchmarks are one CTest test. Google Benchmark writes its figures as JSON to the build tree; the console
# output still goes to the CTest log. Set BENCHMARK_FILTER (a regex) in the environment to run only some of them.
# SKIP_RETURN_CODE 4 is the exit code of a Debug build, which refuses to run benchmarks.
set(LOGLIB_BENCHMARK_RESULTS_DIR "${CMAKE_BINARY_DIR}/benchmark-results")
file(MAKE_DIRECTORY "${LOGLIB_BENCHMARK_RESULTS_DIR}")
add_test(
    NAME loglib_benchmarks
    COMMAND
        loglib_benchmarks "--benchmark_out=${LOGLIB_BENCHMARK_RESULTS_DIR}/loglib_benchmarks.json"
        --benchmark_out_format=json
)
set_tests_properties(loglib_benchmarks PROPERTIES LABELS "benchmark" SKIP_RETURN_CODE 4)

# Installs the loglib component and compiles a consumer that includes every installed header. Skipped for sanitizer
# and coverage builds, because the consumer is built without their flags.
if(NOT CMAKE_CXX_FLAGS MATCHES "-fsanitize|-fprofile")
    add_test(
        NAME loglib_install_consumer
        COMMAND
            ${CMAKE_COMMAND} -DBUILD_DIR=${CMAKE_BINARY_DIR} -DGENERATOR=${CMAKE_GENERATOR}
            -DCXX_COMPILER=${CMAKE_CXX_COMPILER} -DBUILD_TYPE=${CMAKE_BUILD_TYPE}
            -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/install_check -P
            ${PROJECT_SOURCE_DIR}/scripts/check-loglib-install.cmake
    )
    set_tests_properties(loglib_install_consumer PROPERTIES LABELS "install")
endif()
