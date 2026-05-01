# Contributing

Thanks for your interest in contributing! This document is the developer reference for the project. It opens with an architecture deep-dive (project layout, the `loglib` headers, the GUI wrappers, the parsing data flow, and how to teach the viewer a new log format), then covers building from source, running the test suite and benchmarks, code style, the pull-request workflow, and the release process.

## Table of contents

- [Architecture](#architecture)
  - [Project Structure](#project-structure)
  - [Library](#library)
  - [GUI Application](#gui-application)
  - [Data Flow](#data-flow)
  - [Adding a New Structured Log Format](#adding-a-new-structured-log-format)
- [Prerequisites](#prerequisites)
- [Building](#building)
  - [Quick start](#quick-start)
  - [Windows](#windows)
  - [Machine-specific overrides (`CMakeUserPresets.json`)](#machine-specific-overrides-cmakeuserpresetsjson)
  - [Enabling system packages](#enabling-system-packages)
  - [IDE integration](#ide-integration)
- [Running tests](#running-tests)
  - [Targets](#targets)
  - [Common pitfalls](#common-pitfalls)
- [Benchmarking](#benchmarking)
  - [Fixture inventory](#fixture-inventory)
  - [Running](#running)
  - [WARN-line convention](#warn-line-convention)
  - [Acceptance bar](#acceptance-bar)
- [Code style and pre-commit](#code-style-and-pre-commit)
- [Pull requests](#pull-requests)
- [Release process](#release-process)
  - [Steps](#steps)
  - [Verifying a release](#verifying-a-release)
  - [AppImage delta updates (zsync)](#appimage-delta-updates-zsync)
  - [Hotfix / re-tagging](#hotfix--re-tagging)

## Architecture

### Project Structure

The project is organized into two main components: the `library` and the GUI `app`.

```plaintext
structured_log_viewer/
├── library/                  # loglib: core log handling (no Qt dependency)
│   ├── include/loglib/       # Public library headers
│   │   └── internal/         # Internal-but-needed-by-tests headers
│   │                         #   (parser_pipeline.hpp, buffering_sink.hpp,
│   │                         #    timestamp_promotion.hpp,
│   │                         #    transparent_string_hash.hpp,
│   │                         #    parser_options.hpp)
│   ├── src/                  # Library implementation (.cpp only)
│   └── CMakeLists.txt
├── app/                      # Qt6 GUI application (StructuredLogViewer)
│   ├── include/              # GUI headers
│   ├── src/                  # GUI implementation (including main_window.ui)
│   └── CMakeLists.txt
├── test/
│   ├── lib/                  # Catch2 unit tests and benchmarks for loglib
│   ├── app/                  # Qt Test smoke tests for MainWindow (+ JSONL fixtures)
│   ├── common/               # Test helpers shared across lib/app/log_generator
│   └── log_generator/        # Standalone JSONL fixture generator (`log_generator`)
├── cmake/                    # Shared CMake modules (warnings, FetchContent)
├── resources/                # Icons, .desktop entry, Qt resource file
├── doc/                      # End-user documentation
├── .github/workflows/        # CI: build + test on Linux / Windows / macOS
├── CMakeLists.txt
└── README.md
```

### Library

The `library` component (`loglib`) provides the core functionality for handling structured log data and has no Qt dependency, so it can be reused in other applications. The headers are intentionally small; the table below is the recommended reading order for someone new to the codebase.

| Header                          | Role                                                                                                                                                                                                                                                                                                      |
| ------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `loglib/log_file.hpp`           | `LogFile` memory-maps a log on disk and tracks line offsets. It owns the mmap for its full lifetime so `LogValue`s elsewhere can hold `string_view`s into it. `LogFileReference` is a `(LogFile*, line)` pair attached to every parsed line.                                                              |
| `loglib/key_index.hpp`          | `KeyIndex` is an append-only intern table mapping a JSON field name (`"timestamp"`, `"level"`, …) to a dense `KeyId`. It is thread-safe; the parsing pipeline shares a single `KeyIndex` across all workers and the GUI's `LogTable` keeps it for the table's lifetime.                                   |
| `loglib/log_line.hpp`           | `LogValue` is the per-field `std::variant` (`string_view`, `string`, `int64_t`, `uint64_t`, `double`, `bool`, `TimeStamp`, `monostate`). `LogLine` holds one record as a sorted vector of `(KeyId, LogValue)` plus its `LogFileReference`. `string_view` values point straight into the `LogFile`'s mmap. |
| `loglib/log_data.hpp`           | `LogData` owns the `KeyIndex`, all `LogLine`s, and the `LogFile`(s) they reference. It supports `Merge` for opening multiple files and `AppendBatch` for the streaming path.                                                                                                                              |
| `loglib/log_configuration.hpp`  | `LogConfiguration` describes the visible columns (header, JSON keys, print format, `Type::any` vs `Type::time`, time-parse formats, filters). `LogConfigurationManager` loads/saves it and grows the layout during streaming via `AppendKeys`.                                                            |
| `loglib/log_table.hpp`          | `LogTable` pairs a `LogData` with a `LogConfigurationManager` and resolves what to render in cell `(row, column)`. It owns the `BeginStreaming` / `AppendBatch` state machine that the GUI model drives, and back-fills timestamp columns mid-stream as new keys appear.                                  |
| `loglib/log_processing.hpp`     | Timezone bootstrap (`Initialize`), `TryParseTimestamp` fast/slow paths, and the `BackfillTimestampColumn` helper used by `LogTable`.                                                                                                                                                                      |
| `loglib/log_parser.hpp`         | `LogParser` is the format-agnostic interface (`IsValid`, `ParseStreaming`, `ToString`). The synchronous `Parse(path)` overload is implemented on the base class and routes through `ParseStreaming`.                                                                                                      |
| `loglib/streaming_log_sink.hpp` | `StreamingLogSink` is the receiver interface for the streaming parser; one `OnStarted`, zero or more `OnBatch(StreamedBatch)`, one `OnFinished(cancelled)`.                                                                                                                                               |
| `loglib/parser_options.hpp`     | `ParserOptions::stopToken` for cooperative cancellation and `configuration` for inline timestamp promotion. Test-only tuning knobs (thread cap, batch size) live behind `loglib/internal/parser_options.hpp`.                                                                                             |
| `loglib/json_parser.hpp`        | `JsonParser`: the only currently shipped `LogParser`. Built on simdjson + the shared TBB pipeline.                                                                                                                                                                                                        |
| `loglib/log_factory.hpp`        | `LogFactory` picks the right parser for a path via `IsValid` and exposes a typed `Create(Parser)` enum.                                                                                                                                                                                                   |

Several helpers live under `library/include/loglib/internal/` and are intentionally **not** part of the public API. They are kept there (rather than next to the `.cpp`s in `library/src/`) so the unit tests in `test/lib/` can include them via the same `loglib`-prefixed include path as the public headers, without needing a per-target include-directory workaround. The most important are `BufferingSink` (`buffering_sink.hpp`, the sink behind synchronous `Parse(path)`) and `loglib::detail::RunParserPipeline` (`parser_pipeline.hpp`, the shared TBB pipeline harness — see [Data Flow](#data-flow)). `timestamp_promotion.hpp` and `transparent_string_hash.hpp` round out the set.

### GUI Application

The `app` component is a Qt 6 Widgets application. It uses `loglib` for parsing and data management and exposes the data through `QAbstractTableModel`/`QSortFilterProxyModel` subclasses with support for sorting, filtering, searching, and configurable columns.

The Qt-side classes that wrap `loglib` are:

- `LogModel` (`app/include/log_model.hpp`) — a `QAbstractTableModel` that owns the `LogTable`. It exposes `BeginStreaming` / `AppendBatch` / `EndStreaming` for the streaming path and `AddData` for the synchronous path, and emits `lineCountChanged` / `errorCountChanged` / `streamingFinished` so the main window's status bar can tick along while parsing.
- `QtStreamingLogSink` (`app/include/qt_streaming_log_sink.hpp`) — the bridge that turns a `loglib::StreamingLogSink` callback running on a TBB worker thread into a `LogModel::AppendBatch` call on the GUI thread (`Qt::QueuedConnection`). It owns a generation counter so a fresh `BeginParse()` (or a `RequestStop()`) drops any still-queued batches from a previous parse.
- `LogFilterModel` (`app/include/log_filter_model.hpp`) — the `QSortFilterProxyModel` over `LogModel` that implements the multi-column filter set described in the [user guide](doc/README.md#filtering).
- `MainWindow` (`app/include/main_window.hpp`) — orchestrates everything: open dialogs, drag & drop, the find bar, the filter editor, the preferences editor, and (for single-file JSON opens) the streaming pipeline orchestration in `OpenJsonStreaming`.

### Data Flow

A JSON log line takes the same path through the codebase whether you opened the file from `File → Open…` or are running a Catch2 benchmark:

```text
                    mmap                          per-batch
LogFile  ──▶  Stage A  ──▶  Stage B  ──▶  Stage C  ──▶  StreamingLogSink
                                                              │
                                                ┌─────────────┴──────────────┐
                                                ▼                            ▼
                                          BufferingSink              QtStreamingLogSink
                                          (sync Parse)               (Qt::QueuedConnection)
                                                                            │
                                                                            ▼
                                                                  LogModel::AppendBatch
                                                                            │
                                                                            ▼
                                                                 LogTable::AppendBatch
                                                                  • extends LogConfiguration
                                                                    with newKeys
                                                                  • back-fills any newly
                                                                    detected time columns
                                                                            │
                                                                            ▼
                                                        beginInsertColumns / beginInsertRows
                                                        + dataChanged on back-filled cells
                                                                            │
                                                                            ▼
                                                          LogFilterModel (sort + filter proxy)
                                                                            │
                                                                            ▼
                                                                  LogTableView (Qt widget)
```

1. **`LogFile` opens the file.** A `LogFile` mmaps the file and remembers the byte offset of every line boundary as the parser visits them. Because the mmap stays alive for the file's whole on-screen lifetime, every `LogLine` parsed downstream can hold `string_view`s pointing directly into it.
1. **The shared TBB pipeline parses it.** `LogParser::ParseStreaming` is implemented on top of `loglib::detail::RunParserPipeline` (`library/include/loglib/internal/parser_pipeline.hpp`), a three-stage `oneapi::tbb::parallel_pipeline`:
   - **Stage A (`serial_in_order`)** carves the mmap into ~1 MiB byte-range tokens at line boundaries.
   - **Stage B (`parallel`)** decodes each token into a `ParsedPipelineBatch` of `LogLine`s. Field names are interned through a per-worker cache that hits the shared `KeyIndex` only on first sight, and configured `Type::time` columns are promoted to a `TimeStamp` inline (`PromoteLineTimestamps`) while the freshly-written values are still hot in L1.
   - **Stage C (`serial_in_order`)** assigns absolute line numbers, coalesces small batches (~1000 lines or 50 ms), diffs the `KeyIndex` to find newly seen keys, and emits a `StreamedBatch` to the sink.
     The pipeline owns `ParserOptions::stopToken` cancellation, batch coalescing, and the new-keys diff. A format-specific parser only supplies the Stage A/B lambdas (see [Adding a New Structured Log Format](#adding-a-new-structured-log-format)).
1. **A sink consumes the batches.** Two `StreamingLogSink` implementations ship today:
   - `BufferingSink` (private, declared in `library/include/loglib/internal/buffering_sink.hpp`) is the sink behind the synchronous `LogParser::Parse(path)` overload. It accumulates every batch into a single `LogData` and is what tests, benchmarks, and the GUI's fall-back synchronous path use.
   - `QtStreamingLogSink` is the GUI bridge. Its `OnBatch(StreamedBatch)` posts a `QMetaObject::invokeMethod` lambda back to the GUI thread, which drops on a generation mismatch (see below) and otherwise calls `LogModel::AppendBatch`.
1. **`LogModel::AppendBatch` updates the table.** It hands the batch to `LogTable::AppendBatch`, which:
   - extends the `LogConfiguration` with any `batch.newKeys` (auto-promoting names that look like timestamps to `Type::time`),
   - back-fills any *newly* introduced time column over **all** rows so users never see a half-parsed timestamp column,
   - and reports the column range it back-filled via `LastBackfillRange()` so the model emits a single `dataChanged` for those cells.
     The model then emits `beginInsertColumns` / `beginInsertRows` for the rows and columns that grew, plus `lineCountChanged` / `errorCountChanged` so `MainWindow` can tick the status-bar label `Parsing <file> — N lines, M errors`.
1. **The view renders the rows.** `LogFilterModel` (`QSortFilterProxyModel`) applies the active filters, and `LogTableView` renders the surviving rows. The `Edit → Copy` shortcut pulls the original JSON text via `LogFileReference::GetLine` so the row round-trips back to disk format.

A few cross-cutting invariants hold this together:

- **The `LogFile` outlives every `LogLine` that quotes it.** That is why `LogModel::Clear()` and `~LogModel()` block on the streaming `QFuture` before tearing down the `LogTable`: the cooperative `stop_token` only halts Stage A, but Stage B tasks already in flight keep reading from the mmap.
- **The parser sees a snapshot of the configuration.** `MainWindow::OpenJsonStreaming` snapshots `LogConfiguration` into a `shared_ptr<const>` and gates the configuration menus for the duration of the parse, so a configuration edit racing past the UI gate cannot still affect the in-flight parse.
- **`KeyIndex` is the single source of truth for column identity** across the streaming workers, the `LogConfigurationManager`, and `LogTable`'s back-fill logic. It is append-only and assigns dense ids, so it doubles as an index into per-key arrays.

### Adding a New Structured Log Format

To teach the viewer a new format (CSV, logfmt, syslog, …):

1. **Subclass `loglib::LogParser`.** Add `library/include/loglib/<format>_parser.hpp` for the public surface and `library/src/<format>_parser.cpp` for the implementation, then implement:

   - `bool IsValid(const std::filesystem::path &) const` — a cheap content sniff (read one or two leading lines). `LogFactory::Parse` calls this in order to auto-detect the format, so it must be fast and false-positive-free.
   - `void ParseStreaming(LogFile&, StreamingLogSink&, ParserOptions) const` — the streaming entry point.
   - `std::string ToString(const LogLine&) const` — the inverse, used by `Edit → Copy` to round-trip a row back to the format's native text.

1. **Reuse the shared pipeline** in `ParseStreaming`. Almost all of the heavy lifting lives in `loglib::detail::RunParserPipeline` (`library/include/loglib/internal/parser_pipeline.hpp`). Define two types and two lambdas, then call:

   ```cpp
   detail::RunParserPipeline<Token, UserState>(file, sink, options, advanced, stageA, stageB);
   ```

   Where:

   - `Token` is your Stage A unit of work (e.g. `JsonByteRange` for `JsonParser` — typically a `[bytesBegin, bytesEnd)` slice of the mmap).
   - `UserState` is the format-specific per-worker scratch (e.g. a `simdjson::ondemand::parser` and its padded buffer for JSON, a CSV row-splitter for CSV). It is bolted onto `WorkerScratchBase`, which already provides the per-worker `KeyIndex` cache and timestamp-parse scratch.
   - `stageA(Token& out) -> bool` produces the next token from the source; return `false` at EOF.
   - `stageB(Token, WorkerScratch<UserState>&, KeyIndex&, std::span<const TimeColumnSpec>, ParsedPipelineBatch&)` decodes one token into `parsed.lines` (built via `LogLine{sortedValues, keys, fileRef}`) and `parsed.errors`. After pushing each line, call `worker.PromoteTimestamps(parsed.lines.back(), timeColumns)` to keep the inline timestamp fast path warm. Use `detail::InternKeyVia(...)` to intern field names through the per-worker cache. Set `parsed.totalLineCount` to the number of source lines consumed (parsed + errored + skipped) so Stage C can advance its line-number cursor.

   The harness owns batch coalescing, the `newKeys` diff, line-number assignment, and stop-token handling — your parser only has to turn bytes into `(KeyId, LogValue)` pairs.

1. **Register the parser in `LogFactory`** (`library/include/loglib/log_factory.hpp` + `library/src/log_factory.cpp`):

   - Add a value to `LogFactory::Parser` (before `Count`).
   - Extend `LogFactory::Create` to instantiate it.
     Files that match `IsValid` are then auto-detected by `File → Open…`.

1. **Optional GUI affordance.** If you want a "force this parser" entry like `File → Open JSON Logs…`, add a `QAction` in `app/src/main_window.cpp` that calls `OpenFilesWithParser("…", std::make_unique<YourParser>())`. The streaming-on-the-GUI-thread path (`MainWindow::OpenJsonStreaming`) is currently wired specifically to `JsonParser`; either generalise that branch or fall back to the synchronous `LogParser::Parse(path)` path (which still routes through your `ParseStreaming` via the internal `BufferingSink`).

1. **Tests.** Add Catch2 unit tests under `test/lib/` mirroring `test/lib/src/test_json_parser.cpp`, and pipeline tests under `test/lib/src/test_parser_pipeline.cpp` if you want coverage of the cancellation / coalescing / new-keys paths. For Qt Test smoke coverage, add fixtures under `test/app/fixtures/` and reference them from `fixtures.qrc`. The shared helpers in `test/common/` are useful for generating synthetic data.

Timestamp parsing/back-fill, key interning, column-layout updates, and Qt-side bridging fall out of the existing infrastructure as long as your `Stage B` lambda emits `LogLine`s.

## Prerequisites

- **CMake** 3.28 or newer
- **Qt** 6.1 or newer (Qt 6.8 is used in CI)
- A C++23-capable toolchain:
  - Linux: GCC 13+ or Clang 17+
  - Windows: MSVC 2022 (Visual Studio 2022)
  - macOS: Xcode 15+ / Apple Clang

Most third-party C++ dependencies (`date`, `fmt`, `Catch2`, `mio`, `glaze`, `simdjson`, `robin-map`) are fetched automatically via `FetchContent`. To use system copies instead, pass the corresponding option when configuring — one of `USE_SYSTEM_DATE`, `USE_SYSTEM_FMT`, `USE_SYSTEM_CATCH2`, `USE_SYSTEM_MIO`, `USE_SYSTEM_GLAZE`, `USE_SYSTEM_SIMDJSON`, `USE_SYSTEM_ROBIN_MAP` (e.g. `-DUSE_SYSTEM_FMT=ON`). See [`cmake/FetchDependencies.cmake`](cmake/FetchDependencies.cmake) for the pinned versions.

`loglib` also links against [oneTBB](https://github.com/uxlfoundation/oneTBB) (Intel oneAPI Threading Building Blocks) for the parallel JSON parsing pipeline. It is fetched at version `v2022.3.0` by default; pass `-DUSE_SYSTEM_TBB=ON` to use a system installation, which must be **>= 2021.5** (the first oneAPI-style release that ships `tbb::filter_mode` / `tbb::parallel_pipeline`). On Windows, the build copies `tbb12.dll` next to `StructuredLogViewer.exe` and the test binaries automatically (both into the build tree and the install tree) — keep it next to the executable when redistributing.

## Building

All build configurations are defined in [`CMakePresets.json`](CMakePresets.json) (CMake 3.28+). The shared presets are:

| Preset           | Build type       | Purpose                                    |
| ---------------- | ---------------- | ------------------------------------------ |
| `release`        | `Release`        | Optimized build, used by CI and releases.  |
| `debug`          | `Debug`          | Full debug info and assertions.            |
| `relwithdebinfo` | `RelWithDebInfo` | Release optimizations + debug info (perf). |

Each preset uses the **Ninja** generator and writes to `build/<presetName>/`. They also enable `CMAKE_EXPORT_COMPILE_COMMANDS` so `clangd`, `clang-tidy`, and other tools work out of the box. Matching `buildPresets`, `testPresets`, and `workflowPresets` are defined with the same names. Two extra benchmark-only test presets — `release-benchmark` and `relwithdebinfo-benchmark` — opt into the long-running parser benchmarks (see [Benchmarking](#benchmarking)).

### Quick start

Configure, build, and test in one shot:

```sh
cmake --workflow --preset release
```

Or run the steps individually (handy for iterative development — CI does this too):

```sh
cmake --preset release             # configure
cmake --build --preset release     # build
ctest --preset release             # unit + Qt smoke tests (skips benchmarks)
ctest --preset release-benchmark   # opt-in: parser benchmarks only, verbose
```

### Windows

Run the same commands from the **Developer PowerShell for VS 2022** (or Developer Command Prompt) so that `cl.exe` and Ninja are on `PATH`.

### Machine-specific overrides (`CMakeUserPresets.json`)

For paths that differ between machines (typically your Qt install), add a `CMakeUserPresets.json` at the repo root — it is gitignored and merged with `CMakePresets.json` automatically. Example:

```json
{
    "version": 6,
    "cmakeMinimumRequired": { "major": 3, "minor": 25 },
    "include": ["CMakePresets.json"],
    "configurePresets": [
        {
            "name": "local",
            "inherits": "release",
            "cacheVariables": {
                "CMAKE_PREFIX_PATH": "C:/Qt/6.8.0/msvc2022_64"
            }
        }
    ],
    "buildPresets": [{ "name": "local", "configurePreset": "local" }],
    "testPresets":  [
        { "name": "local",           "configurePreset": "local", "inherits": "release"           },
        { "name": "local-benchmark", "configurePreset": "local", "inherits": "release-benchmark" }
    ],
    "workflowPresets": [
        {
            "name": "local",
            "steps": [
                { "type": "configure", "name": "local" },
                { "type": "build",     "name": "local" },
                { "type": "test",      "name": "local" }
            ]
        }
    ]
}
```

Inheriting from `release` / `release-benchmark` picks up the shared `output.outputOnFailure`, `QT_QPA_PLATFORM=offscreen`, and the `benchmark` label filter (plus the verbose output that only `release-benchmark` enables) — `cmake --workflow --preset local` runs the unit + Qt smoke tests against your local Qt, and `ctest --preset local-benchmark` is the matching opt-in benchmark run.

### Enabling system packages

To use system copies of dependencies instead of `FetchContent`, pass the relevant option at configure time — for example:

```sh
cmake --preset release -DUSE_SYSTEM_FMT=ON -DUSE_SYSTEM_SIMDJSON=ON
```

The full list of `USE_SYSTEM_*` options is in [`cmake/FetchDependencies.cmake`](cmake/FetchDependencies.cmake).

### IDE integration

Qt Creator, CLion, Visual Studio, and VS Code (with the CMake Tools extension) all detect `CMakePresets.json` / `CMakeUserPresets.json` automatically — just open the repository folder and pick a preset.

## Running tests

The test suite is registered with [CTest](https://cmake.org/cmake/help/latest/manual/ctest.1.html) and built by the workflow preset alongside the binaries. The `release` test preset enables `--output-on-failure`, pins `QT_QPA_PLATFORM=offscreen` so the Qt smoke tests stay headless, and **filters out the `benchmark` CTest label**. `cmake --workflow --preset release` therefore configures, builds, and runs the unit + Qt smoke tests but skips the long-running parser benchmarks; opt into those via the matching `release-benchmark` preset (see [Benchmarking](#benchmarking)).

```sh
cmake --workflow --preset release        # configure + build + unit tests (no benchmarks)
```

Re-run just the tests against an existing build:

```sh
ctest --preset release                   # unit + Qt smoke (excludes benchmark label)
ctest --preset release-benchmark         # benchmarks only, verbose (see Benchmarking)
ctest --preset release -R log_table      # filter by registered test name
```

The `release` test preset stays quiet on success (only `--output-on-failure` is set) so the workflow output stays scannable. The benchmark presets enable `output.verbosity = verbose` so the WARN-line throughput / latency numbers always show up in the log; pass `-V` explicitly if you want the same firehose output for a unit-test run.

The two presets are intentionally split because `ctest --preset release -L benchmark` would **not** flip the `release` preset's `filter.exclude.label = benchmark` off — `-L` only overrides the *include* label per [`cmake-presets(7)`](https://cmake.org/cmake/help/latest/manual/cmake-presets.7.html#test-preset), so benchmarks would still be skipped. Use the `release-benchmark` (or `relwithdebinfo-benchmark`) preset instead.

### Targets

| Target          | Coverage                                                                                                                                                                                                                                                                                                                           |
| --------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `tests`         | Catch2 unit tests for `loglib` (parsers, the streaming pipeline, `LogTable`, `LogConfiguration`, `KeyIndex`, timestamp parsing) **and** the parser/lookup micro-benchmarks. The benchmark cases are tagged `[.][benchmark]` so they're hidden by default; `catch_discover_tests` registers them under the `benchmark` CTest label. |
| `apptest`       | Qt Test smoke tests that drive a real `MainWindow` in offscreen mode against the JSONL fixtures embedded in `test/app/fixtures/`. Registered as a single CTest entry.                                                                                                                                                              |
| `log_generator` | Standalone JSONL fixture generator (manual UI smoke testing); not run by CTest. Supports `--lines` / `--size` stop conditions, `--timeout` throttling, and in-flight `--roll-strategy {rename,copytruncate,truncate}` for driving Stream Mode rotation tests against `TailingFileSource` (PRD 4.8.6).                              |

### Common pitfalls

- The Qt smoke tests resolve `tzdata/` relative to the test binary's directory (the same layout the production install uses). The CMake build copies `tzdata/` next to `apptest` automatically; if you move the binary, copy `tzdata/` along with it or the loader will pop a `QMessageBox::critical` on startup.
- `tests` registers each Catch2 case individually with CTest via `catch_discover_tests`, which runs `tests --list-test-names-only` at build time. If that invocation fails (e.g. because the TBB DLL is not yet next to `tests.exe` on Windows), CTest reports `No tests were found!!!` rather than a build error — fix the underlying build issue and reconfigure rather than ignoring the warning.

## Benchmarking

This section is the canonical home for the regression gate; the parser benchmarks in [`test/lib/src/benchmark_json.cpp`](test/lib/src/benchmark_json.cpp) link back to it via their top-of-file comment. Quote the bullets in the [Acceptance bar](#acceptance-bar) below in commit messages and PR descriptions.

The benchmarks live in the same `tests` binary as the unit tests but are tagged `[.][benchmark]` so they're hidden by default; `catch_discover_tests` registers them under the `benchmark` CTest label.

**Debug builds skip benchmarks automatically.** Use the `release` preset for the canonical regression-gate number, or `relwithdebinfo` if you want to attach a profiler — both have IPO/LTO on and `NDEBUG` defined. Running benchmarks against `--preset debug` reports each case as `SKIPPED` with a message rather than producing misleading numbers (see `BENCHMARK_REQUIRES_RELEASE_BUILD` in `benchmark_json.cpp`).

### Fixture inventory

Seven `[.][benchmark]…` cases ship today:

| Tag                 | Coverage                                                                                                                                                              |
| ------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `[parse_sync]`      | Synchronous parse coverage (10'000 lines), exercises the `BufferingSink` path.                                                                                        |
| `[large]`           | Streaming-to-`LogTable` flow, 1'000'000 lines (~170 MB). End-to-end GUI benchmark.                                                                                    |
| `[wide]`            | Streaming-to-`LogTable`, 200'000 wide rows (~30 fields/line). Stresses per-line field iteration (`InsertSorted`, `ExtractFieldKey`, `ParseLine`, `IsKeyInAnyColumn`). |
| `[get_value_micro]` | `LogLine::GetValue` slow-path (string lookup) vs fast-path (`KeyId` lookup).                                                                                          |
| `[allocations]`     | `string_view` fast-path fraction over a 1'000-line parse. The test itself only asserts `stringViewValues > 0`; the ≥ 99 % bar is the PR-description convention.       |
| `[cancellation]`    | Cancellation-latency over 20 runs of a 1M-line parse. The test hard-fails only above 5 s; the ±3 % p95 bar is the PR-description convention.                          |
| `[stream_latency]`  | Stream-Mode write-to-row latency over a `TailingFileSource` + `JsonParser::ParseStreaming` chain. Asserts median ≤ 250 ms / p95 ≤ 500 ms (PRD §8 success metric 1).   |

### Running

Benchmarks are tagged `[.][benchmark]` in Catch2 and registered under the `benchmark` CTest label. The `release` test preset filters that label out, so they only run when you explicitly opt in via the dedicated `release-benchmark` test preset (or its `relwithdebinfo-benchmark` sibling, useful when attaching a profiler). Both presets bake in `output.verbosity = verbose`, so the `WARN` lines with throughput and latency show up in the CTest log without needing `-V`:

```sh
ctest --preset release-benchmark            # all parser benchmarks, verbose
ctest --preset release-benchmark -R large   # one fixture, by CTest-registered name regex
ctest --preset relwithdebinfo-benchmark     # release optimisations + debug info
```

Catch2-direct invocations with `--reporter compact` for tight ad-hoc sweeps (no preset wrapping; you choose the tags):

```sh
build/release/bin/Release/tests "[benchmark]" --reporter compact
build/release/bin/Release/tests "[large],[wide]" --reporter compact   # regression-gating subset
build/release/bin/Release/tests "[large]" --reporter compact          # one fixture
```

The path above assumes the `release` preset on Linux/macOS; on multi-config generators (Visual Studio) substitute the active configuration for `Release`. The Catch2 hidden-by-default tag (`[.]`) requires an explicit filter — picking any tag is enough.

### WARN-line convention

Throughput, fast-path fraction, and cancellation latency are reported via Catch2's `WARN` macro so they appear in the output even on success. The streaming-to-`LogTable` cases (`[large]`, `[wide]`) emit three `WARN` lines per case via `RunStreamingBenchmark`:

1. **Warm-up MB/s** — single cold-cache run; informative context but **not** the regression-gate number.
1. **`LogTable::AppendBatch` wall-time per 100 k lines** — the GUI-thread cost of consuming the streamed batches (printed once, sourced from the warm-up run).
1. **`RunTimedSamples` summary** — mean / low / high / stddev MB/s and lines/s across N samples (4 for the heavy fixtures).

The **steady-state MB/s mean from `RunTimedSamples` — not the warm-up MB/s** — is the canonical regression-gate input. The warm-up's run-to-run variance from cache and scheduling effects easily masks single-digit-percent code changes, which is why the gate is anchored on the timed samples.

### Acceptance bar

> **Note:** the bar below is a **manual review convention**, not an automated CI gate. The CI `release-benchmark` step runs the benchmarks and prints the WARN lines; reviewers (and the PR author, in the description / commit message) compare the printed numbers against the prior commit's. There is no script that fails the build on a regression — a machine-readable CSV emitter that wires into a CI compare-against-baseline step is filed as a follow-up. Until that lands, please paste the relevant WARN lines into your PR description so the reviewer can spot a regression at a glance.

The convention is to capture both **before** (clean-tree baseline) and **after** (your branch tip) numbers for each gating fixture and quote them in the commit message or PR description:

- `[large]` and `[wide]` — steady-state MB/s mean within ±3 % of the prior commit's number, or a documented architectural justification.
- `[allocations]` — `string_view` fast-path fraction ≥ 99 %.
- `[cancellation]` — p95 latency within ±3 % of the prior commit's number.
- `[stream_latency]` — median ≤ 250 ms and p95 ≤ 500 ms (the test fails on a regression rather than relying on a manual review compare). Stream-Mode PRs must also re-run `[large]` / `[wide]` / `[allocations]` / `[cancellation]` and record the numbers, since the static-path machinery and the Stream-Mode seam share the same parser and `LogTable` plumbing.

## Code style and pre-commit

All C/C++ sources are formatted with **clang-format** and CMake files with **cmake-format**. These are enforced via pre-commit hooks:

```sh
pip install pre-commit
pre-commit install
```

Manually format the whole tree:

```sh
pre-commit run --all-files
```

The pinned tool versions live in [`.pre-commit-config.yaml`](.pre-commit-config.yaml).

## Pull requests

1. Fork the repository and create a topic branch from `main`.
1. Keep commits focused; rebase onto `main` before opening a PR.
1. Ensure `pre-commit run --all-files` passes locally.
1. Make sure CI (Linux, Windows, macOS) is green — tests and packaging both must succeed.
1. Describe the motivation and user-visible changes in the PR description.

## Release process

Releases are cut from tags matching `v*` (e.g. `v0.7.0`). The [`Build` workflow](.github/workflows/build.yml) detects tag pushes, packages artifacts for all platforms, and attaches them to a GitHub Release.

### Steps

1. Make sure `main` is green and all desired changes are merged.

1. Bump the version in the top-level [`CMakeLists.txt`](CMakeLists.txt):

   ```cmake
   project(
       structured_log_viewer
       VERSION 0.7.0
       LANGUAGES CXX)
   ```

1. Commit the bump and open a PR; merge once CI passes.

1. Tag the merge commit and push the tag:

   ```sh
   git checkout main
   git pull
   git tag -a v0.7.0 -m "Release v0.7.0"
   git push origin v0.7.0
   ```

1. The `Build` workflow will run on the tag and, on success, attach the following assets to the GitHub Release named after the tag:

   | Platform | Artifact                                    | Checksum                                           |
   | -------- | ------------------------------------------- | -------------------------------------------------- |
   | Linux    | `StructuredLogViewer-x86_64.AppImage`       | `StructuredLogViewer-x86_64.AppImage.sha256`       |
   | Linux    | `StructuredLogViewer-x86_64.AppImage.zsync` | `StructuredLogViewer-x86_64.AppImage.zsync.sha256` |
   | Windows  | `StructuredLogViewer.zip`                   | `StructuredLogViewer.zip.sha256`                   |
   | macOS    | `StructuredLogViewer.dmg`                   | `StructuredLogViewer.dmg.sha256`                   |

1. Edit the auto-created GitHub Release to add release notes (highlights, breaking changes, known issues) and publish it.

### Verifying a release

End users can verify a download with:

```sh
# Linux
sha256sum -c StructuredLogViewer-x86_64.AppImage.sha256

# macOS
shasum -a 256 -c StructuredLogViewer.dmg.sha256

# Windows (PowerShell)
$expected = (Get-Content StructuredLogViewer.zip.sha256).Split(" ")[0]
$actual   = (Get-FileHash -Algorithm SHA256 StructuredLogViewer.zip).Hash.ToLower()
if ($expected -eq $actual) { "OK" } else { "MISMATCH" }
```

### AppImage delta updates (zsync)

The Linux AppImage embeds an `UPDATE_INFORMATION` entry pointing at the `latest` GitHub Release, and a sibling `.zsync` file is published alongside every release. Users can update an installed AppImage incrementally with [`appimageupdate`](https://github.com/AppImageCommunity/AppImageUpdate):

```sh
./appimageupdate-x86_64.AppImage StructuredLogViewer-x86_64.AppImage
```

### Hotfix / re-tagging

If you need to re-run packaging for an existing tag (e.g. after fixing a CI issue), delete and re-push the tag:

```sh
git tag -d v0.7.0
git push origin :refs/tags/v0.7.0
git tag -a v0.7.0 -m "Release v0.7.0"
git push origin v0.7.0
```

Avoid doing this once a release has been downloaded by users — checksums and zsync metadata will no longer match their local copies.
