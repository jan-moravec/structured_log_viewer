cmake_minimum_required(VERSION 3.28)

add_library(
    loglib
    STATIC
    src/batch_coalescer.cpp
    src/buffering_sink.cpp
    src/bytes_producer.cpp
    src/compact_log_value.cpp
    src/enum_dictionary.cpp
    src/file_identity.cpp
    src/file_line_source.cpp
    src/key_index.cpp
    src/parse_file.cpp
    src/static_parser_pipeline.cpp
    src/stream_line_source.cpp
    src/tailing_bytes_producer.cpp
    src/tcp_server_producer.cpp
    src/udp_server_producer.cpp
    src/timestamp_promotion.cpp
    src/log_configuration.cpp
    src/clang_tidy_stubs/log_configuration_glaze_meta.cpp
    src/clang_tidy_stubs/log_configuration_glaze_opts.cpp
    src/log_compare.cpp
    src/log_data.cpp
    src/log_factory.cpp
    src/log_file.cpp
    src/log_filter.cpp
    src/log_level.cpp
    src/log_line.cpp
    src/log_processing.cpp
    src/log_table.cpp
    src/parsers/csv_parser.cpp
    src/parsers/json_parser.cpp
    src/parsers/logfmt_parser.cpp
    src/regex_templates.cpp
    src/theme.cpp
    src/clang_tidy_stubs/theme_glaze_meta.cpp
)

target_include_directories(loglib PUBLIC ./include)

target_link_libraries(
    loglib
    PUBLIC robin_map mio simdjson::simdjson glaze::glaze date::date date::date-tz fmt::fmt project_warnings
    # TBB is an implementation detail of the parallel JSON parsing pipeline; no public
    # loglib header exposes a TBB type, so link PRIVATE. Widen to PUBLIC if a future
    # public API ever returns a TBB type.
    # efsw provides the cross-platform filesystem watcher used by TailingBytesProducer;
    # no efsw type appears in any public loglib header, so it stays PRIVATE.
    # asio is used by Tcp/UdpServerProducer; no Asio type leaks through the public
    # loglib headers (pimpl), so it stays PRIVATE.
    # PCRE2 backs the regex-template parser (RegexParser); its public header forward-
    # declares an opaque `RegexParserImpl` so no `pcre2_*` type appears in any public
    # loglib header. PRIVATE link.
    PRIVATE TBB::tbb efsw asio::asio PCRE2::pcre2-8
)

# Optional TLS support for `TcpServerProducer`. When enabled, define
# `LOGLIB_HAS_TLS` PUBLICly so downstream targets (test/lib's TLS test,
# the GUI's NetworkStreamDialog) can compile-out their TLS branches in
# OFF builds. OpenSSL itself stays PRIVATE: only the producer's pimpl
# uses it, and no `OpenSSL::*` type appears in any public loglib header.
if(LOGLIB_NETWORK_TLS)
    target_compile_definitions(loglib PUBLIC LOGLIB_HAS_TLS)
    target_link_libraries(loglib PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()
