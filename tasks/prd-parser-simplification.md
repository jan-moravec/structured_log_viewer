# PRD: Parser Simplification & Common Pipeline Toolkit

**Status:** Draft v1
**Owner:** TBD (junior dev welcome)
**Branch:** continues on `feature/improve-performance-and-add-streaming`
**Predecessors (already shipped, kept verbatim under `tasks/`):** [tasks/prd-fast-streaming-json-parser.md](./prd-fast-streaming-json-parser.md), [tasks/prd-parser-performance-hardening.md](./prd-parser-performance-hardening.md), [tasks/prd-hl-inspired-parser-performance.md](./prd-hl-inspired-parser-performance.md).

---

## 1. Introduction / Overview

After three back-to-back performance PRDs, the JSON streaming parser ships at ~1158 MB/s on the `[large]` benchmark — a ~10× speed-up over the original baseline. Throughput is in a great place. The cost has been **complexity**: `library/src/json_parser.cpp` is ~1,500 lines, with the streaming pipeline (Stage A/B/C, TBB harness, per-worker caches, time-column promotion, coalesced/uncoalesced sink dispatch, telemetry) entangled with JSON-specifics. The codebase carries dense `PRD §…` and `parser-perf task X.Y` cross-references in nearly every comment.

The library was always meant to be **format-pluggable** — JSON is one supported parser, not the only one. New formats may be **binary or text-with-newlines**, so each parser must keep ownership of its own streaming entry point. The current shape is hostile to a junior dev adding a second parser: they would have to re-implement Stage A/B/C, the per-worker key cache, the timestamp-promotion glue, the coalescing flush logic, and the telemetry plumbing.

This PRD is a **simplification pass with a 15 % regression budget**. The goal is a codebase where:

- Common pipeline machinery (TBB harness, batching, key cache, sink coalescing, timestamp post-decoding hook, telemetry) lives in **shared source files** that any parser can pull in.
- `JsonParser` becomes the **first user** of that toolkit, containing only JSON-specific code (simdjson value extraction, key-name handling, JSON-specific edge cases).
- The **public API surface shrinks to "give it a path, get a `LogData` / stream of batches back, with a `stop_token` for cancellation and an optional `LogConfiguration` for time-column promotion"** — no thread count, no batch size, no cache toggles. Advanced tunables move to a dedicated header that benchmarks and tests reach into.
- **Configuration-driven timestamp promotion stays** (the perf win is too good to lose) but is separated into a generic post-decoding hook the shared pipeline applies to any parser's output.
- The **code itself is the documentation**: short comments explain non-obvious intent, no PRD/§ references, no narration of obvious mechanics.

This is **not** a rewrite. The behaviour, public-facing semantics, and existing test suite all stay the same. We are extracting and renaming, not redesigning.

## 2. Goals

Goals split into **hard constraints** (must hold; a violation forces a revert) and **soft priorities** (the simplification's actual purpose; ordered, lower-numbered wins on conflict).

### Hard constraints (non-negotiable)

- **G0 — Zero functional regression.** Every existing Catch2 test under `test/lib/` and `test/app/` continues to pass. Test *logic and assertions* are unchanged; include paths, option-type names (`JsonParserOptions` → `ParserOptions` / `AdvancedParserOptions`) and option-struct field paths may be edited where the rename in 4.2 forces it. The `testStreamingParityVsLegacy` parity test passes byte-for-byte. The `[allocations]`, `[no_thread_local_cache]`, `[get_value_micro]`, `[cancellation]`, `[large]`, `[wide]` and `[stream_to_table]` benchmarks continue to compile and run.
- **G6 — Performance-loss floor.** **0 % regression is the expected outcome** on `[large]`, `[wide]`, `[stream_to_table]` and `[cancellation]` — this is a code-move, not a redesign. The 15 % drift on warm-up MB/s on `[large]` / `[wide]` / `[stream_to_table]` (and ±20 % on `[cancellation]` median/p95) is the **hard floor** before forced revert. Any single benchmark regressing by more than 5 % requires a documented architectural justification in the commit message even though it sits inside the floor.

### Soft priorities (ordered)

1. **G1 — Pluggability.** Adding a second structured log format (binary or text) should require only writing a parser class that implements the common interface plus its own format-specific decoding. The streaming pipeline, batching, key cache, timestamp promotion, telemetry and sink coalescing must all be reusable. **Verified programmatically** (req. 4.1.6) by an in-tree mock parser test, not just by inspection.
2. **G2 — Smaller JSON parser.** `library/src/json_parser.cpp` shrinks by **at least 30 %** of its current line count (1,511 → ≤ 1,050) by extracting all non-JSON-specific machinery into common files.
3. **G3 — Simpler public API.** Public headers expose only the `LogParser` interface (extended once with a streaming entry), the `StreamingLogSink` interface, the `StreamedBatch` struct, the data types (`LogLine`, `LogData`, `LogValue`, `KeyId`, `KeyIndex`, `LogFile`, `LogFileReference`), and `ParserOptions` containing **only** `std::stop_token` and the optional `LogConfiguration` (the two non-tuning inputs every consumer cares about). Every tuning knob (thread count, batch size, ntokens, cache toggles, telemetry pointer) moves to a separate `loglib::internal::AdvancedParserOptions` struct under `library/include/loglib/internal/` that benchmarks/tests opt into.
4. **G4 — Self-explanatory code.** Every `// PRD §…`, `// (PRD req. …)`, `// parser-perf task X.Y`, `// task N.M` reference is removed from source. Doc-comments shrink to single-line summaries on public symbols; private helpers carry no docstring unless the intent is non-obvious. Mid-function narration of obvious mechanics is removed. **Exception:** `@param` / `@return` notes that document a non-obvious contract — preconditions, lifetimes, ownership transfer, sorted-input invariants, or in/out parameter semantics — are preserved (req. 4.4.7). The bar is "could a competent caller misuse the API without this note?", not "could the parameter name carry the meaning on its own?".
5. **G5 — Config-aware timestamp pipeline.** The auto-timestamp-promotion currently inlined in `JsonParser`'s Stage B becomes a generic "post-decoding hook" that the shared pipeline applies to every parser's output. Auto-detection of timestamp columns (configuration-driven, with the same heuristics) keeps working without the GUI noticing the move.

## 3. User Stories

- **As a developer adding support for a new log format** (binary or text), I want to implement just a parser class that exposes `IsValid` and a streaming entry point, while reusing the threading, batching, key-interning, timestamp promotion and sink coalescing machinery, so I can land a working parser in days, not weeks.
- **As a junior maintainer reading the JSON parser for the first time**, I want the file to read top-to-bottom as "parse JSON values, hand them to the shared pipeline" without 200-line `WorkerState` structs or telemetry side-channels mixed in, so I can understand what the JSON parser actually does in a single sitting.
- **As a GUI / library consumer**, I want a small, public-only API: open a log file, hand the parser a `LogConfiguration` and a `stop_token`, and receive a stream of fully-parsed batches. I should not need to know about thread counts, batch sizes, or cache toggles — and `LogTable` should keep handing me ready-to-render rows.
- **As a parser-perf engineer**, I want benchmark/test code to still reach the advanced tunables (thread count, batch size, telemetry) through a clearly labelled `loglib::internal::` header, so existing benchmarks keep working without polluting the public surface.

## 4. Functional Requirements

The work splits into four parents. Each parent is a separate commit on the current branch and ships with a before/after `MB/s` line for `[large]`, `[wide]`, and `[stream_to_table]` per G6 (mirroring the predecessor PRDs' commit-message convention).

### 4.1 Extract a common streaming pipeline toolkit

1. Create new shared files under `library/src/` (and headers under `library/include/loglib/` where they belong on the public surface) that own the format-agnostic pipeline machinery:
    - The TBB Stage A/B/C `parallel_pipeline` harness, parameterised on a per-format "decode batch" callable.
    - The per-worker scratch container (currently `WorkerState`), parameterised on a per-format "per-worker state" type.
    - The Stage C sink coalescing logic (the 1 000-line / 50 ms flush thresholds + the uncoalesced fast-path branch).
    - The timestamp post-decoding hook (see 4.3).
    - The optional per-stage timing telemetry (`StageTimings`).
    - The per-worker key-cache front-end currently inlined in `json_parser.cpp` (`PerWorkerKeyCache`, `TransparentStringHash`, `TransparentStringEqual`, `InternKeyVia`).
2. The shared harness must accept a parser-supplied "decode one batch of bytes into `(LogLine, lineOffset, error)` triples" callable so a binary parser (no newline boundaries) can drive its own batching strategy via a different Stage A function while still reusing Stage B/C.
3. `JsonParser::ParseStreaming` becomes a thin wrapper that wires up: (a) Stage A's "find next newline ≥ batchSize" splitter, (b) Stage B's per-line simdjson + key-interning body, (c) the shared Stage C / sink coalescing harness.
4. The shared toolkit lives in `library/src/parser_pipeline.hpp/.cpp` and `library/src/timestamp_promotion.hpp/.cpp`; `json_parser.cpp` may not contain any class or function whose body has nothing JSON-specific in it.
5. After this requirement lands, `library/src/json_parser.cpp` is at most ~1,050 lines (G2 / M1).
6. **Programmatic pluggability test.** Add a tiny in-tree mock parser (`KeyValueLineParser` for `key=value key2=value2` text, ~100 LOC, *not* shipped through `LogFactory`) defined inline at the top of `test/lib/src/test_parser_pipeline.cpp` — the same TU that hosts its Catch2 cases. The cases must cover, at minimum: (a) a multi-batch parse producing the expected `LogLine`s and `newKeys`, (b) cancellation latency bounded by `ntokens × batchSize` (mirroring `[cancellation]`), (c) per-line errors propagating through `StreamedBatch::errors`, (d) timestamp promotion applied to the mock parser's output via the shared post-decoding hook from a `LogConfiguration` carrying a `Type::time` column on a mock-format key. This is the load-bearing verification of G1 — without it, "the harness is reusable" is opinion.
7. **Cancellation polling lives in the shared harness.** The `std::stop_token` from `ParserOptions` is consulted by the harness's Stage A driver (matching today's `cursor >= fileEnd || stopToken.stop_requested()` check). Parser-supplied Stage A splitters do *not* poll the token themselves; they receive an opaque "next batch" handle and return it.

### 4.2 Shrink and reshape the `LogParser` interface

1. `LogParser` (the public interface) gains the streaming entry point: `void ParseStreaming(LogFile&, StreamingLogSink&, ParserOptions = {}) const`.
2. The synchronous `Parse(path) -> ParseResult` overload stays on `LogParser` and is implemented exactly once on the base class as a thin, **non-virtual** call into `ParseStreaming` + `BufferingSink` (the legacy pure-virtual `Parse` becomes a regular member function with a default body). Concrete parsers (including `JsonParser`) no longer override or re-declare it. `IsValid` and `ToString` remain pure virtual.
3. `ParserOptions` is a struct in a public header (`library/include/loglib/parser_options.hpp`) containing **exactly two** fields: `std::stop_token stopToken` and `std::shared_ptr<const LogConfiguration> configuration`. Both are non-tuning inputs every consumer cares about (cancellation, time-column promotion). Every *tuning knob* — `threads`, `batchSizeBytes`, `ntokens`, `useThreadLocalKeyCache`, `useParseCache`, `timings` — moves to a `loglib::internal::AdvancedParserOptions` struct in `library/include/loglib/internal/parser_options.hpp` that benchmarks and tests include explicitly. The `internal/` directory and `loglib::internal::` namespace signal "library implementation detail; downstream consumers should not depend on this header's stability".
4. The advanced struct is wired through a separate `ParseStreaming(file, sink, ParserOptions, AdvancedParserOptions)` overload (or an "advanced" entry point on a sibling type), reachable only via the `internal/` header. Default callers never see it.
5. **Defaults reproduction.** A default-constructed `ParserOptions{}` paired with a default-constructed `AdvancedParserOptions{}` must reproduce today's `JsonParserOptions{}` defaults bit-for-bit (`threads=0`, `batchSizeBytes=1 MiB`, `ntokens=0`, `useThreadLocalKeyCache=true`, `useParseCache=true`, `configuration=nullptr`, `stopToken={}`, `timings=nullptr`). Existing default callers (`JsonParser parser; parser.Parse(path)`) must observe zero behaviour change.
6. The `JsonParserOptions` and `JsonParser::Options` aliases are removed; callers migrate to `ParserOptions` (GUI / `main_window.cpp`) or to `ParserOptions` + `AdvancedParserOptions` (benchmarks, tests that pin `threads = 1`, etc.).
7. `JsonParser`'s private nested `ParseCache` and `StreamingDetail` structs and the `friend struct StreamingDetail` declaration come out of the public header. Move `JsonParser::ParseLine` from a private static class member to a file-static helper in `json_parser.cpp` so the public header no longer needs to forward-declare `detail::PerWorkerKeyCache` either. The public `library/include/loglib/json_parser.hpp` shrinks dramatically — it should fit on one screen.
8. `LogFactory` (today `library/include/loglib/log_factory.hpp` + `log_factory.cpp`) keeps both its `Parser` enum + `Create` entry point **and** `LogFactory::Parse(path)`. The latter is the auto-detect facade across registered parsers and has live callers (`app/src/main_window.cpp:799` — the legacy non-streaming "open recent file" path — and `test/lib/src/test_log_factory.cpp` `[log_factory]` cases). Both call sites stay on `LogFactory::Parse` post-4.2; this PRD does not migrate them.

### 4.3 Move automatic timestamp detection / promotion to shared code

1. The timestamp-related shared API in `library/include/loglib/log_processing.hpp` (`ClassifyTimestampFormat`, `TryParseIsoTimestamp`, `TryParseGenericTimestamp`, `TryParseTimestamp`, `LastValidTimestampParse`, `TimestampParseScratch`, `BackfillTimestampColumn`, `ParseTimestampLine`) is reviewed and trimmed to the minimum surface the shared pipeline + `LogTable` need.
2. Inline Stage-B timestamp promotion is **preserved** but moves into the shared streaming pipeline harness, not `json_parser.cpp`. The harness pre-resolves the configuration's `Type::time` columns into KeyIds at pipeline start and applies the promotion to every parser's emitted `LogLine`s.
3. The "auto-detect a column as time-typed by header name" heuristic currently in `LogConfigurationManager::Update` / `AppendKeys` keeps the same behaviour from the GUI's perspective. Its implementation may be reorganised but the decision rule (which keys auto-promote to `Type::time`) is unchanged.
4. The `LogConfiguration`-aware path through the parser is **encapsulated**: parser bodies do not interpret `LogConfiguration` directly. The shared harness reads `ParserOptions::configuration` (a `std::shared_ptr<const LogConfiguration>`), extracts a flat `TimeColumnSpec` list (KeyIds + format strings + pre-classified `TimestampFormatKind`s — matching today's `JsonParser::StreamingDetail::TimeColumnSpec`) once at pipeline start, and applies the timestamp hook on each parser's emitted batch. `TimeColumnSpec` is the **only** type the harness pulls out of `LogConfiguration` before invoking parser code; parser callbacks see `std::span<const TimeColumnSpec>`, never `LogConfiguration`. Place `TimeColumnSpec` in the shared toolkit (`library/src/parser_pipeline.hpp`).
5. `LogTable::AppendBatch`'s mid-stream back-fill for time columns auto-promoted *after* the configuration snapshot stays as a correctness safety net for keys that appear later in the stream (current behaviour, unchanged).
6. The shared hook keeps the zero-cost no-config path: when no `Type::time` columns are configured, the per-batch promotion loop must not execute (matches today's `if (!timeColumns.empty())` guard).

### 4.4 Code hygiene pass

1. Remove every `PRD §…`, `(PRD req. …)`, `parser-perf task …`, `task X.Y` reference from every `.hpp`, `.cpp`, and inline doc-comment under `library/`, `app/`, and `test/`.
2. Doc-comments on public types/methods shrink to a one-line summary stating *what* the symbol is. Multi-paragraph rationale, references to other code, and historical context are removed.
3. Remove all mid-function narrative comments that simply restate what the next 3 lines do (e.g. `// Step 2: splice the lines …` directly above `lines.insert(...)`).
4. Inline doc-comments on private helpers, lambdas, and nested types are removed unless the intent is non-obvious.
5. Empty / fluff doc comments (`@brief Default constructor for X.`, `@param x The x.`, parameter descriptions that just repeat the parameter name, `@return The number of …` for an obviously-named getter) come out.
6. Tasks/PRD documents under `tasks/` are **left alone**. Only source-code- and source-comment-level references are removed; predecessor PRDs and task lists stay verbatim as historical record. The companion task list this PRD spawns (`tasks/tasks-parser-simplification.md`) is the single new file there.
7. **Doc-comment safety net.** Preserve `@param` / `@return` / inline notes that document a contract a competent caller could otherwise violate, including:
    - **Preconditions** (e.g. `BackfillTimestampColumn`'s "Caller must ensure `column.type == LogConfiguration::Type::time`").
    - **Lifetime / aliasing** (e.g. "the returned `string_view` points into the mmap and outlives the `LogLine`").
    - **Ownership transfer** (e.g. `BufferingSink(std::unique_ptr<LogFile>)` — sink takes ownership).
    - **Sorted-input invariants** (e.g. `LogLine`'s pre-sorted-by-KeyId constructor contract).
    - **In/out parameter semantics** (e.g. `lastValid` "in/out cache").
    - **Failure-mode contracts** (e.g. "silently returns false; does not touch the line").
    The bar is "could a competent caller misuse the API without this note?". Keep what defends against misuse; drop everything else.

## 5. Non-Goals (Out of Scope)

- **Adding a second shipped parser implementation.** This PRD only restructures so a future parser is easy to add; it does not write a production one. The `KeyValueLineParser` mock that req. 4.1.6 introduces lives entirely under `test/lib/src/`, is not registered with `LogFactory`, and is never reachable from the GUI — its sole purpose is to exercise the shared toolkit's pluggability contract programmatically.
- **Changes to `LogValue`, `LogLine`, `LogData`, `KeyIndex`, `KeyId`, `LogFile`, `LogFileReference` data shapes.** Their public APIs are frozen.
- **Changes to `StreamingLogSink` / `StreamedBatch` semantics or lifecycle.** The Qt streaming sink and `BufferingSink` keep working unchanged.
- **Changes to the benchmark suite or the `[large]` / `[wide]` / `[stream_to_table]` / `[allocations]` / `[cancellation]` / `[no_thread_local_cache]` / `[get_value_micro]` fixtures**, except for the include-path adjustments needed to pull in `<loglib/internal/parser_options.hpp>` and the option-struct rename.
- **Removing performance optimisations** like the per-worker key cache, the relative-line stamping in Stage C, the SIMDJSON_PADDING tail-line scratch, the heterogeneous-lookup transparent hash adapter, or the Stage-B timestamp promotion. They all stay; they just move to where they belong (shared code where format-agnostic, JSON-specific files where JSON-specific).
- **Modifying old PRD or task documents** under `tasks/` (kept verbatim as historical record).
- **Changes to dependency management** (`cmake/FetchDependencies.cmake`, simdjson, oneTBB, fmt, glaze, mio, robin_map, date, Catch2 are all preserved as-is).
- **Re-tuning the per-stage timing telemetry struct.** `StageTimings` field shape stays identical; it just moves to the shared-toolkit header.
- **Re-baselining performance.** The 15 % regression budget keys off the post-`hl-inspired` numbers captured in `tasks/tasks-hl-inspired-parser-performance.md` task 6.1.

## 6. Design Considerations

- **File layout (suggested, not prescriptive):**
    - `library/include/loglib/parser_options.hpp` — public `ParserOptions { std::stop_token stopToken; std::shared_ptr<const LogConfiguration> configuration; }`.
    - `library/include/loglib/internal/parser_options.hpp` — opt-in tunables (thread count, batch size, ntokens, cache toggles, telemetry pointer); declares `loglib::internal::AdvancedParserOptions`.
    - `library/src/parser_pipeline.hpp` / `.cpp` — TBB harness, Stage C coalescing, telemetry plumbing, per-worker scratch container, the `TimeColumnSpec` value type the harness extracts from `LogConfiguration` before invoking parser code (req. 4.3.4).
    - `library/src/per_worker_key_cache.hpp` (or stays inside `parser_pipeline.*`) — the transparent-hash `tsl::robin_map` and `InternKeyVia` helper.
    - `library/src/timestamp_promotion.hpp` / `.cpp` — Stage-B-style inline promotion hook keyed off the harness's `TimeColumnSpec` list. (`log_processing.*` keeps the time-zone init / formatting helpers.)
    - `library/src/json_parser.cpp` — only JSON-specific code, plus the wiring lambda that hands its decode body to the shared pipeline.
    - `library/src/buffering_sink.{hpp,cpp}` — already format-agnostic, stays put; used by `LogParser::Parse(path)`'s base-class default.
- **Include graph.** `log_parser.hpp` will include `parser_options.hpp` directly (so the streaming entry's defaulted `ParserOptions = {}` parameter is well-formed at the declaration site). `parser_options.hpp` includes `log_configuration.hpp` (for the `shared_ptr<const LogConfiguration>` member). Neither public header includes anything under `loglib/internal/`; benchmarks/tests `#include <loglib/internal/parser_options.hpp>` explicitly.
- **Extension shape for a hypothetical new parser** (binary example, illustrative):
    1. `class MyBinaryParser : public LogParser` with `IsValid(path)` peeking at a magic-byte header.
    2. Inside `ParseStreaming(file, sink, options)`, call the shared pipeline with: (a) a Stage A splitter that walks the binary's record-length prefixes (instead of `memchr('\n')`), (b) a Stage B "decode one record" body that fills a `LogLine`, (c) a per-worker state type (or the default if none needed). Get the timestamp promotion, sink coalescing, telemetry, key cache, and cancellation for free.
- **Mock parser test as G1's load-bearing verification.** Per req. 4.1.6, a small `KeyValueLineParser` (text format `key=value key2=value2 …`, ~100 LOC, lives in `test/lib/src/`) is the artefact that turns "the harness is reusable" from an opinion into a passing test. It does *not* register with `LogFactory` and is not exposed to the GUI; it only exists so the shared toolkit keeps a non-JSON consumer to keep itself honest.

## 7. Technical Considerations

- The shared pipeline harness will likely need to be a C++ template (parameterised on the per-format Stage A token type, the parser's per-worker state type, and the Stage A/B callables) so binary parsers can pick a different Stage A token granularity. The instantiation cost is paid once per parser; the per-call code path generated for JSON should match today's hand-rolled one closely enough that G6's 15 % budget is comfortable.
- Moving private nested types (`ParseCache`, `StreamingDetail`) out of `JsonParser` may require a small `JsonParser::Impl` PIMPL or just file-static helpers. Either is fine; whichever keeps the public header smaller wins.
- `ParserOptions` ↔ `loglib::internal::AdvancedParserOptions` split must preserve source-compat in the sense that `ParseStreaming(file, sink, ParserOptions)` works with no advanced struct passed; benchmarks/tests adopt the second overload explicitly. Default values across the two structs combine to reproduce today's `JsonParserOptions{}` defaults bit-for-bit (req. 4.2.5).
- **Binary / source compatibility.** `LogParser::Parse(path)` changes from pure virtual to a regular member function with a default body. We do not ship `loglib` as a binary library to external downstreams (`LogFactory::Create` is the only registered way to construct parsers, both `app/` and `test/` consume `loglib` as a static library), so this is a source-only break and only affects the in-tree `JsonParser` (which loses its override). Any external code that derived from `LogParser` and overrode `Parse` will silently keep its override; the new default is harmless to it. No deprecation cycle is needed.
- The base-class `Parse(path) -> ParseResult` default needs to construct a `BufferingSink`, which in turn needs the file already open. The cleanest shape is for the base class to open the `LogFile` once and pass it to both the sink and `ParseStreaming` (matching today's `JsonParser::Parse` body verbatim).
- `LogTable::BeginStreaming` / `LogTable::AppendBatch` paths (mid-stream back-fill for late time columns) are already format-agnostic and stay unchanged.
- After 4.4 strips PRD references, search the codebase one more time for orphaned `§` characters, `parser-perf` strings, `task N.M` patterns and the literal substring `PRD` to make sure no stragglers slip through. A simple `rg` is sufficient.
- The existing CI matrix (`build-linux`, `build-linux-system-tbb`, `build-macos`, `build-windows-msvc`) must keep going green. No new compilers / new toolchains are introduced by this PRD.

## 8. Success Metrics

| ID | Metric | Current branch | Target after PRD | How measured |
|---|---|---|---|---|
| **M1** | `library/src/json_parser.cpp` line count | 1,511 | **≤ 1,050** (≥ 30 % reduction per G2) | `wc -l library/src/json_parser.cpp` |
| **M2** | `[large]` warm-up MB/s | ~1,158 | **≥ 985** (≤ 15 % regression per G6) | `benchmarks.exe "[…benchmark…large]"` |
| **M3** | `[wide]` warm-up MB/s | ~1,354 | **≥ 1,151** (≤ 15 %) | `benchmarks.exe "[…wide]"` |
| **M4** | `[stream_to_table]` warm-up MB/s | ~150 | **≥ 128** (≤ 15 %) | `benchmarks.exe "[…stream_to_table]"` |
| **M5** | Tuning-knob fields on the public `ParserOptions` (i.e. excluding `stopToken` and `configuration`, the two non-tuning inputs) | 6 — `threads`, `batchSizeBytes`, `ntokens`, `useThreadLocalKeyCache`, `useParseCache`, `timings` all live on the public `JsonParserOptions` today | **0** (every tuning knob lives on `loglib::internal::AdvancedParserOptions` in `loglib/internal/parser_options.hpp`) | manual review of `library/include/loglib/parser_options.hpp` and `library/include/loglib/json_parser.hpp` |
| **M6** | `PRD\b`, `§`, `parser-perf`, `task \d+\.\d+` references in `library/`, `app/`, `test/` source | many (hundreds; `library/src/json_parser.cpp` alone has 58) | **0** | `rg "PRD\b\|§\|parser-perf\|task \d+\.\d+" library app test` |
| **M7** | Unit tests passing | 95/95 | **95/95 + new `[mock_parser]` cases from req. 4.1.6** | `ctest --preset local` |
| **M8** | `testStreamingParityVsLegacy` | passes | **passes** | the named Catch2 test |
| **M9** | `[cancellation]` median / p95 | ~3.4 ms / ~6.9 ms | **within ±20 %** of current values | `[cancellation]` benchmark |
| **M10** | Pluggability — the shared toolkit is exercised by a non-JSON parser in-tree | none | **`KeyValueLineParser` mock + Catch2 cases land per req. 4.1.6, covering multi-batch parse, cancellation, error propagation, and timestamp promotion** | review of `test/lib/src/test_parser_pipeline.cpp` |
| **M11** | `loglib::JsonParserOptions` / `JsonParser::Options` references in `library/`, `app/`, `test/` source | 70+ (per `rg`) | **0** (renamed to `ParserOptions` / `AdvancedParserOptions`) | `rg "JsonParserOptions\|JsonParser::Options" library app test` |

## 9. Open Questions

No PRD-level open questions remain. Earlier-round decisions are recorded below for the implementer's audit trail; do not relitigate them without amending §4.

- **Advanced-options header location** → `library/include/loglib/internal/parser_options.hpp`, namespace `loglib::internal`, struct name `AdvancedParserOptions`. Rationale: `internal/` is a stronger "do not depend on this" signal than `advanced/`, matching the intent that benchmarks and tests are the only sanctioned consumers.
- **Shared pipeline filename** → `library/src/parser_pipeline.{hpp,cpp}`. Rationale: "parser pipeline" reads as "the pipeline parsers plug into", not as "this file does streaming I/O" (which `streaming_pipeline` invited).
- **Mock parser placement (req. 4.1.6)** → defined inline at the top of the single TU `test/lib/src/test_parser_pipeline.cpp`, alongside its Catch2 cases. The PRD budgets ~100 LOC for the parser itself; if it organically grows past ~200 LOC during implementation, lift it into `test/lib/src/mock_parsers/key_value_line_parser.{hpp,cpp}` and keep the test TU thin — that move is an implementation-time call, not a PRD revision.

Anything else uncovered during 4.x (e.g. exact `KeyValueLineParser` grammar edge cases, telemetry-pointer ABI inside `AdvancedParserOptions`) is an implementation detail and lives in commit messages / inline comments, not here.
