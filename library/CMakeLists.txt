cmake_minimum_required(VERSION 3.19)

add_library(
    loglib
    STATIC
    src/buffering_sink.cpp
    src/json_parser.cpp
    src/key_index.cpp
    src/log_configuration.cpp
    src/log_data.cpp
    src/log_factory.cpp
    src/log_file.cpp
    src/log_line.cpp
    src/log_processing.cpp
    src/log_table.cpp
)

target_include_directories(loglib PUBLIC ./include)

target_link_libraries(
    loglib
    PUBLIC robin_map mio simdjson::simdjson glaze::glaze date::date date::date-tz fmt::fmt project_warnings
    # TBB is an implementation detail of the parallel JSON parsing pipeline; no public
    # loglib header exposes a TBB type, so link PRIVATE. Widen to PUBLIC if a future
    # public API ever returns a TBB type.
    PRIVATE TBB::tbb
)
