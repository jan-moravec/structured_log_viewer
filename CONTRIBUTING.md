# Contributing

Thanks for your interest in contributing! This document is the developer reference for the project. It opens with an architecture deep-dive (project layout, the `loglib` headers, the GUI wrappers, the parsing data flow, and how to teach the viewer a new log format), then covers building from source, running the test suite and benchmarks, code style, the pull-request workflow, and the release process.

## Table of contents

- [Architecture](#architecture)
  - [Project Structure](#project-structure)
  - [Library](#library)
  - [GUI Application](#gui-application)
  - [Static vs Streaming pipelines](#static-vs-streaming-pipelines)
  - [Network producers (TCP / UDP)](#network-producers-tcp--udp)
  - [Data Flow](#data-flow)
  - [Adding a New Structured Log Format](#adding-a-new-structured-log-format)
- [Prerequisites](#prerequisites)
  - [Third-party license bundle](#third-party-license-bundle)
- [Building](#building)
  - [Quick start](#quick-start)
  - [Windows](#windows)
  - [Machine-specific overrides (`CMakeUserPresets.json`)](#machine-specific-overrides-cmakeuserpresetsjson)
  - [Enabling system packages](#enabling-system-packages)
  - [IDE integration](#ide-integration)
- [Running tests](#running-tests)
  - [Targets](#targets)
  - [Network smoke recipes](#network-smoke-recipes)
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
│   │   ├── parsers/          # Format-specific parser headers (json_parser.hpp, ...)
│   │   └── internal/         # Internal-but-needed-by-tests headers
│   │                         #   (static_parser_pipeline.hpp,
│   │                         #    streaming_parse_loop.hpp,
│   │                         #    batch_coalescer.hpp,
│   │                         #    parse_runtime.hpp,
│   │                         #    buffering_sink.hpp,
│   │                         #    line_decoder.hpp,
│   │                         #    file_identity.hpp,
│   │                         #    timestamp_promotion.hpp,
│   │                         #    transparent_string_hash.hpp,
│   │                         #    compact_log_value.hpp,
│   │                         #    advanced_parser_options.hpp)
│   ├── src/                  # Library implementation (.cpp only;
│   │                         #   parsers/ subfolder mirrors include/)
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

| Header                              | Role                                                                                                                                                                                                                                                                                                                                                                                                                                                   |
| ----------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `loglib/log_file.hpp`               | `LogFile` memory-maps a log on disk and tracks line offsets. It outlives every `LogLine` parsed from it so values backed by `MmapSlice` stay valid for the file's whole on-screen lifetime.                                                                                                                                                                                                                                                            |
| `loglib/line_source.hpp`            | `LineSource` is the polymorphic seam every `LogLine` carries (paired with a `lineId`). It owns the bytes backing each line and resolves `CompactTag::MmapSlice` / `CompactTag::OwnedString` payloads through `ResolveMmapBytes` / `ResolveOwnedBytes`. `BytesAreStable()` discriminates mmap-backed sources from streaming ones; `SupportsEviction()` / `EvictBefore` are the retention hook used by `LogTable::EvictPrefixRows`.                      |
| `loglib/file_line_source.hpp`       | `FileLineSource` adapts an owned `LogFile` to `LineSource` for the static `File → Open…` path. `BytesAreStable()` is `true`, so the parser keeps its zero-copy `MmapSlice` fast path. LineIds are 0-based file-line indices.                                                                                                                                                                                                                           |
| `loglib/stream_line_source.hpp`     | `StreamLineSource` adapts a live `BytesProducer` to `LineSource` for Stream Mode. Lines are owned in a pair of `std::deque<std::string>`s (raw text + per-line owned arena), 1-based monotonic ids are assigned by `AppendLine`, and a mutex makes it safe for the parser worker to append while the GUI reads / evicts.                                                                                                                               |
| `loglib/bytes_producer.hpp`         | `BytesProducer` is the abstract byte feed behind `StreamLineSource`. `Read` / `WaitForBytes` form the parser's pull loop, `Stop()` unblocks I/O during teardown, and `SetRotationCallback` / `SetStatusCallback` surface rotation events and `Running` ↔ `Waiting` transitions to the GUI. The seam keeps the parser ignorant of where the bytes come from.                                                                                            |
| `loglib/tailing_bytes_producer.hpp` | `TailingBytesProducer` is the file-tailing `BytesProducer`: a one-thread tailer that pre-fills the last *N* complete lines and follows growth via [`efsw`](https://github.com/SpartanJ/efsw) with a polling fallback. Survives rename-and-create / copy-truncate / in-place truncate / delete-then-recreate rotations and coalesces bursts within a 1 s window.                                                                                        |
| `loglib/tcp_server_producer.hpp`    | `TcpServerProducer` is a multi-client TCP-listener `BytesProducer`. Each accepted session has a per-connection carry buffer so output from concurrent clients interleaves at line granularity into one shared queue. With `LOGLIB_NETWORK_TLS=ON` the same class accepts TLS connections via `asio::ssl::stream` (config under `Options::tls`); without it the binary is plaintext-only. Reports `SourceStatus::Waiting` until the first byte arrives. |
| `loglib/udp_server_producer.hpp`    | `UdpServerProducer` is the connectionless counterpart: each datagram becomes one or more complete log records (a missing trailing newline is appended so downstream line-splitting still works). Plaintext only — DTLS is intentionally out of scope. Reports `SourceStatus::Running` immediately because there is no connection state.                                                                                                                |
| `loglib/key_index.hpp`              | `KeyIndex` is an append-only intern table mapping a JSON field name (`"timestamp"`, `"level"`, …) to a dense `KeyId`. It is thread-safe; the parsing pipeline shares a single `KeyIndex` across all workers and the GUI's `LogTable` keeps it for the table's lifetime.                                                                                                                                                                                |
| `loglib/log_line.hpp`               | `LogLine` holds one parsed record as a sorted vector of `(KeyId, internal::CompactLogValue)` pairs plus a `(LineSource *, lineId)` pair. `CompactLogValue` is a 16-byte union (mmap slice / owned-string offset / int / uint / double / bool / timestamp / monostate); the `LineSource` resolves the row's raw bytes regardless of whether they came from an mmap or from a live byte producer.                                                        |
| `loglib/log_data.hpp`               | `LogData` owns the `KeyIndex`, all `LogLine`s, and the `LineSource`(s) they reference. It supports `Merge` for opening multiple files and `AppendBatch` for the streaming path; the static-path single-`LogFile` invariant only applies to `LogLine`s rooted in a `FileLineSource`.                                                                                                                                                                    |
| `loglib/log_configuration.hpp`      | `LogConfiguration` describes the visible columns (header, JSON keys, print format, `Type::any` vs `Type::time`, time-parse formats, filters). `LogConfigurationManager` loads/saves it and grows the layout during streaming via `AppendKeys`.                                                                                                                                                                                                         |
| `loglib/log_table.hpp`              | `LogTable` pairs a `LogData` with a `LogConfigurationManager` and resolves what to render in cell `(row, column)`. It owns the `BeginStreaming` / `AppendBatch` state machine that the GUI model drives, back-fills timestamp columns mid-stream as new keys appear, and exposes `EvictPrefixRows(count)` for the Stream-Mode retention cap.                                                                                                           |
| `loglib/log_processing.hpp`         | Timezone bootstrap (`Initialize`), `TryParseTimestamp` fast/slow paths, and the `BackfillTimestampColumn` helper used by `LogTable`.                                                                                                                                                                                                                                                                                                                   |
| `loglib/log_parser.hpp`             | `LogParser` is the format-agnostic interface (`IsValid`, two `ParseStreaming` overloads — one for `FileLineSource`, one for `StreamLineSource` — and `ToString`). Streaming is the only ingestion path the parser exposes; the synchronous "parse a file to a `ParseResult`" helper lives outside the base class as the free function `loglib::ParseFile(parser, path)` in `loglib/parse_file.hpp`.                                                    |
| `loglib/log_parse_sink.hpp`         | `LogParseSink` is the receiver interface for the streaming parser; one `OnStarted`, zero or more `OnBatch(StreamedBatch)`, one `OnFinished(cancelled)`.                                                                                                                                                                                                                                                                                                |
| `loglib/parser_options.hpp`         | `ParserOptions::stopToken` for cooperative cancellation and `configuration` for inline timestamp promotion. Test-only tuning knobs (thread cap, batch size) live behind `loglib/internal/advanced_parser_options.hpp`.                                                                                                                                                                                                                                 |
| `loglib/parsers/json_parser.hpp`    | `JsonParser`: the only currently shipped `LogParser`. The static `FileLineSource` overload runs the TBB pipeline over the mmap; the live-tail `StreamLineSource` overload runs the single-threaded streaming loop over `source.Producer()`. Both share simdjson via the per-worker decoder.                                                                                                                                                            |
| `loglib/log_factory.hpp`            | `LogFactory::Create(Parser)` is the typed factory for the shipping `LogParser` implementations. The auto-detecting `loglib::ParseFile(path)` overload (in `loglib/parse_file.hpp`) probes every entry of the `Parser` enum via its `IsValid` content sniff and dispatches to the first match.                                                                                                                                                          |

Several helpers live under `library/include/loglib/internal/` and are intentionally **not** part of the public API. They are kept there (rather than next to the `.cpp`s in `library/src/`) so the unit tests in `test/lib/` can include them via the same `loglib`-prefixed include path as the public headers, without needing a per-target include-directory workaround. The most important are:

- `BufferingSink` (`buffering_sink.hpp`) — the sink behind the `loglib::ParseFile(parser, path)` free helper.
- `loglib::internal::RunStaticParserPipeline` (`static_parser_pipeline.hpp`) — the TBB three-stage pipeline used for static-file parses; see [Static vs Streaming pipelines](#static-vs-streaming-pipelines).
- `loglib::internal::RunStreamingParseLoop` (`streaming_parse_loop.hpp`) — the single-threaded read / decode / batch loop used for live tailing.
- `BatchCoalescer` (`batch_coalescer.hpp`) — shared by both pipelines for the "flush every ~1000 lines or 50 ms / ~250 lines or 100 ms" coalescing and the `newKeys` diff against `KeyIndex`.
- `loglib::detail::FileIdentity` (`file_identity.hpp`) — POSIX `(st_dev, st_ino)` / Windows `GetFileInformationByHandle` helper used by `TailingBytesProducer` for rotation detection.
- `CompactLineDecoder` (`line_decoder.hpp`) — the single-line decoder concept that the streaming loop drives; `JsonParser` provides the JSON instance.
- The shared scratch types both pipelines use live in `parse_runtime.hpp`; `timestamp_promotion.hpp`, `compact_log_value.hpp`, and `transparent_string_hash.hpp` round out the set.

### GUI Application

The `app` component is a Qt 6 Widgets application. It uses `loglib` for parsing and data management and exposes the data through `QAbstractTableModel` / `QSortFilterProxyModel` subclasses with support for sorting, filtering, searching, configurable columns, and live-tail streaming.

The Qt-side classes that wrap `loglib` are:

- `LogModel` (`app/include/log_model.hpp`) — a `QAbstractTableModel` that owns the `LogTable`, the `QtStreamingLogSink`, the active parser `QFutureWatcher`, and (in Stream Mode) the live `BytesProducer`. Population is always streaming-driven through one of two entry points:
  - `BeginStreaming(unique_ptr<FileLineSource>, parseCallable)` / `AppendStreaming(...)` for the static `File → Open…` queue.
  - `BeginStreaming(unique_ptr<StreamLineSource>, ParserOptions)` for Stream Mode; spawns the `JsonParser::ParseStreaming(StreamLineSource&, ...)` worker and re-emits `rotationDetected` / `sourceStatusChanged` from the source's worker thread via queued connections.
    Both paths converge on `AppendBatch(StreamedBatch)`. With a non-zero `RetentionCap()` (Stream Mode), `AppendBatch` FIFO-evicts the visible row prefix before insertion and head-trims over-cap batches first so per-batch eviction stays O(cap). `Reset()` runs the full teardown (`BytesProducer::Stop()` → sink `RequestStop()` → worker join → paused-buffer flush → `DropPendingBatches()`); `StopAndKeepRows()` runs the same teardown but preserves the visible rows so the user can keep working on them after Stop.
- `QtStreamingLogSink` (`app/include/qt_streaming_log_sink.hpp`) — the bridge that turns a `loglib::LogParseSink` callback running on a TBB / streaming worker thread into a `LogModel::AppendBatch` call on the GUI thread. `OnBatch` enqueues into a bounded SPSC `BoundedBatchQueue` (default capacity 32) and, on the queue's empty-to-non-empty edge, posts a single `Drain` lambda via `Qt::QueuedConnection`; subsequent enqueues until the lambda runs are coalesced under a `mDrainScheduled` atomic. The drain pulls everything pending and applies it to the model in one `AppendBatch` call. The queue gives end-to-end back-pressure: once it fills, `WaitEnqueue` parks the worker, which propagates through `BatchCoalescer` → TBB Stage C → Stage A so the parser runs at the GUI's pace and resident in-flight rows stay bounded regardless of file size. The sink owns a generation counter so a fresh `Arm()` (or a `RequestStop()`) drops any still-queued batches from a previous parse, and a separate bounded paused buffer with `Pause` / `Resume` / `SetRetentionCap` / `TakePausedBuffer` for the Stream-Mode toolbar (`Resume` coalesces the buffered batches into a single queued post). `PausedDropCount()` reports how many lines were FIFO-evicted from the paused buffer for the status bar's `… , X dropped while paused` suffix.
- `BoundedBatchQueue` (`app/include/bounded_batch_queue.hpp`) — header-only SPSC queue (`std::mutex` + `std::condition_variable` + `std::deque`) that backs `QtStreamingLogSink::mPending`. Producer side blocks in `WaitEnqueue` once the queue holds `capacity` items; consumer side never blocks (`DrainAll` moves every buffered item out). Cancellation is explicit via `NotifyStop`, which broadcasts to wake any parked producer immediately — this is paired with `RequestStop()` so teardown does not deadlock against `mStreamingWatcher->waitForFinished()`. We do not use `std::stop_token` because `loglib::StopToken` (see `library/include/loglib/stop_token.hpp`) is intentionally callback-free for macOS toolchain compatibility.
- `RowOrderProxyModel` (`app/include/row_order_proxy_model.hpp`) — optional newest-first reversal layer between `LogModel` and `LogFilterModel`. A custom `QAbstractProxyModel` that does O(1)-per-row mirror mapping (no internal sort, no mapping table); structural source signals are translated and forwarded so streaming a 1 GB file scales linearly instead of the prior O(N² log N) behaviour. Driven exclusively from `MainWindow::ApplyDisplayOrder`, which keeps the proxy direction, the `LogTableView` tail edge, and the alternating-row-colours flag in lockstep, and which picks per-mode whether to consult `StreamingControl::IsNewestFirst()` (stream sessions) or `StreamingControl::IsStaticNewestFirst()` (static sessions).
- `LogFilterModel` (`app/include/log_filter_model.hpp`) — the `QSortFilterProxyModel` over `RowOrderProxyModel` that implements the multi-column filter set described in the [user guide](doc/README.md#filtering). The view chain is therefore `LogModel → RowOrderProxyModel → LogFilterModel → LogTableView`.
- `LogTableView` (`app/include/log_table_view.hpp`) — `QTableView` subclass that owns the `TailEdge` (`Bottom` by default, `Top` in newest-first mode) and emits edge-triggered `userScrolledAwayFromTail` / `userScrolledToTail` signals. `MainWindow` wires those to auto-disengage / auto-re-engage **Follow newest** without fighting programmatic scrolls. In newest-first mode the view also runs the chat-app reading-position preservation pattern around `rowsInserted` / `layoutChanged`, anchoring the topmost visible row across batches so the user's place in history is stable while new lines arrive on top.
- `StreamingControl` (`app/include/streaming_control.hpp`) — `QSettings`-backed transactional store for the **Streaming** and **Static (file mode)** groups of `PreferencesEditor` (retention cap, stream-mode newest-first flag, static-mode newest-first flag). Same Ok / Cancel pattern as `AppearanceControl`: in-memory mutation, `SaveConfiguration` commits, `LoadConfiguration` reverts.
- `PreferencesEditor` (`app/include/preferences_editor.hpp`) — Qt widget hosting the **Appearance**, **Streaming**, and **Static (file mode)** groups; emits `streamingRetentionChanged` / `streamingDisplayOrderChanged` / `staticDisplayOrderChanged` on Ok so `MainWindow::ApplyDisplayOrder` can re-apply them live with mode-aware dispatch.
- `MainWindow` (`app/include/main_window.hpp`) — orchestrates everything: open dialogs, drag & drop, the find bar, the filter editor, the preferences editor, the sequential file-queue open flow (`StartStreamingOpenQueue` / `StreamNextPendingFile`) that drives `LogModel::BeginStreaming` for the first file and `LogModel::AppendStreaming` for the rest, and the Stream Mode entry point (`OpenLogStream` → `LogModel::BeginStreaming(StreamLineSource, ...)`) plus the toolbar wiring (`TogglePauseStream` / `StopStream`) and the status-bar state machine (`UpdateStreamingStatus` covers the `Streaming` / `Paused` / `Source unavailable` / `— rotated` / `dropped while paused` variants).

### Static vs Streaming pipelines

`LogParser` exposes two `ParseStreaming` overloads, each backed by a different in-library driver. They share the sink contract, the `KeyIndex` / `LogConfiguration` plumbing, and `BatchCoalescer`, but differ in concurrency model and byte source. Pick the one that matches your input.

| Aspect                | Static pipeline (`RunStaticParserPipeline`)                                                                                       | Streaming loop (`RunStreamingParseLoop`)                                                                                                                                                                    |
| --------------------- | --------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **Header**            | `loglib/internal/static_parser_pipeline.hpp`                                                                                      | `loglib/internal/streaming_parse_loop.hpp`                                                                                                                                                                  |
| **Parser entry**      | `LogParser::ParseStreaming(FileLineSource&, LogParseSink&, ParserOptions)`                                                        | `LogParser::ParseStreaming(StreamLineSource&, LogParseSink&, ParserOptions)`                                                                                                                                |
| **Use case**          | Static `File → Open…` queue, `loglib::ParseFile(parser, path)`, the `[parse_sync]` / `[large]` / `[wide]` benchmarks.             | Stream Mode (live tail), the `[stream_latency]` benchmark.                                                                                                                                                  |
| **Concurrency**       | `oneapi::tbb::parallel_pipeline`, three stages (Stage A serial, Stage B parallel, Stage C serial). One TBB worker pool per parse. | Single thread driven by the parser worker. The target is thousands of lines/s, so TBB overhead is not warranted.                                                                                            |
| **Byte source**       | mmap via `FileLineSource::File()`. `BytesAreStable()` is `true`, so emitted values can be `MmapSlice` (zero-copy fast path).      | `BytesProducer::Read` / `WaitForBytes`, called in a tight loop. `BytesAreStable()` is `false`, so values are always `OwnedString`.                                                                          |
| **Line storage**      | `LogFile` mmap stays alive for the whole on-screen lifetime; `lineId` is the 0-based file-line index.                             | Per-line `std::string` in `StreamLineSource`'s deque, committed atomically by `AppendLine`; `lineId` is 1-based and monotonic.                                                                              |
| **Coalescing target** | `STATIC_BATCH_FLUSH_LINES = 1000` lines or `50 ms` (throughput optimised).                                                        | `STREAMING_BATCH_FLUSH_LINES = 250` lines or `100 ms` (latency optimised).                                                                                                                                  |
| **Cancellation**      | `ParserOptions::stopToken` only — cooperatively checked at Stage A token boundaries and Stage C flushes.                          | Both `ParserOptions::stopToken` (checked between lines / batches) **and** `BytesProducer::Stop()` (releases I/O parked in `Read` / `WaitForBytes`). Either one alone cannot unblock a worker parked on I/O. |
| **Eviction**          | Not used (`FileLineSource::SupportsEviction()` is `false`).                                                                       | `LogTable::EvictPrefixRows` calls `StreamLineSource::EvictBefore` on the FIFO-evicted prefix once per `AppendBatch`.                                                                                        |

Both drivers funnel through `BatchCoalescer`, which re-asserts ordering, diffs the `KeyIndex` to find `newKeys` for the batch, and emits `OnStarted` / `OnBatch(StreamedBatch)` / `OnFinished(cancelled)` to whichever sink is wired in.

### Network producers (TCP / UDP)

`TcpServerProducer` and `UdpServerProducer` are alternative `BytesProducer`s for live ingestion sourced over the network. They reuse the same streaming-loop driver and `StreamLineSource` plumbing as the file tailer; the only difference is who writes the bytes.

```mermaid
flowchart LR
    appA["App A"] --> tcpServer["TcpServerProducer<br/>(asio + optional asio::ssl)"]
    appB["App B"] --> tcpServer
    appC["App C"] --> udpServer["UdpServerProducer<br/>(asio UDP, plaintext)"]
    tcpServer --> queue["shared byte queue<br/>(line-granular interleave)"]
    udpServer --> queue
    queue --> streamLoop["RunStreamingParseLoop"]
    streamLoop --> sink["QtStreamingLogSink"]
    sink --> ui["LogTable / LogTableView"]
```

Notes worth knowing before adding a feature here:

- **TCP interleaving**. Each accepted session has its own carry buffer (held inside `TcpServerProducerImpl::Session<Stream>`). When a recv ends mid-line, the trailing fragment stays in that session's carry until the rest of the line lands; complete lines are atomically pushed into the shared queue. This is the only reason output from concurrent clients can interleave at line granularity rather than tearing.
- **TLS gating**. `LOGLIB_NETWORK_TLS=ON` defines the public `LOGLIB_HAS_TLS` macro and pulls in `find_package(OpenSSL REQUIRED)`. With it off, `Options::tls.has_value()` becomes a runtime error in `TcpServerProducer` (`std::runtime_error("TLS not built in")`) — by design; we want callers to fail fast rather than silently downgrade to plaintext. The unit-test suite mirrors this: `test_tcp_server_producer_tls.cpp` is only compiled when the flag is on, and CI passes `-DLOGLIB_NETWORK_TLS=ON` on every runner.
- **UDP framing**. Each datagram is treated as one or more complete log records. A trailing `\n` is appended if missing so `RunStreamingParseLoop`'s line splitter can do its job. Datagrams arriving out of order are rare enough on loopback / LAN to ignore; for WAN-grade reliability, use TCP.
- **No DTLS**. UDP is plaintext-only on purpose; DTLS is a heavier dependency than the use case justifies. Encrypted log shipping is the TCP+TLS path.
- **Test certificates**. The TLS unit tests mint a fresh RSA-2048 self-signed cert per test run via the OpenSSL EVP API (`test/lib/src/test_tcp_server_producer_tls.cpp`). For manual GUI testing, `openssl req -x509 -newkey rsa:2048 -keyout key.pem -out cert.pem -days 1 -nodes -subj "/CN=localhost"` works; pair with `--tls-skip-verify` on the client side.

### Data Flow

A JSON log line takes the same path through the codebase whether you opened the file from `File → Open…` or are tailing it through Stream Mode. Only the leftmost stage (and the threading model it runs in) differs:

```text
  Static path (File → Open…)             Stream path (File → Open Log Stream…)
  ┌────────────────────────────┐         ┌──────────────────────────────────┐
  │ FileLineSource over mmap   │         │ StreamLineSource over            │
  │                            │         │   TailingBytesProducer           │
  │ RunStaticParserPipeline    │         │ RunStreamingParseLoop            │
  │ (TBB Stage A → B → C)      │         │ (single-threaded read/decode)    │
  └─────────────┬──────────────┘         └─────────────────┬────────────────┘
                │                                          │
                └──────────────────┬───────────────────────┘
                                   ▼
                         BatchCoalescer (newKeys diff,
                         line-number cursor, flush thresholds)
                                   │
                                   ▼ OnBatch(StreamedBatch)
                              LogParseSink
                                   │
                ┌──────────────────┴──────────────────┐
                ▼                                     ▼
          BufferingSink                     QtStreamingLogSink
          (loglib::ParseFile)               (BoundedBatchQueue → drain
                                             lambda → AppendBatch,
                                             paused buffer, generation
                                             counter, drop counter)
                                                      │
                                                      ▼
                                          LogModel::AppendBatch
                                          • FIFO-evict the visible
                                            prefix when over the
                                            retention cap (Stream Mode)
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
                                  RowOrderProxyModel (optional newest-first)
                                                      │
                                                      ▼
                                  LogFilterModel (sort + filter proxy)
                                                      │
                                                      ▼
                                         LogTableView (Qt widget)
```

1. **A `LineSource` is opened.** Static opens build a `FileLineSource` over a `LogFile` (mmap + line offsets). Stream Mode builds a `StreamLineSource` wrapping a `TailingBytesProducer`, which spawns its own worker thread, pre-fills the last *N* complete lines, watches the file via `efsw` (with a 250 ms polling fallback), and recovers from rename / copytruncate / in-place truncate / delete-then-recreate rotations.
1. **The matching parser driver runs.** `JsonParser::ParseStreaming(FileLineSource&, ...)` calls `internal::RunStaticParserPipeline` and supplies the Stage A/B lambdas; `JsonParser::ParseStreaming(StreamLineSource&, ...)` calls `internal::RunStreamingParseLoop` with a per-line decoder. Both use simdjson via the same per-worker scratch (`WorkerScratchBase` + format-specific extension), and both promote configured `Type::time` columns inline (`PromoteLineTimestamps`) while the freshly-written values are still hot in L1.
   - **Static (TBB pipeline)** — Stage A (`serial_in_order`) carves the mmap into ~1 MiB byte-range tokens at line boundaries; Stage B (`parallel`) decodes each token into a `ParsedPipelineBatch` of `LogLine`s, with field names interned through a per-worker cache that hits the shared `KeyIndex` only on first sight; Stage C (`serial_in_order`) assigns absolute line numbers and forwards to `BatchCoalescer`.
   - **Streaming loop** — pulls bytes from `source.Producer()` into a 64 KiB buffer, splits at `\n`, calls the decoder per line, atomically commits `(rawText, ownedArena)` to the `StreamLineSource` via `AppendLine`, and forwards to `BatchCoalescer`. On transient EOF (`Read` returns 0 but `IsClosed()` is `false`) it flushes the partial batch and parks on `WaitForBytes` until the producer wakes it. On rotation, the producer fires its rotation callback and resumes from the new file's start offset.
1. **`BatchCoalescer` flushes a `StreamedBatch`.** Both pipelines coalesce small batches (1000 / 50 ms for static, 250 / 100 ms for streaming), diff the `KeyIndex` to populate `batch.newKeys`, advance the line-number cursor across batches, and emit `OnBatch(StreamedBatch)` to the sink. The format-specific parser only has to turn bytes into `(KeyId, LogValue)` pairs.
1. **A sink consumes the batches.** Two `LogParseSink` implementations ship today:
   - `BufferingSink` (declared in `library/include/loglib/internal/buffering_sink.hpp`) is the sink behind the synchronous `loglib::ParseFile(parser, path)` free helper in `loglib/parse_file.hpp`. It accumulates every batch into a single `LogData` and is what tests and any non-GUI caller that wants a one-shot `LogData` use. Production GUI code never reaches it — both the static-file open path and Stream Mode always stream.
   - `QtStreamingLogSink` is the GUI bridge. Its `OnBatch(StreamedBatch)` enqueues into a bounded SPSC `BoundedBatchQueue` and lazily posts a single `Drain` lambda per drain epoch; the lambda pulls everything pending in one shot and forwards it to `LogModel::AppendBatch`. The bounded queue is the unified back-pressure point of the lib-app pipeline: when the GUI falls behind the worker parks in `WaitEnqueue`, propagating pressure all the way back to TBB Stage A. The drain lambda drops on a generation mismatch (a fresh `Arm()` or `RequestStop()` invalidates batches from a previous parse). While **Pause** is engaged, the sink routes batches into a bounded paused buffer (FIFO-trimmed against the retention cap, with `PausedDropCount()` tracking the loss) instead of the bounded queue; **Resume** drains the paused buffer in a single coalesced post.
1. **`LogModel::AppendBatch` updates the table.** With a non-zero `RetentionCap()` (Stream Mode), the visible row prefix is FIFO-evicted before insertion and over-cap batches are head-collapsed first so per-batch eviction stays O(cap). The batch is then handed to `LogTable::AppendBatch`, which:
   - extends the `LogConfiguration` with any `batch.newKeys` (auto-promoting names that look like timestamps to `Type::time`),
   - back-fills any *newly* introduced time column over **all** rows (file rows + stream rows) so users never see a half-parsed timestamp column,
   - and reports the column range it back-filled via `LastBackfillRange()` so the model emits a single `dataChanged` for those cells.
     The model then emits `beginInsertColumns` / `beginInsertRows` (and `beginRemoveRows` / `endRemoveRows` for the FIFO eviction), plus `lineCountChanged` / `errorCountChanged` / `rotationDetected` / `sourceStatusChanged` so `MainWindow` can tick the status-bar label (`Parsing <file>` / `Streaming <file>` / `Paused` / `Source unavailable` / `… — rotated`).
1. **The view renders the rows.** `RowOrderProxyModel` optionally reverses the order for newest-first display (O(1)-per-row mirror mapping over a custom `QAbstractProxyModel`; the prior `QSortFilterProxyModel`-based implementation re-sorted on every batch and was O(N² log N) for large files), `LogFilterModel` (`QSortFilterProxyModel`) applies the active filters, and `LogTableView` renders the surviving rows. Follow-newest scrolls land on the proxy-mapped index of the most-recently-appended source row; the view's `TailEdge` (set from `MainWindow::ApplyDisplayOrder`) keeps the auto-disengage / re-engage signals on the right scrollbar edge. The `Edit → Copy` shortcut pulls the original log text via `LogLine::Source()->RawLine(lineId)` so the row round-trips back to its on-disk (or on-tail) format.

A few cross-cutting invariants hold this together:

- **Each `LineSource` outlives every `LogLine` that quotes it.** That is why `LogModel::Reset()` / `LogModel::StopAndKeepRows()` and `~LogModel()` run a strict teardown order — `BytesProducer::Stop()` first (releases the live-tail worker parked on I/O), then sink `RequestStop()` (which both trips the `stop_token` and calls `mPending.NotifyStop()` so a worker parked on `BoundedBatchQueue::WaitEnqueue` wakes immediately instead of deadlocking the join), then `mStreamingWatcher` join, then paused-buffer flush, then `DropPendingBatches()` (which also drain-and-discards the bounded queue) — before tearing down the `LogTable`. The cooperative `stop_token` only halts the Stage A token loop / the streaming-loop top, but tasks already in flight keep reading from the source.
- **The parser sees a snapshot of the configuration.** Both the static queue (`MainWindow::StreamNextPendingFile`) and the Stream Mode entry (`MainWindow::OpenLogStream`) snapshot `LogConfiguration` into a `shared_ptr<const>` and gate the configuration menus for the duration of the parse, so a configuration edit racing past the UI gate cannot still affect the in-flight parse.
- **`KeyIndex` is the single source of truth for column identity** across the streaming workers, the `LogConfigurationManager`, and `LogTable`'s back-fill logic. It is append-only and assigns dense ids, so it doubles as an index into per-key arrays. `LogTable::AppendBatch` uses it identically for static-file rows and for stream rows.
- **Stream-Mode retention is a model-level concern, not a parser one.** The parser keeps appending lines to `StreamLineSource` regardless of cap; `LogModel::AppendBatch` is the only place that evicts (visible prefix + over-cap-batch head trim + paused-buffer trim), and it is a no-op when `mRetentionCap == 0` so the static path executes zero new instructions.

### Adding a New Structured Log Format

To teach the viewer a new format (CSV, logfmt, syslog, …):

1. **Subclass `loglib::LogParser`.** Add `library/include/loglib/parsers/<format>_parser.hpp` for the public surface and `library/src/parsers/<format>_parser.cpp` for the implementation, then implement:

   - `bool IsValid(const std::filesystem::path &) const` — a cheap content sniff (read one or two leading lines). The auto-detecting `loglib::ParseFile(path)` overload calls this when probing parsers, so it must be fast and false-positive-free.
   - `void ParseStreaming(FileLineSource&, LogParseSink&, ParserOptions) const` — the static-file entry point. Drives the TBB pipeline over the source's underlying mmap.
   - `void ParseStreaming(StreamLineSource&, LogParseSink&, ParserOptions) const` — the live-tail entry point. Drives the streaming loop over `source.Producer()` and appends each parsed line to `source` via `AppendLine` so the row keeps a stable id.
   - `std::string ToString(const LogLine&) const` — the inverse, used by `Edit → Copy` to round-trip a row back to the format's native text.

1. **Reuse the shared static pipeline** in the `FileLineSource` overload. Almost all of the heavy lifting lives in `loglib::internal::RunStaticParserPipeline` (`library/include/loglib/internal/static_parser_pipeline.hpp`). Define two types and two lambdas, then call:

   ```cpp
   internal::RunStaticParserPipeline<Token, UserState>(source, sink, options, advanced, stageA, stageB);
   ```

   Where:

   - `Token` is your Stage A unit of work (e.g. `JsonByteRange` for `JsonParser` — typically a `[bytesBegin, bytesEnd)` slice of the mmap).
   - `UserState` is the format-specific per-worker scratch (e.g. a `simdjson::ondemand::parser` and its padded buffer for JSON, a CSV row-splitter for CSV). It is bolted onto `WorkerScratchBase`, which already provides the per-worker `KeyIndex` cache and timestamp-parse scratch.
   - `stageA(Token& out) -> bool` produces the next token from the source; return `false` at EOF.
   - `stageB(Token, WorkerScratch<UserState>&, KeyIndex&, std::span<const TimeColumnSpec>, ParsedPipelineBatch&)` decodes one token into `parsed.lines` (built via `LogLine{sortedValues, keys, source, lineId}`) and `parsed.errors`. After pushing each line, call `worker.PromoteTimestamps(parsed.lines.back(), timeColumns)` to keep the inline timestamp fast path warm. Use `internal::InternKeyVia(...)` to intern field names through the per-worker cache. Set `parsed.totalLineCount` to the number of source lines consumed (parsed + errored + skipped) so Stage C can advance its line-number cursor.

   The harness owns batch coalescing, the `newKeys` diff, line-number assignment, and stop-token handling — your parser only has to turn bytes into `(KeyId, LogValue)` pairs.

1. **Reuse the shared streaming loop** in the `StreamLineSource` overload. `loglib::internal::RunStreamingParseLoop` (`library/include/loglib/internal/streaming_parse_loop.hpp`) handles `Read`/`WaitForBytes` polling, partial-line carry-over across reads, batch coalescing, the `newKeys` diff, atomic `AppendLine` commits to the source, and inline timestamp promotion. Implement a single-line decoder that satisfies the `CompactLineDecoder` concept — `bool DecodeCompact(string_view line, KeyIndex&, KeyCache*, vector<pair<KeyId, CompactLogValue>>& out, string& ownedArena, string& errorOut)` — and call:

   ```cpp
   internal::RunStreamingParseLoop(source, decoder, sink, options);
   ```

   The decoder is the same primitive `Stage B` calls in the static pipeline, so most parsers can share one implementation between the two overloads (`JsonParser` does).

1. **Register the parser in `LogFactory`** (`library/include/loglib/log_factory.hpp` + `library/src/log_factory.cpp`):

   - Add a value to `LogFactory::Parser` (before `Count`).
   - Extend `LogFactory::Create` to instantiate it.
     Files that match `IsValid` are then auto-detected by `File → Open…`.

1. **Optional GUI affordance.** Both `File → Open…` (via `MainWindow::StartStreamingOpenQueue` → `StreamNextPendingFile`) and `File → Open Log Stream…` (via `MainWindow::OpenLogStream`) currently hard-code `JsonParser`. To force a different parser through the GUI, parameterise the open call sites on a `LogParser` reference (similar to the existing `parser.ParseStreaming(*fileSourcePtr, *sink, options)` site for static and the `JsonParser{}` instance constructed by the streaming worker) and pick the parser per-format up front.

1. **Tests.** Add Catch2 unit tests under `test/lib/` mirroring `test/lib/src/test_json_parser.cpp`, and pipeline tests under `test/lib/src/test_parser_pipeline.cpp` if you want coverage of the static-pipeline cancellation / coalescing / new-keys paths. Stream-Mode coverage lives in `test/lib/src/test_stream_line_source.cpp`, `test/lib/src/test_tailing_bytes_producer.cpp`, and `test/lib/src/test_stream_stop_teardown.cpp`. For Qt Test smoke coverage, add fixtures under `test/app/fixtures/` and reference them from `fixtures.qrc`. The shared helpers in `test/common/` are useful for generating synthetic data, and `test/log_generator/` ships an out-of-process JSONL fixture generator with `--lines` / `--size` stop conditions, `--timeout` throttling, and `--roll-strategy {rename,copytruncate,truncate}` for driving Stream-Mode rotation tests against a real `TailingBytesProducer`.

Timestamp parsing / back-fill, key interning, column-layout updates, retention eviction, and Qt-side bridging fall out of the existing infrastructure as long as your decoder emits `LogLine`s through one of the two harnesses.

## Prerequisites

- **CMake** 3.28 or newer
- **Qt** 6.1 or newer (Qt 6.8 is used in CI)
- A C++23-capable toolchain:
  - Linux: GCC 13+ or Clang 17+
  - Windows: MSVC 2022 (Visual Studio 2022)
  - macOS: Xcode 15+ / Apple Clang

Most third-party C++ dependencies (`date`, `fmt`, `Catch2`, `mio`, `glaze`, `simdjson`, `robin-map`, `efsw`, `asio`) are fetched automatically via `FetchContent`. To use system copies instead, pass the corresponding option when configuring — one of `USE_SYSTEM_DATE`, `USE_SYSTEM_FMT`, `USE_SYSTEM_CATCH2`, `USE_SYSTEM_MIO`, `USE_SYSTEM_GLAZE`, `USE_SYSTEM_SIMDJSON`, `USE_SYSTEM_ROBIN_MAP`, `USE_SYSTEM_EFSW`, `USE_SYSTEM_ASIO` (e.g. `-DUSE_SYSTEM_FMT=ON`). See [`cmake/FetchDependencies.cmake`](cmake/FetchDependencies.cmake) for the pinned versions.

`loglib` also links against [oneTBB](https://github.com/uxlfoundation/oneTBB) (Intel oneAPI Threading Building Blocks) for the parallel JSON parsing pipeline and against [efsw](https://github.com/SpartanJ/efsw) for the cross-platform filesystem watcher behind Stream Mode's `TailingBytesProducer`. oneTBB is fetched at `v2022.3.0` by default; pass `-DUSE_SYSTEM_TBB=ON` to use a system installation, which must be **>= 2021.5** (the first oneAPI-style release that ships `tbb::filter_mode` / `tbb::parallel_pipeline`). efsw is fetched at `1.6.1`; the matching `USE_SYSTEM_EFSW` knob picks up a system copy. On Windows, the build copies `tbb12.dll` next to `StructuredLogViewer.exe` and the test binaries automatically (both into the build tree and the install tree) — keep it next to the executable when redistributing.

The TCP / UDP network producers use [standalone Asio](https://github.com/chriskohlhoff/asio) (header-only, BSL-1.0, fetched at `asio-1-30-2`). TLS for the TCP producer is gated behind the **`LOGLIB_NETWORK_TLS`** CMake option (default `ON`) and consumes system OpenSSL via `find_package(OpenSSL REQUIRED)`. Install the dev package on each platform:

- **Linux** (Debian / Ubuntu): `sudo apt-get install libssl-dev`
- **Windows**: `choco install openssl --yes` (default install root: `C:\Program Files\OpenSSL-Win64`); the configure step needs `OPENSSL_ROOT_DIR` set if the install is somewhere non-default.
- **macOS** (Homebrew): `brew install openssl@3` and pass `-DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3)` at configure time so CMake skips the system-Python fork of OpenSSL.

If you don't need TLS (developer build, AppImage smoke check, local fuzzing), pass `-DLOGLIB_NETWORK_TLS=OFF` at configure time and OpenSSL becomes unnecessary; `TcpServerProducer` then only accepts plaintext clients and the corresponding TLS unit tests are excluded from the suite. CI passes `-DLOGLIB_NETWORK_TLS=ON` on every runner so released binaries always include TLS.

The Windows build also copies the OpenSSL runtime DLLs (`libssl-3-x64.dll`, `libcrypto-3-x64.dll`) next to `StructuredLogViewer.exe`; if the post-build step warns that it could not locate them, set `OPENSSL_ROOT_DIR` so its `bin/` subdirectory contains the expected files. macOS resolves OpenSSL via the Homebrew rpath, and Linux relies on the system `libssl.so.3` provided by the distro (or the AppImage bundle for distributable builds).

### Third-party license bundle

[`cmake/BundleLicenses.cmake`](cmake/BundleLicenses.cmake) aggregates the LICENSE / COPYING text of every linked dependency into a single `THIRD_PARTY_LICENSES.txt` and stages it (plus the project's own `LICENSE`) next to the executable in the build tree, the install tree, and every packaged artifact (Linux AppImage, Windows zip, macOS DMG). It picks up FetchContent-staged deps via `FetchContent_GetProperties` so the lookup survives the `block()`-scoped `FetchContent_MakeAvailable` calls in [`cmake/FetchDependencies.cmake`](cmake/FetchDependencies.cmake). OpenSSL and Qt are system / find_package deps; their attribution texts live as snippets under [`cmake/license_snippets/`](cmake/license_snippets/) (the OpenSSL section is gated on `LOGLIB_NETWORK_TLS=ON` so a no-TLS build does not falsely advertise it).

When adding a new third-party dependency to `cmake/FetchDependencies.cmake`, append an entry to the `FETCHED_DEPS` list inside `bundle_third_party_licenses` so its license is included automatically. For system-only deps that ship a fixed license text (Qt, OpenSSL), drop a snippet under `cmake/license_snippets/` and add a `_bundle_collect_entry` call.

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

| Target          | Coverage                                                                                                                                                                                                                                                                                                                                                                                                           |
| --------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `tests`         | Catch2 unit tests for `loglib` (parsers, the streaming pipeline, `LogTable`, `LogConfiguration`, `KeyIndex`, timestamp parsing) **and** the parser/lookup micro-benchmarks. The benchmark cases are tagged `[.][benchmark]` so they're hidden by default; `catch_discover_tests` registers them under the `benchmark` CTest label.                                                                                 |
| `apptest`       | Qt Test smoke tests that drive a real `MainWindow` in offscreen mode against the JSONL fixtures embedded in `test/app/fixtures/`. Registered as a single CTest entry.                                                                                                                                                                                                                                              |
| `log_generator` | Standalone JSONL fixture generator (manual UI smoke testing); not run by CTest. Supports `--lines` / `--size` stop conditions, `--timeout` throttling, in-flight `--roll-strategy {rename,copytruncate,truncate}` for Stream Mode rotation tests, and `--target tcp://host:port` / `--target tcp+tls://host:port` / `--target udp://host:port` for driving `TcpServerProducer` and `UdpServerProducer` end-to-end. |

#### Network smoke recipes

These pair the GUI's `File → Open Network Stream…` action (`Ctrl+Shift+N`) with `log_generator` to exercise the TCP / UDP / TLS paths end-to-end against the running app. They assume both binaries built from the same tree.

```sh
# Plain TCP: launch the app, open a network stream on TCP port 5141,
# then push 1000 lines through it from a second terminal.
log_generator --target tcp://127.0.0.1:5141 --lines 1000 --timeout 5

# Plain UDP: same shape but datagram-per-line.
log_generator --target udp://127.0.0.1:5142 --lines 1000 --timeout 5

# TLS: mint a self-signed dev cert, point the GUI at it, then push
# from log_generator with --tls-skip-verify (matching CN= would be
# 'localhost' for production).
openssl req -x509 -newkey rsa:2048 -keyout key.pem -out cert.pem \
    -days 1 -nodes -subj "/CN=localhost"
# In the GUI: Open Network Stream... -> TCP, port 5143, Enable TLS,
# pick cert.pem / key.pem, leave CA blank, OK.
log_generator --target tcp+tls://127.0.0.1:5143 --lines 100 \
    --timeout 50 --tls-skip-verify
```

The `--roll-*` and `--append` flags are rejected on network targets (rotation is a filesystem concept), and `--tls-*` flags only apply to `tcp+tls://`. The lib-level Catch2 tests (`test/lib/src/test_tcp_server_producer.cpp`, `test_tcp_server_producer_tls.cpp`, `test_udp_server_producer.cpp`) already cover the producer side; the recipes above are for human eyes-on-screen verification of the GUI status-bar / row-flow paths.

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
| `[stream_latency]`  | Stream-Mode write-to-row latency over a `TailingFileSource` + `JsonParser::ParseStreaming` chain. Asserts median ≤ 250 ms / p95 ≤ 500 ms.                             |

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
