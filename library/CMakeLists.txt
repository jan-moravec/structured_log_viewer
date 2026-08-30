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
    src/auto_detect_parser.cpp
    src/batch_coalescer.cpp
    src/buffering_sink.cpp
    src/bytes_producer.cpp
    src/compact_log_value.cpp
    src/decompressing_byte_source.cpp
    src/enum_dictionary.cpp
    src/enum_inference.cpp
    src/exports/export_sink.cpp
    src/exports/row_exporter.cpp
    src/file_identity.cpp
    src/file_line_source.cpp
    src/format_detection.cpp
    src/histogram_bucket_index.cpp
    src/key_index.cpp
    src/parse_file.cpp
    src/static_parser_pipeline.cpp
    src/stream_line_source.cpp
    src/tailing_bytes_producer.cpp
    src/tcp_server_producer.cpp
    src/udp_server_producer.cpp
    src/timestamp_promotion.cpp
    src/log_configuration.cpp
    src/column_projection.cpp
    src/column_key_id_cache.cpp
    src/column_type_health.cpp
    src/clang_tidy_stubs/log_configuration_glaze_meta.cpp
    src/clang_tidy_stubs/log_configuration_glaze_opts.cpp
    src/log_compare.cpp
    src/log_data.cpp
    src/log_factory.cpp
    src/log_file.cpp
    src/log_filter.cpp
    src/log_level.cpp
    src/log_line.cpp
    src/log_parser.cpp
    src/log_processing.cpp
    src/time_zone_context.cpp
    src/user_timestamp.cpp
    src/log_table.cpp
    src/normalized_json_row.cpp
    src/query_parser.cpp
    src/rotation_siblings.cpp
    src/row_ordering.cpp
    src/session_bundle_writer.cpp
    src/session_bundle_reader.cpp
    src/parsers/csv_parser.cpp
    src/parsers/json_parser.cpp
    src/parsers/logfmt_parser.cpp
    src/parsers/regex_parser.cpp
    src/regex_templates.cpp
    src/stdin_bytes_producer.cpp
    src/stdin_peek.cpp
    ${REGEX_TEMPLATES_EMBEDDED_SRC}
    src/clang_tidy_stubs/regex_template_glaze_meta.cpp
    src/theme.cpp
    src/clang_tidy_stubs/theme_glaze_meta.cpp
)

# Supported v1 headers. This file set is the machine-readable public API:
# CMake install/export rules consume it, and a configure-time check below
# fails if it drifts from the non-internal headers on disk. Internal
# headers stay under include/loglib/internal/ so in-tree library and
# test/lib translation units can still include them via BASE_DIRS;
# they are not members of this set and must not be installed.
set(LOGLIB_PUBLIC_HEADERS
    include/loglib/anchor_entry.hpp
    include/loglib/ascii_case.hpp
    include/loglib/auto_detect_parser.hpp
    include/loglib/bytes_producer.hpp
    include/loglib/column_projection.hpp
    include/loglib/column_schema.hpp
    include/loglib/column_type_health.hpp
    include/loglib/compact_log_value.hpp
    include/loglib/decompressing_byte_source.hpp
    include/loglib/enum_dictionary.hpp
    include/loglib/enum_inference.hpp
    include/loglib/exports/export_sink.hpp
    include/loglib/exports/row_exporter.hpp
    include/loglib/file_line_source.hpp
    include/loglib/filter_expression.hpp
    include/loglib/format_detection.hpp
    include/loglib/highlight_rule.hpp
    include/loglib/histogram_bucket_index.hpp
    include/loglib/key_index.hpp
    include/loglib/line_source.hpp
    include/loglib/log_compare.hpp
    include/loglib/log_configuration.hpp
    include/loglib/log_data.hpp
    include/loglib/log_factory.hpp
    include/loglib/log_file.hpp
    include/loglib/log_filter.hpp
    include/loglib/log_level.hpp
    include/loglib/log_line.hpp
    include/loglib/log_parse_sink.hpp
    include/loglib/log_parser.hpp
    include/loglib/log_processing.hpp
    include/loglib/log_table.hpp
    include/loglib/log_value.hpp
    include/loglib/parse_file.hpp
    include/loglib/parser_options.hpp
    include/loglib/parsers/csv_parser.hpp
    include/loglib/parsers/json_parser.hpp
    include/loglib/parsers/logfmt_parser.hpp
    include/loglib/parsers/regex_parser.hpp
    include/loglib/query_parser.hpp
    include/loglib/regex_templates.hpp
    include/loglib/rotation_siblings.hpp
    include/loglib/row_ordering.hpp
    include/loglib/session_bundle.hpp
    include/loglib/session_view.hpp
    include/loglib/source_descriptor.hpp
    include/loglib/stdin_bytes_producer.hpp
    include/loglib/stdin_peek.hpp
    include/loglib/stop_token.hpp
    include/loglib/stream_line_source.hpp
    include/loglib/tailing_bytes_producer.hpp
    include/loglib/tcp_server_producer.hpp
    include/loglib/theme.hpp
    include/loglib/time_zone_context.hpp
    include/loglib/transparent_string_hash.hpp
    include/loglib/udp_server_producer.hpp
    include/loglib/user_timestamp.hpp
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
        "Update the supported header set in library/CMakeLists.txt."
    )
endif()

target_sources(loglib PUBLIC FILE_SET HEADERS BASE_DIRS include FILES ${LOGLIB_PUBLIC_HEADERS})

target_compile_features(loglib PUBLIC cxx_std_23)

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
    # PCRE2 backs the regex-template parser (RegexParser). All PCRE2 includes
    # are confined to `library/src/parsers/regex_parser.cpp`; no `pcre2_*`
    # type appears in any public loglib header. PRIVATE link.
    # zlib / bzip2 / liblzma / zstd back `loglib::internal::DecompressingByteSource`
    # for transparent decompression of the four canonical codecs. All codec
    # includes are confined to `library/src/decompressing_byte_source.cpp`;
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

include(${CMAKE_SOURCE_DIR}/cmake/LoglibHeaderCheck.cmake)
loglib_add_public_header_check()

include(${CMAKE_SOURCE_DIR}/cmake/LoglibInstall.cmake)
loglib_install_library()
