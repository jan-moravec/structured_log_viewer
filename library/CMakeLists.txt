cmake_minimum_required(VERSION 3.28)

add_library(
    loglib
    STATIC
    src/batch_coalescer.cpp
    src/buffering_sink.cpp
    src/bytes_producer.cpp
    src/compact_log_value.cpp
    src/file_identity.cpp
    src/file_line_source.cpp
    src/key_index.cpp
    src/parse_file.cpp
    src/static_parser_pipeline.cpp
    src/stream_line_source.cpp
    src/tailing_bytes_producer.cpp
    src/timestamp_promotion.cpp
    src/log_configuration.cpp
    src/log_data.cpp
    src/log_factory.cpp
    src/log_file.cpp
    src/log_line.cpp
    src/log_processing.cpp
    src/log_table.cpp
    src/parsers/json_parser.cpp
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
    PRIVATE TBB::tbb efsw
)
