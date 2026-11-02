cmake_minimum_required(VERSION 3.28)

# Generate `regex_templates_embedded.cpp` from every JSON file under
# `data/regex_templates/` at configure time. The generated TU plays
# the role of a Qt resource file for the library layer: it lets
# `loglib::BuiltinRegexTemplates()` ship the same one-file-per-template
# data shape themes use, without dragging Qt into the library. The
# generator (`cmake/EmbedRegexTemplates.cmake`) registers each JSON
# as a configure-step dependency, so editing a template
# automatically re-runs the embed step.
include(${CMAKE_SOURCE_DIR}/cmake/EmbedRegexTemplates.cmake)
set(REGEX_TEMPLATES_EMBEDDED_SRC ${CMAKE_CURRENT_BINARY_DIR}/regex_templates_embedded.cpp)
embed_regex_templates(
    INPUT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/data/regex_templates
    OUTPUT_FILE ${REGEX_TEMPLATES_EMBEDDED_SRC}
)

add_library(
    loglib
    STATIC
    src/parse/auto_detect_parser.cpp
    src/parse/batch_coalescer.cpp
    src/parse/buffering_sink.cpp
    src/core/bytes_producer.cpp
    src/core/compact_log_value.cpp
    src/ingest/decompressing_byte_source.cpp
    src/core/enum_dictionary.cpp
    src/table/enum_inference.cpp
    src/output/exports/export_sink.cpp
    src/output/exports/row_exporter.cpp
    src/ingest/file_identity.cpp
    src/core/file_line_source.cpp
    src/parse/format_detection.cpp
    src/query/histogram_bucket_index.cpp
    src/core/key_index.cpp
    src/parse/parse_error_text.cpp
    src/parse/parse_file.cpp
    src/parse/static_parser_pipeline.cpp
    src/core/stream_line_source.cpp
    src/ingest/tailing_bytes_producer.cpp
    src/ingest/tcp_server_producer.cpp
    src/ingest/udp_server_producer.cpp
    src/parse/timestamp_promotion.cpp
    src/schema/log_configuration.cpp
    src/query/column_projection.cpp
    src/parse/column_key_id_cache.cpp
    src/table/column_type_health.cpp
    src/clang_tidy_stubs/log_configuration_glaze_meta.cpp
    src/clang_tidy_stubs/log_configuration_glaze_opts.cpp
    src/query/log_compare.cpp
    src/core/log_data.cpp
    src/core/log_file.cpp
    src/query/log_filter.cpp
    src/core/log_level.cpp
    src/core/log_line.cpp
    src/parse/log_parser.cpp
    src/query/leaf_rule_compile.cpp
    src/query/log_string_matcher.cpp
    src/time/time_zone_context.cpp
    src/table/timestamp_backfill.cpp
    src/time/timestamp_parsing.cpp
    src/time/user_timestamp.cpp
    src/table/log_table.cpp
    src/output/normalized_json_row.cpp
    src/query/query_parser.cpp
    src/ingest/rotation_siblings.cpp
    src/ingest/source_plan.cpp
    src/core/stable_byte_arena.cpp
    src/support/tzdata_search.cpp
    src/support/user_paths.cpp
    src/parse/worker_threads.cpp
    src/core/version.cpp
    src/query/row_ordering.cpp
    src/output/session_bundle_writer.cpp
    src/output/session_bundle_reader.cpp
    src/parse/parsers/csv_parser.cpp
    src/parse/parsers/json_parser.cpp
    src/parse/parsers/logfmt_parser.cpp
    src/parse/parsers/regex_parser.cpp
    src/parse/regex_templates.cpp
    src/ingest/stdin_bytes_producer.cpp
    src/ingest/stdin_peek.cpp
    ${REGEX_TEMPLATES_EMBEDDED_SRC}
    src/clang_tidy_stubs/regex_template_glaze_meta.cpp
    src/support/theme.cpp
    src/clang_tidy_stubs/theme_glaze_meta.cpp
)

# Supported v1 headers. This file set is the machine-readable public API:
# CMake install/export rules consume it, and a configure-time check below
# fails if it drifts from the non-internal headers on disk. Internal
# headers stay under include/loglib/internal/ so in-tree library and
# test/lib translation units can still include them via BASE_DIRS;
# they are not members of this set and must not be installed.
set(LOGLIB_PUBLIC_HEADERS
    include/loglib/schema/anchor_entry.hpp
    include/loglib/core/ascii_case.hpp
    include/loglib/parse/auto_detect_parser.hpp
    include/loglib/core/bytes_producer.hpp
    include/loglib/query/column_projection.hpp
    include/loglib/schema/column_schema.hpp
    include/loglib/table/column_type_health.hpp
    include/loglib/core/compact_log_value.hpp
    include/loglib/ingest/decompressing_byte_source.hpp
    include/loglib/core/enum_dictionary.hpp
    include/loglib/table/enum_inference.hpp
    include/loglib/output/exports/export_sink.hpp
    include/loglib/output/exports/row_exporter.hpp
    include/loglib/core/file_line_source.hpp
    include/loglib/schema/filter_expression.hpp
    include/loglib/parse/format_detection.hpp
    include/loglib/schema/highlight_rule.hpp
    include/loglib/query/histogram_bucket_index.hpp
    include/loglib/core/key_index.hpp
    include/loglib/query/leaf_rule_compile.hpp
    include/loglib/core/line_source.hpp
    include/loglib/query/log_compare.hpp
    include/loglib/schema/log_configuration.hpp
    include/loglib/core/log_data.hpp
    include/loglib/core/log_file.hpp
    include/loglib/query/log_filter.hpp
    include/loglib/core/log_level.hpp
    include/loglib/core/log_line.hpp
    include/loglib/parse/log_parse_sink.hpp
    include/loglib/parse/log_parser.hpp
    include/loglib/query/log_string_matcher.hpp
    include/loglib/table/log_table.hpp
    include/loglib/core/log_value.hpp
    include/loglib/parse/parse_error_text.hpp
    include/loglib/parse/parse_file.hpp
    include/loglib/parse/parser_options.hpp
    include/loglib/parse/parsers/csv_parser.hpp
    include/loglib/parse/parsers/json_parser.hpp
    include/loglib/parse/parsers/logfmt_parser.hpp
    include/loglib/parse/parsers/regex_parser.hpp
    include/loglib/query/query_parser.hpp
    include/loglib/parse/regex_templates.hpp
    include/loglib/ingest/rotation_siblings.hpp
    include/loglib/query/row_ordering.hpp
    include/loglib/output/session_bundle.hpp
    include/loglib/schema/session_view.hpp
    include/loglib/schema/source_descriptor.hpp
    include/loglib/ingest/source_plan.hpp
    include/loglib/ingest/stdin_bytes_producer.hpp
    include/loglib/ingest/stdin_peek.hpp
    include/loglib/core/stop_token.hpp
    include/loglib/core/stream_line_source.hpp
    include/loglib/ingest/tailing_bytes_producer.hpp
    include/loglib/ingest/tcp_server_producer.hpp
    include/loglib/support/theme.hpp
    include/loglib/time/time_zone_context.hpp
    include/loglib/table/timestamp_backfill.hpp
    include/loglib/time/timestamp_parsing.hpp
    include/loglib/core/transparent_string_hash.hpp
    include/loglib/ingest/udp_server_producer.hpp
    include/loglib/support/tzdata_search.hpp
    include/loglib/support/user_paths.hpp
    include/loglib/parse/worker_threads.hpp
    include/loglib/time/user_timestamp.hpp
    include/loglib/core/version.hpp
)

file(
    GLOB_RECURSE _loglib_headers_on_disk
    RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/loglib/*.hpp"
)
list(TRANSFORM _loglib_headers_on_disk REPLACE "\\\\" "/")
list(FILTER _loglib_headers_on_disk EXCLUDE REGEX "/internal/")
set(_loglib_public_headers_sorted ${LOGLIB_PUBLIC_HEADERS})
list(SORT _loglib_headers_on_disk)
list(SORT _loglib_public_headers_sorted)
if(NOT _loglib_headers_on_disk STREQUAL _loglib_public_headers_sorted)
    message(
        FATAL_ERROR
        "LOGLIB_PUBLIC_HEADERS does not match non-internal headers under include/loglib/. "
        "Update the supported header set in lib/CMakeLists.txt."
    )
endif()

target_sources(loglib PUBLIC FILE_SET HEADERS BASE_DIRS include FILES ${LOGLIB_PUBLIC_HEADERS})

target_compile_features(loglib PUBLIC cxx_std_23)

# `Version()` is the only version surface. The string is private to
# this target so dependents cannot depend on the macro name.
target_compile_definitions(loglib PRIVATE "LOGLIB_VERSION_STRING=\"${PROJECT_VERSION}\"")

target_link_libraries(
    loglib
    # tsl::robin_map appears in `enum_dictionary.hpp`; it is the remaining
    # third-party type in the supported header set. Bundled headers are
    # installed next to loglib so the exported `loglib::loglib` target
    # can compile `enum_dictionary.hpp` without a separate find_package.
    PUBLIC robin_map
    # `$<BUILD_INTERFACE:...>` keeps these off the installed link interface.
    # Private compiled archives are reattached as `$<LINK_ONLY:...>` imported
    # targets by `cmake/LoglibInstall.cmake`. Header-only private deps are
    # compiled into `loglib` and are not installed.
    # Repository warning flags stay PRIVATE so installing or consuming loglib
    # does not export this project's `project_warnings` policy to downstream
    # targets.
    # mio backs `LogFile` mapping; the public header holds an incomplete
    # `Mapping` type, so mio stays PRIVATE.
    # date/tz backs `TimeZoneContext` and timestamp parsing; no `date::`
    # type appears in supported signatures.
    # simdjson, glaze, and fmt are confined to implementation files and
    # `loglib/internal/**`.
    # TBB is an implementation detail of the parallel JSON parsing pipeline; no public
    # loglib header exposes a TBB type, so link PRIVATE. Widen to PUBLIC if a future
    # public API ever returns a TBB type.
    # efsw provides the cross-platform filesystem watcher used by TailingBytesProducer;
    # no efsw type appears in any public loglib header, so it stays PRIVATE.
    # asio is used by Tcp/UdpServerProducer; no Asio type leaks through the public
    # loglib headers (pimpl), so it stays PRIVATE.
    # PCRE2 backs the regex-template parser (RegexParser) and
    # `MakeStringMatcher`. Includes stay in `regex_parser.cpp` and
    # `log_string_matcher.cpp`; no `pcre2_*` type appears in any public
    # loglib header. PRIVATE link.
    # utf8proc decodes UTF-8 and classifies whitespace for `MakeStringMatcher`;
    # it is included only by `log_string_matcher.cpp`. PRIVATE link.
    # zlib / bzip2 / liblzma / zstd back `loglib::internal::DecompressingByteSource`
    # for transparent decompression of the four canonical codecs. All codec
    # includes are confined to `lib/src/ingest/decompressing_byte_source.cpp`;
    # no codec type appears in any public loglib header. PRIVATE link.
    PRIVATE
        $<BUILD_INTERFACE:project_warnings>
        $<BUILD_INTERFACE:mio>
        $<BUILD_INTERFACE:simdjson::simdjson>
        $<BUILD_INTERFACE:glaze::glaze>
        $<BUILD_INTERFACE:date::date>
        $<BUILD_INTERFACE:date::date-tz>
        $<BUILD_INTERFACE:fmt::fmt>
        $<BUILD_INTERFACE:TBB::tbb>
        $<BUILD_INTERFACE:efsw>
        $<BUILD_INTERFACE:asio::asio>
        $<BUILD_INTERFACE:PCRE2::pcre2-8>
        $<BUILD_INTERFACE:utf8proc::utf8proc>
        $<BUILD_INTERFACE:ZLIB::ZLIB>
        $<BUILD_INTERFACE:BZip2::BZip2>
        $<BUILD_INTERFACE:LibLZMA::LibLZMA>
        $<BUILD_INTERFACE:zstd::libzstd>
)

if(WIN32)
    # Public headers call `std::numeric_limits<T>::max()`. Windows headers
    # otherwise define `max` as a macro and break those includes.
    target_compile_definitions(loglib PUBLIC NOMINMAX)
endif()

# Optional TLS support for `TcpServerProducer`. When enabled, define
# `LOGLIB_HAS_TLS` PUBLICly so downstream targets (test/lib's TLS test,
# the GUI's NetworkStreamDialog) can compile-out their TLS branches in
# OFF builds. OpenSSL itself stays PRIVATE: only the producer's pimpl
# uses it, and no `OpenSSL::*` type appears in any public loglib header.
# The installed package calls `find_dependency(OpenSSL)` so consumers
# do not name OpenSSL in their own CMakeLists.
if(LOGLIB_NETWORK_TLS)
    target_compile_definitions(loglib PUBLIC LOGLIB_HAS_TLS)
    target_link_libraries(loglib PRIVATE $<BUILD_INTERFACE:OpenSSL::SSL> $<BUILD_INTERFACE:OpenSSL::Crypto>)
endif()

include(${CMAKE_SOURCE_DIR}/cmake/LoglibInstall.cmake)
loglib_install_library()
