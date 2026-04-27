# Tasks: Parser Simplification & Common Pipeline Toolkit

Source PRD: [prd-parser-simplification.md](./prd-parser-simplification.md)

Each parent task 1.0–4.0 maps to one PRD functional-requirement section (§4.1–§4.4) and ships as **a single commit on `feature/improve-performance-and-add-streaming`**, with a before/after `MB/s` line for `[large]`, `[wide]`, and `[stream_to_table]` in the commit message per G6. Task 5.0 verifies the success metrics (M1–M11) before the work is considered done.

## Relevant Files

### Library — public headers (will change)

- `library/include/loglib/log_parser.hpp` — `LogParser` interface; gains `ParseStreaming` and a non-virtual default `Parse(path)` body (PRD §4.2.1–4.2.2).
- `library/include/loglib/parser_options.hpp` **(new)** — public `ParserOptions { std::stop_token stopToken; std::shared_ptr<const LogConfiguration> configuration; }` (PRD §4.2.3, §6).
- `library/include/loglib/internal/parser_options.hpp` **(new)** — `loglib::internal::AdvancedParserOptions` with every tuning knob (PRD §4.2.3, §6).
- `library/include/loglib/json_parser.hpp` — slimmed down: drops `JsonParserOptions`, `Options` alias, `ParseCache`, `StreamingDetail`, `friend struct StreamingDetail`, `detail::PerWorkerKeyCache` forward decl, private static `ParseLine` (PRD §4.2.6–4.2.7).
- `library/include/loglib/log_processing.hpp` — trimmed to the minimum surface the shared pipeline + `LogTable` need (PRD §4.3.1).
- `library/include/loglib/streaming_log_sink.hpp` — comments scrubbed (currently references `JsonParserOptions` / `PRD`).
- `library/include/loglib/log_table.hpp`, `log_configuration.hpp`, `log_line.hpp`, `log_data.hpp`, `log_file.hpp`, `key_index.hpp`, `log_factory.hpp` — comment hygiene (PRD §4.4); no API changes.

### Library — internal sources (will change)

- `library/src/parser_pipeline.hpp` / `parser_pipeline.cpp` **(new)** — TBB Stage A/B/C harness, per-worker scratch container, Stage C sink coalescing, optional `StageTimings` plumbing, `TimeColumnSpec` value type (PRD §4.1.1, §4.1.4, §4.3.4, §6).
- `library/src/per_worker_key_cache.hpp` **(new, or co-located in `parser_pipeline.hpp`)** — `PerWorkerKeyCache`, `TransparentStringHash`, `TransparentStringEqual`, `InternKeyVia` (PRD §4.1.1, §6).
- `library/src/timestamp_promotion.hpp` / `timestamp_promotion.cpp` **(new)** — generic post-decoding timestamp hook keyed off `TimeColumnSpec` (PRD §4.1.1, §4.3.2, §6).
- `library/src/json_parser.cpp` — only JSON-specific code remains; `ParseStreaming` becomes a thin wrapper around the shared pipeline; `ParseLine` becomes a file-static helper (PRD §4.1.3–4.1.5, M1: ≤ 1,050 lines).
- `library/src/buffering_sink.hpp` / `buffering_sink.cpp` — already format-agnostic, kept; consumed by the new `LogParser::Parse(path)` base default.
- `library/src/log_factory.cpp` — keeps both `Parse(path)` and `Create(Parser)` (PRD §4.2.8); only call-site adjustments needed.
- `library/src/log_processing.cpp`, `log_table.cpp`, `log_data.cpp`, `log_line.cpp`, `key_index.cpp`, `log_configuration.cpp`, `log_file.cpp` — comment hygiene (PRD §4.4); minor changes to mirror trimmed `log_processing.hpp`.
- `library/CMakeLists.txt` — register the new `.hpp/.cpp` files in the `loglib` static library target.

### App (consumer of public API)

- `app/src/main_window.cpp` — migrate the streaming-parser call site from `JsonParserOptions` to `ParserOptions` (PRD §4.2.6); no functional change.
- `app/include/log_model.hpp` / `app/src/log_model.cpp` — same rename + comment scrub.
- `app/include/qt_streaming_log_sink.hpp` / `app/src/qt_streaming_log_sink.cpp` — same rename + comment scrub.
- `app/include/main_window.hpp` — comment scrub.

### Tests / benchmarks

- `test/lib/src/test_parser_pipeline.cpp` **(new)** — defines the `KeyValueLineParser` mock inline at the top of the file plus its Catch2 cases (PRD §4.1.6, §6).
- `test/lib/src/benchmark_json.cpp` — migrate to `ParserOptions` + `loglib::internal::AdvancedParserOptions`; keep every existing tag/fixture (`[allocations]`, `[no_thread_local_cache]`, `[get_value_micro]`, `[cancellation]`, `[large]`, `[wide]`, `[stream_to_table]`).
- `test/lib/src/test_json_parser.cpp` — same migration; keep parity test `testStreamingParityVsLegacy` byte-identical (PRD G0).
- `test/lib/src/test_log_factory.cpp` — left functionally unchanged (`[log_factory]` cases stay on `LogFactory::Parse`); comment hygiene only.
- `test/lib/src/test_log_processing.cpp`, `test_log_table.cpp`, `test_key_index.cpp`, `test_log_line.cpp`, `test_log_file.cpp`, `test_log_data.cpp`, `test_log_configuration.cpp`, `common.cpp` — comment hygiene; no logic changes.
- `test/app/src/main_window_test.cpp` — option-name rename + comment hygiene.
- `test/lib/CMakeLists.txt` — register `test_parser_pipeline.cpp` in the `loglib_tests` target; comment hygiene.

### Notes

- **Hard floors (G0, G6).** Every existing Catch2 test under `test/lib/` and `test/app/` must keep passing. Warm-up MB/s on `[large]` / `[wide]` / `[stream_to_table]` may regress at most 15 % vs. current branch numbers; `[cancellation]` median/p95 may drift at most ±20 %. Any single benchmark regressing more than 5 % requires an architectural justification in the commit message even though it sits inside the floor.
- **This is an extract-and-rename pass, not a redesign.** Public-facing semantics, the parity test (`testStreamingParityVsLegacy`), and `LogTable` / GUI behaviour must not change. The 0 % regression target is the expected outcome; the 15 % is only a forced-revert floor.
- **Branch policy.** All work commits directly onto `feature/improve-performance-and-add-streaming`. Each parent task is one commit; commit messages must include a before/after `MB/s` line for `[large]`, `[wide]`, and `[stream_to_table]`.
- **Predecessor PRDs and task lists under `tasks/` are historical record** — do **not** edit them. The only new doc artefact is this file (PRD §4.4.6).
- **Build presets.** Existing CMake presets continue to drive the build; `local` is the developer preset (`ctest --preset local` runs the whole suite). The new `.hpp/.cpp` files must be added to the `loglib` and `loglib_tests` targets.

## Tasks

- [x] 1.0 Extract a common streaming pipeline toolkit (PRD §4.1)
  - [x] 1.1 Create `library/src/parser_pipeline.hpp` / `.cpp` with the shared TBB Stage A/B/C `parallel_pipeline` harness, parameterised on a per-format Stage A token type, a per-worker state type, and Stage A / Stage B callables (PRD §4.1.1, §4.1.2, §7). Carry over today's `ntokens` defaulting (`0` → `2 * effectiveThreads`) and the cooperative `stop_token` poll on Stage A's `cursor >= fileEnd || stopToken.stop_requested()` check (PRD §4.1.7).
  - [x] 1.2 Move the per-worker scratch container (today `WorkerState` in `json_parser.cpp`) into the shared harness as a generic `enumerable_thread_specific<WorkerScratch<UserState>>` parameter. The default `UserState` is empty so binary parsers can opt out.
  - [x] 1.3 Move the Stage C sink coalescing logic out of `json_parser.cpp` — keep the existing 1 000-line / 50 ms flush thresholds, the uncoalesced fast path branch, and the relative-line-number stamping (PRD §4.1.1 / §5).
  - [x] 1.4 Move `StageTimings` and its plumbing (Stage A/B/C CPU totals, `wallClockTotal`, `effectiveThreads`, batch counts, `sinkTotal`) into the shared harness. Keep the field shape bit-identical (PRD §5: "no re-tuning of telemetry"); only the location changes. *(Plumbing moved to harness; the struct's home stays in `json_parser.hpp` until task 2.3 retags it.)*
  - [x] 1.5 Create `library/src/per_worker_key_cache.hpp` (or place inside `parser_pipeline.hpp`) holding `PerWorkerKeyCache`, `TransparentStringHash`, `TransparentStringEqual`, and `InternKeyVia`, lifted verbatim from `json_parser.cpp`. Drop the `loglib::detail::PerWorkerKeyCache` forward declaration from `library/include/loglib/json_parser.hpp` (PRD §4.1.1, §4.2.7). *(Co-located in `parser_pipeline.hpp`; the forward decl is removed in task 2.7.)*
  - [x] 1.6 Create `library/src/timestamp_promotion.hpp` / `.cpp` exposing the post-decoding hook the harness applies to every emitted batch. The hook takes `std::span<const TimeColumnSpec>` and a per-worker `TimestampParseScratch` slice and reuses the existing `ParseTimestampLine` / `LastValidTimestampParse` fast path (PRD §4.1.1, §4.3.2). Wire the hook into the harness so it runs on every parser's output, not just JSON's. *(Implemented as `WorkerScratchBase::PromoteTimestamps`, called inline per `LogLine` to keep the line hot in L1; a per-batch post-decode loop showed up as a 7 % regression on `[stream_to_table]`.)*
  - [x] 1.7 Place `TimeColumnSpec` (KeyIds + format strings + pre-classified `TimestampFormatKind`s) in `timestamp_promotion.hpp`. The harness is responsible for snapshotting it from `ParserOptions::configuration` once at pipeline start; parser callbacks see only `std::span<const TimeColumnSpec>`, never `LogConfiguration` (PRD §4.3.4).
  - [x] 1.8 Update `library/CMakeLists.txt` to add the new `.hpp` / `.cpp` files to the `loglib` static-library target. Confirm the build still produces `loglib`, `loglib_app`, `loglib_tests`, and `loglib_benchmarks` with no missing-symbol errors.
  - [x] 1.9 Refactor `JsonParser::ParseStreaming` (in `library/src/json_parser.cpp`) into a thin wrapper that supplies the shared pipeline with: (a) a Stage A "find next newline ≥ batchSize" splitter, (b) a Stage B "decode one line via simdjson + key-interning" body, (c) a JSON-specific per-worker state. Stage C / sink coalescing / telemetry come from the shared harness for free (PRD §4.1.3).
  - [x] 1.10 Move `JsonParser::ParseLine` from a private static class member to a file-static helper in `json_parser.cpp` so the public header no longer needs the `detail::PerWorkerKeyCache` forward declaration (PRD §4.2.7). Keep its parameters bit-identical for now (the option-rename happens in task 2.0); the `useThreadLocalKeyCache` / `useParseCache` toggles route through the advanced struct in the next parent. *(File-static `ParseJsonLine` is the new home; the public `JsonParser::ParseLine` member stays as a thin shim until task 2.7 strips the header.)*
  - [x] 1.11 Verify `json_parser.cpp` no longer contains any class or function whose body has nothing JSON-specific in it (PRD §4.1.4). Re-run `wc -l library/src/json_parser.cpp`; the target ≤ 1,050 may not be met until task 4.0 strips comments, but the structural extraction must already be complete. *(670 lines after this task; M1 floor already cleared.)*
  - [x] 1.12 Add `test/lib/src/test_parser_pipeline.cpp` and register it in `test/lib/CMakeLists.txt`. Define `KeyValueLineParser` (text format `key=value key2=value2 …`, ~100 LOC, *not* registered with `LogFactory`) inline at the top of the same TU as its Catch2 cases (PRD §4.1.6, §6, Open Q.3). If the parser organically passes ~200 LOC during implementation, lift it into `test/lib/src/mock_parsers/key_value_line_parser.{hpp,cpp}` and keep the test TU thin.
  - [x] 1.13 Add Catch2 cases under tag `[mock_parser]` (or similar) covering at least: (a) multi-batch parse producing the expected `LogLine`s and `newKeys`, (b) cancellation latency bounded by `ntokens × batchSize` (mirroring `[cancellation]`), (c) per-line errors propagating through `StreamedBatch::errors`, (d) timestamp promotion applied via the shared post-decoding hook from a `LogConfiguration` carrying a `Type::time` column on a mock-format key (PRD §4.1.6).
  - [x] 1.14 Run `ctest --preset local`. All 95 existing tests + the new `[mock_parser]` cases pass; `testStreamingParityVsLegacy` passes byte-for-byte; `[allocations]`, `[no_thread_local_cache]`, `[get_value_micro]`, `[cancellation]`, `[large]`, `[wide]`, `[stream_to_table]` all compile and run (PRD G0). *(103/103 passing — 99 pre-existing + 4 `[mock_parser]` cases.)*
  - [x] 1.15 Capture before/after `MB/s` for `[large]`, `[wide]`, `[stream_to_table]` (PRD §4 commit-message convention; expected drift ~0 %). *(See commit message; same-machine before/after taken via stashed baseline.)*
  - [x] 1.16 Commit to `feature/improve-performance-and-add-streaming` with a message that includes the §1.15 numbers plus a one-line architectural justification for any single benchmark that regressed > 5 % (PRD G6).

- [x] 2.0 Shrink and reshape the `LogParser` interface (PRD §4.2)
  - [x] 2.1 Add `library/include/loglib/parser_options.hpp` defining `loglib::ParserOptions` with **exactly two** fields: `std::stop_token stopToken` and `std::shared_ptr<const LogConfiguration> configuration`. The header includes `log_configuration.hpp` so the `shared_ptr<const LogConfiguration>` member is well-formed (PRD §4.2.3, §6).
  - [x] 2.2 Add `library/include/loglib/internal/parser_options.hpp` defining `loglib::internal::AdvancedParserOptions` with the six tuning knobs: `unsigned int threads`, `size_t batchSizeBytes`, `size_t ntokens`, `bool useThreadLocalKeyCache`, `bool useParseCache`, `mutable StageTimings *timings`. Carry over the `kDefaultMaxThreads = 8` and `kDefaultBatchSizeBytes = 1 MiB` constants; default-construct every other field to today's `JsonParserOptions{}` defaults (PRD §4.2.3, §4.2.5).
  - [x] 2.3 Move the `StageTimings` struct definition out of `library/include/loglib/json_parser.hpp` into the shared toolkit (`library/src/parser_pipeline.hpp`, exported via `library/include/loglib/internal/parser_options.hpp` so benchmarks can still see it). Field shape stays bit-identical (PRD §5: "no re-tuning of telemetry").
  - [x] 2.4 Extend `library/include/loglib/log_parser.hpp` with `void ParseStreaming(LogFile&, StreamingLogSink&, ParserOptions = {}) const;` as the new pluggability surface (PRD §4.2.1). Include `parser_options.hpp` directly so the defaulted parameter is well-formed at the declaration site (PRD §6 include-graph rule).
  - [x] 2.5 Demote `LogParser::Parse(const std::filesystem::path&)` from pure-virtual to a regular member function with a default body that opens a `LogFile`, builds a `BufferingSink`, calls `ParseStreaming(file, sink, ParserOptions{})`, and returns `ParseResult{ sink.TakeData(), sink.TakeErrors() }`. `IsValid` and `ToString` stay pure-virtual (PRD §4.2.2, §7 binary/source-compat note).
  - [x] 2.6 Add a sibling overload reachable only via `<loglib/internal/parser_options.hpp>`: `void ParseStreaming(LogFile&, StreamingLogSink&, ParserOptions, AdvancedParserOptions) const;` (or an "advanced" entry point on a sibling type). Default callers continue to see only the two-arg form (PRD §4.2.4).
  - [x] 2.7 Strip the public `library/include/loglib/json_parser.hpp` down to: `IsValid`, `ToString(const LogLine&)`, `ToString(const LogMap&)`, and the `ParseStreaming` override declaration. Remove `JsonParserOptions`, `using Options = JsonParserOptions;`, `struct StreamingDetail;`, `friend struct StreamingDetail;`, `struct ParseCache`, the static `ParseLine` declaration, and the `loglib::detail::PerWorkerKeyCache` forward decl. The header should fit on roughly one screen (PRD §4.2.7).
  - [x] 2.8 In `library/src/json_parser.cpp`, remove `JsonParser::Parse(path)` and `JsonParser::Parse(path, JsonParserOptions)` overrides — both are now redundant against the base class default and the advanced overload. Internal helpers move to file-static. Confirm the `friend struct StreamingDetail` declaration is gone (PRD §4.2.2, §4.2.7).
  - [x] 2.9 Migrate the GUI call site `app/src/main_window.cpp:799` (legacy non-streaming "open recent file") and the streaming-parser call site to `ParserOptions` (no advanced struct). Verify `LogFactory::Parse(path)` and `LogFactory::Create(Parser)` semantics are unchanged (PRD §4.2.8).
  - [x] 2.10 Migrate `app/include/log_model.hpp`, `app/src/log_model.cpp`, `app/include/qt_streaming_log_sink.hpp`, and `app/src/qt_streaming_log_sink.cpp` from `JsonParserOptions` to `ParserOptions` (no advanced struct needed; consumers only set `stopToken` and `configuration`).
  - [x] 2.11 Migrate `test/lib/src/benchmark_json.cpp` to `ParserOptions` + `loglib::internal::AdvancedParserOptions`. The benchmarks that pin `threads = 1`, set `batchSizeBytes`, `ntokens`, toggle `useThreadLocalKeyCache` / `useParseCache`, or read `timings` use the advanced struct; the rest use defaults. Keep every existing tag, fixture, and assertion.
  - [x] 2.12 Migrate `test/lib/src/test_json_parser.cpp` and `test/app/src/main_window_test.cpp` to the new option types. Keep `testStreamingParityVsLegacy` byte-identical at the assertion level — only option-construction syntax may change (PRD G0).
  - [x] 2.13 Verify defaults reproduction: `rg "JsonParserOptions|JsonParser::Options" library app test` returns **zero hits** (M11). A default-constructed `ParserOptions{}` paired with default-constructed `AdvancedParserOptions{}` reproduces today's `JsonParserOptions{}` defaults bit-for-bit; assert this with a short Catch2 case in `test_json_parser.cpp` if not already covered (PRD §4.2.5).
  - [x] 2.14 Run `ctest --preset local`. All tests pass; `testStreamingParityVsLegacy` still passes byte-for-byte; existing default-constructed callers (`JsonParser parser; parser.Parse(path)`) observe zero behaviour change. *(104/104 tests passing.)*
  - [x] 2.15 Capture before/after `MB/s` for `[large]`, `[wide]`, `[stream_to_table]` (expected drift ~0 % — this is a rename + interface-shape refactor). *(See commit message.)*
  - [x] 2.16 Commit to `feature/improve-performance-and-add-streaming` with the §2.15 numbers in the message and any > 5 % per-benchmark regression justified inline (PRD G6).

- [ ] 3.0 Move automatic timestamp detection / promotion to shared code (PRD §4.3)
  - [ ] 3.1 Audit `library/include/loglib/log_processing.hpp` against actual call sites in the shared pipeline and `LogTable`. Delete or move-into-`.cpp` any helper that survives only to support the JSON-specific Stage B inline promotion (PRD §4.3.1). Keep timezone init / formatting helpers in `log_processing.*`.
  - [ ] 3.2 Wire the harness's `TimeColumnSpec` snapshot (created in task 1.7) to feed `library/src/timestamp_promotion.cpp`'s post-decoding hook. The hook runs on every parser's emitted batch; parsers no longer interpret `LogConfiguration` directly (PRD §4.3.2, §4.3.4).
  - [ ] 3.3 Confirm the zero-cost no-config path: when `ParserOptions::configuration == nullptr` or no `Type::time` columns are configured, the per-batch promotion loop must not execute (PRD §4.3.6, today's `if (!timeColumns.empty())` guard).
  - [ ] 3.4 Verify the GUI-facing auto-detection rule in `LogConfigurationManager::Update` / `AppendKeys` is unchanged — same heuristic, same set of auto-promoted keys; only the implementation may be reorganised (PRD §4.3.3).
  - [ ] 3.5 Confirm `LogTable::AppendBatch`'s mid-stream back-fill for late-arriving time columns still runs as the correctness safety net for keys that appear after the configuration snapshot (PRD §4.3.5).
  - [ ] 3.6 Extend the mock-parser test (task 1.13) to cover the new shared timestamp-promotion path end-to-end via a `LogConfiguration` carrying a `Type::time` column on a mock-format key (PRD §4.1.6 (d)).
  - [ ] 3.7 Run `ctest --preset local`. All tests pass, including `[stream_to_table]` and the timestamp-related cases in `test_log_processing.cpp` and `test_log_table.cpp`.
  - [ ] 3.8 Capture before/after `MB/s` for `[large]`, `[wide]`, `[stream_to_table]`. `[stream_to_table]` is the bench most sensitive to this change; flag and justify any regression > 5 % in the commit message (PRD G6).
  - [ ] 3.9 Commit to `feature/improve-performance-and-add-streaming` with the §3.8 numbers in the message.

- [ ] 4.0 Code hygiene pass (PRD §4.4)
  - [ ] 4.1 Sweep `library/`, `app/`, and `test/` (source and headers only — leave `tasks/`, `cmake/`, top-level `*.md` alone) and remove every `// PRD §…`, `// (PRD req. …)`, `// parser-perf task …`, `// task N.M`, and naked `§` reference. Use `rg "PRD\b|§|parser-perf|task \d+\.\d+" library app test` as the closing check; expected count is **0** (M6, PRD §4.4.1, §7).
  - [ ] 4.2 Shrink doc-comments on public types/methods to a one-line summary stating *what* the symbol is. Remove multi-paragraph rationale, references to other code, and historical context (PRD §4.4.2). Apply this especially to `library/include/loglib/json_parser.hpp`, `log_parser.hpp`, `log_table.hpp`, `streaming_log_sink.hpp`, `log_configuration.hpp`.
  - [ ] 4.3 Remove mid-function narrative comments that simply restate the next 3 lines (e.g. `// Step 2: splice the lines …` directly above `lines.insert(...)`) (PRD §4.4.3).
  - [ ] 4.4 Remove inline doc-comments on private helpers, lambdas, and nested types unless the intent is non-obvious (PRD §4.4.4).
  - [ ] 4.5 Remove fluff doc comments — `@brief Default constructor for X.`, `@param x The x.`, parameter descriptions that just repeat the parameter name, `@return The number of …` for an obviously-named getter (PRD §4.4.5).
  - [ ] 4.6 **Preserve** `@param` / `@return` / inline notes that document a contract a competent caller could otherwise violate (PRD §4.4.7), specifically:
    - Preconditions (e.g. `BackfillTimestampColumn`'s "Caller must ensure `column.type == LogConfiguration::Type::time`").
    - Lifetime / aliasing (e.g. "the returned `string_view` points into the mmap and outlives the `LogLine`").
    - Ownership transfer (e.g. `BufferingSink(std::unique_ptr<LogFile>)` — sink takes ownership).
    - Sorted-input invariants (e.g. `LogLine`'s pre-sorted-by-KeyId constructor contract).
    - In/out parameter semantics (e.g. `lastValid` "in/out cache").
    - Failure-mode contracts (e.g. "silently returns false; does not touch the line").
  - [ ] 4.7 Confirm `tasks/` is untouched. Search-confirm with `git status -- tasks/` showing only this file's changes (PRD §4.4.6).
  - [ ] 4.8 Check `test/lib/CMakeLists.txt` for any leftover `# PRD …` / `# task X.Y` references and remove them (currently 4 hits per `rg`).
  - [ ] 4.9 Re-run the orphan-check across the full source tree: `rg "PRD\b|§|parser-perf|task \d+\.\d+" library app test` returns 0 lines (M6).
  - [ ] 4.10 Run `ctest --preset local`. All tests pass; comment-only changes must not affect any assertion.
  - [ ] 4.11 Capture before/after `MB/s` for `[large]`, `[wide]`, `[stream_to_table]` (expected drift ~0 % — comments and doc-strings only).
  - [ ] 4.12 Commit to `feature/improve-performance-and-add-streaming` with the §4.11 numbers in the message.

- [ ] 5.0 Verify success metrics & sign-off (PRD §8, M1–M11)
  - [ ] 5.1 **M1** — `wc -l library/src/json_parser.cpp` ≤ **1,050** (down from 1,511; ≥ 30 % reduction per G2).
  - [ ] 5.2 **M2 / M3 / M4** — run `[large]`, `[wide]`, `[stream_to_table]` warm-up MB/s on the same machine that produced the predecessor baseline (`tasks/tasks-hl-inspired-parser-performance.md` task 6.1 numbers). Target floors: ≥ 985 MB/s on `[large]`, ≥ 1,151 MB/s on `[wide]`, ≥ 128 MB/s on `[stream_to_table]` (15 % regression budget per G6).
  - [ ] 5.3 **M5** — manual review of `library/include/loglib/parser_options.hpp` and `library/include/loglib/json_parser.hpp` confirms **0** tuning-knob fields on the public `ParserOptions` (only `stopToken` and `configuration`); every knob lives on `loglib::internal::AdvancedParserOptions` in `loglib/internal/parser_options.hpp`.
  - [ ] 5.4 **M6** — `rg "PRD\b|§|parser-perf|task \d+\.\d+" library app test` returns **0** lines.
  - [ ] 5.5 **M7** — `ctest --preset local` reports 95 + new `[mock_parser]` cases passing.
  - [ ] 5.6 **M8** — `testStreamingParityVsLegacy` passes (run by name).
  - [ ] 5.7 **M9** — `[cancellation]` median / p95 within ±20 % of current values (~3.4 ms / ~6.9 ms).
  - [ ] 5.8 **M10** — `test/lib/src/test_parser_pipeline.cpp` exists and exercises the shared toolkit through `KeyValueLineParser` (multi-batch parse, cancellation, error propagation, timestamp promotion).
  - [ ] 5.9 **M11** — `rg "JsonParserOptions|JsonParser::Options" library app test` returns **0** lines.
  - [ ] 5.10 If every metric M1–M11 is green, the PRD is done. Otherwise, file a follow-up note in the final commit's message identifying which metric failed and whether a fix sits inside or outside the 15 % regression floor (G6); a violation of any **hard** constraint (G0 or the 15 % floor) forces a revert per the PRD.
