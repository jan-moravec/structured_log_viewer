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

| Module  | Its one job                                  | Main headers                                                                                                                                                                                 |
| ------- | -------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| core    | What a log row is, and where its bytes live  | `log_line`, `log_value`, `compact_log_value`, `key_index`, `enum_dictionary`, `line_source`, `file_line_source`, `stream_line_source`, `log_file`, `bytes_producer`, `row_batch`, `log_data` |
| schema  | What the user configured, as saveable values | `log_configuration`, `column_schema`, `filter_expression`, `highlight_rule`, `source_descriptor`, `session_view`, `anchor_entry`                                                             |
| time    | Reading timestamps and converting time zones | `time_zone_context`, `timestamp_parsing`, `user_timestamp`                                                                                                                                   |
| ingest  | Getting bytes from the outside world         | `tailing_bytes_producer`, `tcp_server_producer`, `udp_server_producer`, `stdin_bytes_producer`, `decompressing_byte_source`, `rotation_siblings`, `source_plan`                              |
| parse   | Turning bytes into rows                      | `log_parser`, `parsers/*`, `format_detection`, `regex_templates`, `log_parse_sink`                                                                                                           |
| table   | Holding rows and working out column types    | `log_table`, `table_loader`, `enum_inference`, `column_type_health`                                                                                                                          |
| query   | Finding, ordering and summarizing rows       | `log_filter`, `leaf_rule_compile`, `query_parser`, `log_string_matcher`, `log_compare`, `row_ordering`, `histogram_bucket_index`                                                             |
| output  | Writing and reading whole files of rows      | `exports/*`, `session_bundle`, `file_loading`                                                                                                                                                |
| support | Application helpers that are not about logs  | `theme`, `user_paths`, `tzdata_search`                                                                                                                                                       |

Rules that `scripts/check-loglib-internal-includes.py` enforces in pre-commit and CI:

- A file includes only its own module and the modules it may depend on. `output` also reads bundles back through `ingest`.
- Headers in `internal/` folders are for `lib/src` and `test/lib` only. The application and the command line tool must not include them.
- Public headers do not include Qt or implementation libraries such as simdjson, glaze, fmt, PCRE2 or TBB.

## Walkthrough: from a line to an exported row

1. **Choose the files.** `ingest/source_plan.hpp` turns the selected paths into the files to open, adding rotated siblings if wanted. `ingest/decompressing_byte_source.hpp` unpacks compressed files first. `output/file_loading.hpp` does this and the next step for a path in OpenFileSource, and LoadFile goes all the way to a table.
1. **Map the bytes.** `core/log_file.hpp` memory-maps the file and `core/file_line_source.hpp` serves its bytes. For a live source, `ingest/tailing_bytes_producer.hpp` or another producer delivers bytes and `core/stream_line_source.hpp` keeps the lines.
1. **Pick the format.** `parse/format_detection.hpp` sniffs the first bytes and `MakeParserForFormat` builds the parser from `parse/parsers/`.
1. **Parse.** For a file, `parse/internal/static_parser_pipeline.hpp` splits the bytes into batches and decodes them on worker threads. For a live stream, `parse/internal/streaming_parse_loop.hpp` does the same on one thread. Each line becomes a `core/log_line.hpp` made of `core/compact_log_value.hpp` fields, with keys interned by `core/key_index.hpp`. Time columns are converted on the way with `time/timestamp_parsing.hpp`. The parser does not touch the table. It writes rows, copied text and keys to three different places, described in [Data flow and threads](#data-flow-and-threads).
1. **Deliver.** `parse/internal/batch_coalescer.hpp` groups lines into `core/row_batch.hpp` batches and hands them to a `parse/log_parse_sink.hpp`.
1. **Fill the table.** The sink passes each batch to `table/log_table.hpp`, which appends it. `table/table_loader.hpp` has the sink that `slp` and LoadFile use (TableSink) and ParseIntoTable, which adds a source to a table and parses it. `table/enum_inference.hpp` turns columns with few distinct values into enumerations, and LogTable fixes time columns after the fact when a column becomes a time column.
1. **Filter and sort.** Rules from `schema/filter_expression.hpp`, or text parsed by `query/query_parser.hpp`, are compiled by `query/leaf_rule_compile.hpp` and run by `query/log_filter.hpp`. `query/row_ordering.hpp` and `query/log_compare.hpp` order the rows.
1. **Write out.** `query/column_projection.hpp` picks the visible columns, `output/exports/row_exporter.hpp` writes the rows to an `output/exports/export_sink.hpp`, and `output/session_bundle.hpp` saves a whole session.

## Data flow and threads

A parser produces three kinds of output, and each goes to a different place for a reason. Do not merge them into one path: the split is what lets other threads read rows while parsing continues.

- **Rows** go in batches (`core/row_batch.hpp`) to a LogParseSink, which usually appends them to a LogTable.
- **Copied text** is text that cannot be a view into the input, such as decoded escapes or joined continuation lines. For a file, the pipeline's last stage appends it to the byte arena of the LogFile, which never moves its bytes, so a row can point at its final offset while other threads read. For a live stream, the streaming loop hands the raw line and its copied text to StreamLineSource, which stores them under its lock.
- **Keys** go into the KeyIndex that the sink lends through Keys. It is safe to share between threads, and a key id stays valid for the life of the index.

The line offsets of a file travel in the batch and are added to the LogFile by whoever appends the batch to a LogData, because the offset vector is a plain vector and is not safe to grow while another thread reads it. Line ranges of multi-line records are registered after the offsets of the same batch.

### Which thread does what

- **Static file.** The caller of ParseStreaming for a FileLineSource runs a TBB pipeline and waits in it. One stage cuts the file into batches in order, many workers decode batches in parallel, and a last in-order stage shifts line ids to their file positions, copies text into the arena, joins continuation lines, coalesces batches and calls the sink. TBB runs that last stage on whichever thread of its pool is free, one call at a time and in file order, so a sink must not assume that OnBatch runs on the calling thread. OnStarted and OnFinished, and the batch that holds the final record of the file, are sent from the calling thread after the pipeline has drained.
- **Live stream.** The thread that calls ParseStreaming for a StreamLineSource does all the work: it reads bytes, decodes lines, stores them in the source and calls the sink. One thread, so the sink is called on that thread.
- **Consumer.** The sink is the consumer, or hands each batch to one. A GUI sink queues batches and appends them on the GUI thread. LogTable is not thread-safe, so only one thread may append to it and read it at a time.

### What a line id means

A row's line id depends on its source. For a FileLineSource it is the zero-based physical line of the first line of the record, so it indexes the line offsets of the file. For a StreamLineSource it is a one-based number that AppendLine gives to each record, and a record that spans several lines gets one id. Blank lines, failed lines and continuation lines take up physical lines in a file, and a stream has no physical lines at all. Do not turn line ids into line counts.

## Minimal usage

Typical composition:

1. Construct a `TimeZoneContext` (default UTC, or `Load(tzdataPath[, zone])`).
1. Load a file into a `LogTable` with `LoadFile`. It opens plain, compressed and bundle files, detects the format, parses and finishes type detection.
1. Or take the steps yourself: `OpenFileSource`, `MakeParserForFormat`, then `ParseIntoTable` and `FinishLoading` for each source.

`SetProcessDefaultTimeZone` is a documented process-wide convenience for applications and for formatting helpers such as `UtcMicrosecondsToDateTimeString`. The library never reads it, and `scripts/check-loglib-internal-includes.py` enforces that. A parse gets its zone as `ParserOptions::defaultSourceTimeZone`, a `LogTable` has its own display zone and default source zone, and `ParseQuery` takes a zone. All of them are UTC unless the caller sets one. Core conversion APIs take an explicit context; they do not require a prior `Initialize` call.

## Worked example: `slp`

[`cli/`](../cli) is a complete `loglib`-only host. It links no Qt target and builds with `-DSLV_BUILD_GUI=OFF`. It shows the whole composition: source resolution (`ResolveSourcePlan`), format detection, streaming parse into a `LogTable`, filter compilation (`CompileExpression`), sorting (`RowOrdering`), export sinks, session bundles, and the live producers behind `slp tail`. See the [`slp` reference](../doc/cli.md).

## API stability

Supported headers are `LOGLIB_PUBLIC_HEADERS` in [`lib/CMakeLists.txt`](CMakeLists.txt). Nothing is stable before 1.0: the C++ API, the configuration file, session bundles, regex template files and theme files may all change without migration. The formats are fixed at 1.0. There is no shared-library ABI and no vcpkg/Conan recipe.

## v1 boundary decisions

- Timezone type: `TimeZoneContext`. `Load` throws `std::runtime_error` on invalid tzdata or zone.
- `LogFile` hides the mapping backend behind an incomplete type and an out-of-line destructor.
- Configuration groups: `Column` / `ColumnType`, `Source` / `SourceLocator`, `Sort` / `SessionView`, `AnchorEntry`, `HighlightRule`. JSON remains flat on `LogConfiguration`.
- Table collaborators: `EnumInference`, `ColumnTypeHealth`, and `internal::RefreshColumnKeyIds`. `LogTable` remains the row facade.
