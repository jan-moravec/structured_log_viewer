# loglib

Qt-free structured-log core used by Structured Log Viewer. C++23, static library. Start with the module map and the walkthrough below, then read the file comment at the top of each header.

## Install

From a tree already configured and built with a CMake preset:

```sh
cmake --install build/release --prefix <prefix> --component loglib
```

`--component loglib` stages the static archive, supported headers, bundled `tsl/robin_map.h`, private link archives, and the CMake package. It does not install the GUI. Headers in `internal/` folders are not installed.

## Consume

```cmake
cmake_minimum_required(VERSION 3.28)
project(example LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
find_package(loglib CONFIG REQUIRED)
add_executable(example main.cpp)
target_link_libraries(example PRIVATE loglib::loglib)
```

Configure with `-DCMAKE_PREFIX_PATH=<prefix>`. Do not add `lib/include` or name loglib's third-party packages (`fmt`, TBB, simdjson, PCRE2, …) in the consumer `CMakeLists.txt`.

`loglib` is a static library. Private compiled archives are linked automatically as `$<LINK_ONLY:>` imported targets so vendor types stay out of the public compile interface. Header-only private dependencies (mio, glaze, asio, Howard Hinnant `date`) are compiled into the archive and are not installed. When TLS was enabled at build time, the package calls `find_dependency(OpenSSL)`; the consumer still does not name OpenSSL.

On Windows, keep `tbb12.dll` from `<prefix>/bin` on `PATH` or next to the executable. A default `TimeZoneContext` is UTC and does not need tzdata; `TimeZoneContext::Load` requires an IANA tzdata directory. A Release archive built with interprocedural optimization (MSVC `/GL`) needs LTCG at the final link.

## Module map

The library is one static target, `loglib`. Its files sit in nine module folders, `include/loglib/<module>/` and `src/<module>/`. A module may include only the modules below it in this picture.

```text
                     support      output
                                  /    \
                              query     \
                                |        \
                              table       \
                                |          \
                              parse       ingest
                             /  |  \       /
                       schema  time  \    /
                                 \    \  /
                                   core
```

| Module  | Its one job                                  | Main headers                                                                                                                                                                    |
| ------- | -------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| core    | What a log row is, and where its bytes live  | `log_line`, `log_value`, `compact_log_value`, `key_index`, `enum_dictionary`, `line_source`, `file_line_source`, `stream_line_source`, `log_file`, `bytes_producer`, `log_data` |
| schema  | What the user configured, as saveable values | `log_configuration`, `column_schema`, `filter_expression`, `highlight_rule`, `source_descriptor`, `session_view`, `anchor_entry`                                                |
| time    | Reading timestamps and converting time zones | `time_zone_context`, `timestamp_parsing`, `user_timestamp`                                                                                                                      |
| ingest  | Getting bytes from the outside world         | `tailing_bytes_producer`, `tcp_server_producer`, `udp_server_producer`, `stdin_bytes_producer`, `decompressing_byte_source`, `rotation_siblings`, `source_plan`                 |
| parse   | Turning bytes into rows                      | `log_parser`, `parsers/*`, `format_detection`, `regex_templates`, `parse_file`, `log_parse_sink`                                                                                |
| table   | Holding rows and working out column types    | `log_table`, `enum_inference`, `column_type_health`, `timestamp_backfill`                                                                                                       |
| query   | Finding, ordering and summarizing rows       | `log_filter`, `leaf_rule_compile`, `query_parser`, `log_string_matcher`, `log_compare`, `row_ordering`, `histogram_bucket_index`                                                |
| output  | Writing rows out                             | `exports/*`, `session_bundle`                                                                                                                                                   |
| support | Application helpers that are not about logs  | `theme`, `user_paths`, `tzdata_search`                                                                                                                                          |

Rules that `scripts/check-loglib-internal-includes.py` enforces in pre-commit and CI:

- A file includes only its own module and the modules it may depend on. `output` also reads bundles back through `ingest`.
- Headers in `internal/` folders are for `lib/src` and `test/lib` only. The application and the command line tool must not include them.
- Public headers do not include Qt or implementation libraries such as simdjson, glaze, fmt, PCRE2 or TBB.

## Walkthrough: from a line to an exported row

1. **Choose the files.** `ingest/source_plan.hpp` turns the selected paths into the files to open, adding rotated siblings if wanted. `ingest/decompressing_byte_source.hpp` unpacks compressed files first.
1. **Map the bytes.** `core/log_file.hpp` memory-maps the file and `core/file_line_source.hpp` serves its bytes. For a live source, `ingest/tailing_bytes_producer.hpp` or another producer delivers bytes and `core/stream_line_source.hpp` keeps the lines.
1. **Pick the format.** `parse/format_detection.hpp` sniffs the first bytes and `MakeParserForFormat` builds the parser from `parse/parsers/`.
1. **Parse.** For a file, `parse/internal/static_parser_pipeline.hpp` splits the bytes into batches and decodes them on worker threads. For a live stream, `parse/internal/streaming_parse_loop.hpp` does the same on one thread. Each line becomes a `core/log_line.hpp` made of `core/compact_log_value.hpp` fields, with keys interned by `core/key_index.hpp`. Time columns are converted on the way with `time/timestamp_parsing.hpp`.
1. **Deliver.** `parse/internal/batch_coalescer.hpp` groups lines into batches and hands them to a `parse/log_parse_sink.hpp`.
1. **Fill the table.** `table/log_table.hpp` appends each batch. `table/enum_inference.hpp` turns columns with few distinct values into enumerations, and `table/timestamp_backfill.hpp` fixes time columns after the fact.
1. **Filter and sort.** Rules from `schema/filter_expression.hpp`, or text parsed by `query/query_parser.hpp`, are compiled by `query/leaf_rule_compile.hpp` and run by `query/log_filter.hpp`. `query/row_ordering.hpp` and `query/log_compare.hpp` order the rows.
1. **Write out.** `query/column_projection.hpp` picks the visible columns, `output/exports/row_exporter.hpp` writes the rows to an `output/exports/export_sink.hpp`, and `output/session_bundle.hpp` saves a whole session.

## Minimal usage

Typical composition:

1. Construct a `TimeZoneContext` (default UTC, or `Load(tzdataPath[, zone])`).
1. Ingest with `ParseFile` / `MakeParserForFormat`.
1. Hold rows in `LogTable`.

`SetProcessDefaultTimeZone` is a documented process-wide convenience for formatting helpers such as `UtcMicrosecondsToDateTimeString`. Core conversion APIs take an explicit context; they do not require a prior `Initialize` call.

## Worked example: `slp`

[`cli/`](../cli) is a complete `loglib`-only host. It links no Qt target and builds with `-DSLV_BUILD_GUI=OFF`. It shows the whole composition: source resolution (`ResolveSourcePlan`), format detection, streaming parse into a `LogTable`, filter compilation (`CompileExpression`), sorting (`RowOrdering`), export sinks, session bundles, and the live producers behind `slp tail`. See the [`slp` reference](../doc/cli.md).

## API stability

Supported headers are `LOGLIB_PUBLIC_HEADERS` in [`lib/CMakeLists.txt`](CMakeLists.txt). Pre-v1 C++ source may still change. Persisted configuration JSON keys stay compatible. There is no shared-library ABI and no vcpkg/Conan recipe.

## v1 boundary decisions

- Timezone type: `TimeZoneContext`. `Load` throws `std::runtime_error` on invalid tzdata or zone.
- `LogFile` hides the mapping backend behind an incomplete type and an out-of-line destructor.
- Configuration groups: `Column` / `ColumnType`, `Source` / `SourceLocator`, `Sort` / `SessionView`, `AnchorEntry`, `HighlightRule`. JSON remains flat on `LogConfiguration`.
- Table collaborators: `EnumInference`, `ColumnTypeHealth`, and `internal::RefreshColumnKeyIds`. `LogTable` remains the row facade.
