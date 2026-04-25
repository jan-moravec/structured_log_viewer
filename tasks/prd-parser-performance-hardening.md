# PRD: Parser Performance Hardening

**Status:** Draft v2 (post-review revision)
**Owner:** TBD (junior dev welcome — every change is locally scoped)
**Source feature:** [tasks/prd-fast-streaming-json-parser.md](./prd-fast-streaming-json-parser.md)
**Companion task list:** to be generated after this PRD is approved

> **v2 changes (vs. the original draft):**
> - §4.2 route choice rewritten — "Route A: keep `tbb::concurrent_hash_map`, override `HashCompare`" is **not implementable** against oneTBB's API and has been removed. Route B (store `string_view` keys) and a new Route C (sharded `tsl::robin_map`) are now the two real choices. See §6.3 for the constraint analysis.
> - §4.3 line-numbering rewritten so Stage C does **O(1) per batch instead of O(N) per line** by stamping relative line numbers in Stage B and shifting them in Stage C.
> - **New §4.2a (P1)** — wire Stage B in-pipeline timestamp promotion as the source PRD originally specified. The current code casts the option away and runs the whole-file back-fill on the GUI thread; this is the largest perceived-UX win the v1 draft missed.
> - §4.4 simdjson API choice gated on the pinned `simdjson` version (`escaped_key()` if ≥ 3.6, SWAR otherwise).
> - §4.5 made explicit that `simdjson::pad` itself heap-allocates and must be eliminated, not just pre-sized.
> - §4.7 telemetry split into wall-clock vs CPU-time so per-worker sums on Stage B stop reading as ">100 % of wall clock"; §6.1 struct shape updated.
> - §4.8 expanded (and promoted from P4 to P3) to cover `LogTable::AppendBatch::RefreshColumnKeyIds` thrashing on the GUI thread and the `BufferingSink` Stage-C double-buffer that pays Stage C's coalescing tax for nothing in the legacy path.
> - §4.1.8 unit-test approach rewritten — `KeyIndex::Size()` invariance does not actually measure that the per-worker cache eliminated allocations; replaced with explicit `GetOrInsert`/`Find` call counters or a per-thread allocator wrapper.

---

## 1. Introduction / Overview

The streaming JSON parser delivered in `prd-fast-streaming-json-parser.md` shipped a complete `oneTBB` `parallel_pipeline`, a shared `KeyIndex`, a Qt streaming sink, and full GUI integration. End-to-end it parses ~110 MB/s on a 1 M-line / 181 MB fixture on an 8-core MSVC 2022 Release box, vs. ~70 MB/s for the legacy single-threaded parser. That is a 1.6× speedup — far less than the 5–6× a well-tuned multi-threaded pipeline should give on this hardware, and only ~10% better than the new code's own `[single_thread]` benchmark variant.

A code review of the hot path (see [Appendix A](#appendix-a-bottleneck-evidence) for line-level evidence) shows the **architecture is sound** but several **implementation gaps** prevent multi-threading from paying off:

- The per-worker key cache designed in `WorkerState::keyCache` is **never used** — every field key goes through the contended canonical `KeyIndex`.
- `KeyIndex::GetOrInsert` *and* `KeyIndex::Find` both allocate a `std::string` on **every** call (~5 M allocations per 1 M-line parse), even for keys that are already known.
- Stage A walks the entire input a redundant time just to count newlines, **serialising** ~180 MB of `memchr` work in front of the parallel stage.
- `ExtractFieldKey` runs a byte-at-a-time loop with a second `find('\\')` scan over the same bytes for every field key.
- Stage B's configuration-driven timestamp promotion was never wired (`(void)configuration;` in `ParseBatchBody`); the whole-file back-fill runs on the GUI thread instead of on the parallel workers, throttling perceived UI throughput on big files and making cancellation feel sluggish even after the parser thread stopped.
- `LogTable::AppendBatch` calls `RefreshColumnKeyIds()` on every batch (each `Find` inside also allocates), even on the steady-state batches where no new keys arrived. For wide configurations this is a meaningful per-batch tax on the GUI thread.
- `BufferingSink` re-buffers everything Stage C already coalesced, paying the `kStreamFlushInterval` clock checks for nothing in the legacy `Parse(path)` path.

This PRD scopes the work to **close those gaps and any further bottlenecks discovered during measurement**, and commits the project to a benchmark-driven workflow where every change is gated by a reproducible `MB/s` measurement.

This is **not** a redesign. It is a hardening pass on the existing pipeline to make it deliver the throughput its architecture was designed for.

## 2. Goals

The goals are ordered by priority. Lower-numbered goals take precedence when they conflict.

1. **G1 — Aspirational throughput improvement.** Pursue every change that does not regress steady-state throughput. Stop when diminishing returns kick in, *not* at a hard MB/s number. Measured on the existing `[.][benchmark][json_parser][large]` fixture (1 M lines / ~181 MB / 5 columns).
2. **G2 — Multi-thread efficiency.** Default-thread-count throughput must scale meaningfully versus the `[single_thread]` benchmark variant on the same fixture (target: **≥ 3× the single-thread number**, as a soft target — measured, not gated).
3. **G3 — No functional regression.** All 77 existing unit tests, the streaming parity test (`testStreamingParityVsLegacy`), and the cancellation-latency benchmark must continue to pass with no behavioural changes.
4. **G4 — No performance regression > ±3 % per change.** Every commit reports before/after `MB/s` on the 1 M fixture in its message. A regression beyond noise requires either a documented architectural justification (e.g. "removes serial bottleneck so future commit X scales") or revert. Benchmark methodology is fixed in [§7.4](#74-benchmark-methodology).
5. **G5 — Visibility.** Add per-stage timing telemetry to the benchmark output so reviewers can see *why* a change helped or didn't (Stage A %, Stage B %, Stage C %, sink % of wall time).

## 3. User Stories

- **As a Structured Log Viewer end-user**, when I open a multi-hundred-megabyte JSON log file, I want the rows to start arriving in under 100 ms and the whole file to finish in seconds, so I can investigate incidents without staring at a frozen UI.
- **As a developer of a downstream tool that links `loglib`**, I want the synchronous `JsonParser::Parse(path)` call to be as close to the theoretical max throughput of my hardware as possible, so my batch processing jobs scale.
- **As a maintainer of the parser**, I want the benchmark suite to tell me at a glance which stage is the bottleneck, so I can target optimisations precisely instead of guessing.
- **As a reviewer of a parser PR**, I want every commit message to carry a before/after `MB/s` measurement against a fixed methodology, so I can confidently approve performance work without re-running it locally.

## 4. Functional Requirements

The bottlenecks are listed in priority order; each is a separate, independently mergeable unit of work. **Each requirement is a separate PR** that ships with its own benchmark numbers (cf. [§7.4](#74-benchmark-methodology)).

### 4.1 P1 — Per-worker key cache (highest expected impact)

- **4.1.1** Wire in the `WorkerState::keyCache` field that already exists in [`json_parser.cpp` lines 614–620](#appendix-a-bottleneck-evidence). It must be consulted **before** `KeyIndex::GetOrInsert` is called.
- **4.1.2** The cache must support **heterogeneous lookup** — a `std::string_view` key must hash and compare against stored keys without materialising a `std::string`. A `tsl::robin_map<std::string, KeyId>` with a transparent hash + equality, or `absl::flat_hash_map<std::string, KeyId>` with `absl::Hash`, are both acceptable choices.
- **4.1.3** On a cache hit, no heap allocation may occur on the hot path.
- **4.1.4** On a cache miss the worker calls `KeyIndex::GetOrInsert(key)`, then writes the resulting `KeyId` into its local cache so subsequent lookups for the same key in the same worker are zero-allocation.
- **4.1.5** The cache is **per-worker**, held in `tbb::enumerable_thread_specific<WorkerState>`. It is never shared between workers.
- **4.1.6** The behavior must be controlled by `JsonParserOptions::useThreadLocalKeyCache` (which already exists; the `(void)useThreadLocalKeyCache;` cast in `ParseBatchBody` must be removed). When `false`, behaviour matches today.
- **4.1.7** The existing `[no_thread_local_cache]` benchmark variant is the **opt-out** measurement and must remain green.
- **4.1.8** Add a unit test that asserts the per-worker cache eats the canonical lookups. The "structurally inspect `KeyIndex::Size()` after the call" shortcut from the v1 draft only proves no new keys were *inserted* — it does not prove no `std::string` was *constructed* on a fast-path lookup (a regression that made the cache miss every line would still leave `Size()` invariant while allocating millions of strings). Pick one of:
    - (a) gate `std::atomic<size_t> KeyIndex::sGetOrInsertCallCount` (and a sibling `sFindCallCount`) behind a `LOGLIB_KEY_INDEX_INSTRUMENTATION` macro, compile-time off in shipped builds; assert the counter equals at most `workerCount × distinctKeys` after a 100-line stream of 5 fixed keys parsed with `useThreadLocalKeyCache = true`; or
    - (b) wrap the parse in a per-thread allocator-counting wrapper (counts `operator new`/`operator delete` calls into a `thread_local` counter) and assert per-line steady-state allocation count is `0` after warm-up.
- **Expected impact (informational, not gated):** −40 % to −60 % wall time on the 1 M fixture. This single requirement should account for the bulk of the multi-thread win.

### 4.2 P1 — Heterogeneous fast path in `KeyIndex::GetOrInsert` *and* `KeyIndex::Find`

- **4.2.1** Both the fast-path lookup in [`key_index.cpp` lines 89–100](#appendix-a-bottleneck-evidence) **and** `KeyIndex::Find` (see [Appendix A.11](#a11-keyindexfind-also-allocates)) must not materialise a `std::string` from the input `std::string_view`. The v1 draft only listed `GetOrInsert`; `Find` has the identical bug and is on the hot path of `LogTable::AppendBatch::RefreshColumnKeyIds` (cf. §4.8.2).
- **4.2.2** **Implementation route — pick one.** The v1 draft's "Route A: keep `tbb::concurrent_hash_map`, override `HashCompare` to take `std::string_view`" route is **not implementable** against oneTBB. `tbb::concurrent_hash_map::find` has no template overload accepting a heterogeneous query type, and its `HashCompare` callbacks are only ever invoked on the *stored* `Key` type, never on the query. See §6.3 for details. The two viable routes are:
    - **Route B (recommended for minimal-change paths):** keep `tbb::concurrent_hash_map` but switch the stored `Key` type to `std::string_view` whose bytes point into the `reverse` deque. The deque's `data()` pointer-stability contract (already required by `KeyOf`) guarantees the views never dangle. Lookup with the input `std::string_view` then trivially works because `Key == queryType`. The slow-path insert appends to the deque first, then inserts the resulting view into the map under the existing `reverseMutex`.
    - **Route C (recommended for cleanest-code paths):** drop `tbb::concurrent_hash_map` entirely and use a sharded `tsl::robin_map<std::string, KeyId>` with `is_transparent` hash + equality. ~16 shards, each behind a `std::shared_mutex`, give wait-free reader concurrency for the steady state and only contend on the (rare, post-§4.1) cold-path inserts. Heterogeneous lookup falls out of `tsl::robin_map`'s `is_transparent` support and needs no custom adapter.
- **4.2.3** The implementer must run a brief spike (~half a day) with both routes against the `[large]` benchmark and pick whichever gives the better wall-clock + better code-clarity trade-off. The chosen route, the spike numbers, and the rationale go into the commit message.
- **4.2.4** Concurrent insert behaviour, the `kInvalidKeyId` sentinel from `Find`, the dense-id contract of `GetOrInsert`, and the pointer-stability contract of `KeyOf` must all be preserved exactly. The existing `KeyIndex` thread-safety unit tests stay green.
- **4.2.5** The slow path (actual insert) may still allocate a `std::string` once for the `reverse` deque storage. That is the canonical owning copy.
- **4.2.6** This requirement is **independent of P1 §4.1** — both should be implemented because they cover different call sites:
    - §4.1 covers the **per-worker** parser hot path (where it eliminates 99 % of `KeyIndex::GetOrInsert` calls outright).
    - §4.2 covers the residual **first-time-per-worker** lookups, every `Find` call (especially the `LogTable::AppendBatch::RefreshColumnKeyIds` thrashing covered by §4.8.2) and any non-pipeline call sites (`LogLine::GetValue(const std::string&)`, `LogLine::SetValue(const std::string&, ...)`, etc.).
- **Expected impact (informational):** −5 to −10 % wall time on the parser side on top of §4.1, **plus** a separate (un-modelled) win on the GUI thread for wide-configuration streaming where every `AppendBatch` was paying ~`columnCount × keysPerColumn` `Find` allocations.

### 4.2a P1 — Wire Stage B in-pipeline timestamp promotion (GUI throughput fix)

This is a **new** requirement in v2 that the v1 draft missed. It is the single largest perceived-UX win on big files because it moves all `date::parse` work off the GUI thread.

- **4.2a.1** The original feature PRD's req. 4.2.21 specified that Stage B promotes `Type::time` columns inline using the `JsonParserOptions::configuration` snapshot. The current code casts the option away (`(void)configuration;` at [`json_parser.cpp:652`](#a7-stage-b-timestamp-promotion-never-wired)). As a result `LogTable::AppendBatch` runs `BackfillTimestampColumn` over every row on the GUI thread (cf. [`log_table.cpp:92`](#a8-marktimestampsparsed-lies-today)). For a 1 M-line file with one time column, that is ~1 M `date::parse` calls **serialised on the Qt event loop**, which both (a) caps perceived UI throughput at single-thread `date::parse` speed regardless of how fast Stage B is, and (b) blocks the event loop for the duration, making cancellation feel sluggish even though the parser thread already stopped.
- **4.2a.2** Wire the promotion as originally designed: at the start of `ParseStreaming`, pre-compute a `std::vector<TimeColumnSpec>` (KeyId-keyed, capturing the `parseFormats` per column from the snapshot configuration). In Stage B, after every successful `ParseLine`, walk the spec vector and promote any matching `LogValue` from `string_view`/`string` to `TimeStamp` via the existing `LogLine::SetValue(KeyId, LogValue)` overload. The per-worker `LastValidTimestampParse` cache from `log_processing.cpp` should live in `WorkerState` so the same fast-path short-circuit is used in-pipeline (see §6.5).
- **4.2a.3** Once 4.2a.2 lands, `LogTable::BeginStreaming` may legitimately call `LogData::MarkTimestampsParsed()` and `LogTable::AppendBatch`'s back-fill loop only fires for time columns auto-promoted **mid-stream** (i.e. KeyIds not in the Stage-B snapshot configuration). Today the unconditional `MarkTimestampsParsed()` call at [`log_table.cpp:47`](#a8-marktimestampsparsed-lies-today) is a load-bearing lie — Stage B did not parse any timestamps yet, but the flag claims it did. 4.2a.2 makes the flag truthful.
- **4.2a.4** **Cancellation contract:** Stage B's timestamp promotion runs inside `ParseBatchBody`, which is already gated by Stage A's `stop_token` poll between batches. Per-line `stop_token.stop_requested()` checks are **not** added — the existing `ntokens × batchSizeBytes` worst-case-wasted-work bound still holds.
- **4.2a.5** **Failure mode:** when no format in `column.parseFormats` matches the line's value, leave the value as `string_view`/`string` and let `LogTable::AppendBatch`'s mid-stream back-fill take a second pass on it. This matches the legacy `BackfillTimestampColumn` semantics exactly. Stage B does not push promotion failures into `parsed.errors`.
- **4.2a.6** **Test:** a new unit test in `test_json_parser.cpp` parses a 1 000-line fixture with a `Type::time` column and asserts every parsed `LogValue` for that column holds the `TimeStamp` alternative directly out of `Parse(path, opts)` — no whole-data back-fill required. The existing `testStreamingParityVsLegacy` must continue to pass byte-for-byte.
- **4.2a.7** **Benchmark:** a new GUI-thread micro-benchmark variant `[.][benchmark][json_parser][stream_to_table]` parses the 1 M fixture into a `LogTable` (mimicking the `BufferingSink::TakeData` → `LogTable::Update` flow) and reports `MB/s` end-to-end. This isolates the perceived-UI throughput from raw parser throughput and is the primary metric tracking 4.2a's success (cf. M1a in §8).
- **Expected impact:** moves all `date::parse` calls from the GUI thread to the parallel Stage B workers — on an 8-core box that's roughly an 8× speedup for the back-fill alone, and removes a multi-second event-loop block on big files. **This is the largest perceived-UX win in this PRD.**

### 4.3 P2 — Move the line-number accumulator from Stage A to Stage C

- **4.3.1** Remove the per-batch newline-counting loop at [`json_parser.cpp` lines 846–858](#appendix-a-bottleneck-evidence). Stage A no longer needs to know line numbers at all. It emits `PipelineBatch` tokens with `firstLineNumber = 0` (the field becomes purely advisory and may be retired in a follow-up).
- **4.3.2** Stage B already builds `parsed.localLineOffsets` and computes a `totalLineCount` per batch (`json_parser.cpp:746`). Have Stage B stamp **relative** line numbers (1-indexed within the batch) onto each `LogLine::FileReference()` while the lines are still in cache from `ParseLine`. The existing per-line `SetLineNumber` setter call is unchanged — only the value supplied changes from "absolute" to "relative".
- **4.3.3** Stage C (also `serial_in_order`) maintains a single running `nextLineNumber` cursor. After each in-order `ParsedPipelineBatch` arrives, Stage C does **two operations per batch** (not per line):
    - (a) walk `parsed.lines` and add `nextLineNumber - 1` to each line's `LogFileReference::mLineNumber` via a new `LogFileReference::ShiftLineNumber(size_t delta)` helper (no virtual dispatch, hot in cache because the lines were just produced);
    - (b) `nextLineNumber += parsed.totalLineCount`.
- **4.3.4** Add `LogFileReference::ShiftLineNumber(size_t delta)` alongside the existing `SetLineNumber`: a `noexcept` inline `mLineNumber += delta`. Documented as Stage-C-internal — callers outside the streaming pipeline should not use it.
- **4.3.5** **Alternative considered, rejected:** the v1 draft proposed "Stage C calls `SetLineNumber(absoluteLineNumber)` per line". That approach keeps the existing setter call but moves it onto the serial Stage C thread, which adds 1 M serial-thread setter calls + virtual indirection through `LogFileReference` per line on the 1 M fixture. Stamping relative numbers in Stage B (parallel, cache-hot) and applying a single `+=` per line in Stage C (serial but cache-hot, no virtual dispatch) is strictly cheaper. Recorded for the historical record only.
- **4.3.6** The `LogLine::FileReference()::GetLineNumber()` values observed by the sink (and asserted by `testStreamingParityVsLegacy`) must be byte-identical to today.
- **4.3.7** Empty lines must continue to consume one line number, matching the legacy parser's accounting (the existing `localLineOffsets.push_back` in Stage B already handles this; no behaviour change required).
- **Expected impact:** Stage A goes from O(file size) work back down to O(batch count). For 1 M lines this is one fewer 181 MB sequential `memchr` walk. Wall-time gain is modest in isolation (~5 %), but it **uncaps the parallel speedup** that §4.1 unlocks.

### 4.4 P2 — Replace `ExtractFieldKey` byte-loop with simdjson length-aware scan

- **4.4.1** The hand-rolled `while (*p != '"' || sawBackslash)` loop in [`json_parser.cpp` lines 79–91](#appendix-a-bottleneck-evidence) must be replaced with a length-known span. simdjson already located the closing quote during its SIMD pre-scan; we are needlessly re-doing it. **Choose based on the pinned `simdjson` version** (check the `FetchContent` `GIT_TAG` in `CMakeLists.txt` before drafting the patch and quote the version + chosen variant in the commit message):
    - **simdjson ≥ 3.6:** use `field.escaped_key()`, which returns a `string_view` over the raw key bytes (no surrounding quotes, no unescape) at zero cost. Five-line replacement.
    - **simdjson < 3.6:** vectorise the byte loop with SWAR — load 8 bytes at a time, mask for `'"'` and `'\\'` simultaneously, count trailing zeros to find the next quote/backslash, dispatch slow path only on a backslash hit. This is a smaller change than swapping the simdjson API surface and avoids a dependency bump.
- **4.4.2** The double-scan (the loop above followed by `inner.find('\\')` on the same bytes) must be collapsed into a single pass — either tracked during the first scan or eliminated by trusting simdjson's `key_equals_case_insensitive` / `unescaped_key()` cost characterisation.
- **4.4.3** The fast/slow path split must remain: a key with no escape sequence yields a `string_view` into the mmap; an escaped key falls back to `unescaped_key()` and an owned `std::string`.
- **4.4.4** The existing `[allocations]` benchmark must continue to report **≥ 99 % `string_view` fast-path fraction** for the fixture used.
- **Expected impact:** −5 to −10 % wall time, mostly from removing branch-predictor stalls on the byte loop.

### 4.5 P3 — Pre-size the per-worker padded-tail scratch buffer

- **4.5.1** `WorkerState::linePadded` should be reserved once at first use to `maxObservedLineSize + simdjson::SIMDJSON_PADDING + 64` so the `assign(line)` in [`json_parser.cpp` line 706](#appendix-a-bottleneck-evidence) never re-allocates.
- **4.5.2** `simdjson::pad(buffer)` itself heap-allocates a `simdjson::padded_string` and **must be eliminated**, not merely pre-sized. The per-worker `linePadded` is already padded to the right capacity at first use (4.5.1); Stage B writes the line bytes into it, zero-fills the trailing `SIMDJSON_PADDING` bytes manually (`std::memset(linePadded.data() + line.size(), 0, SIMDJSON_PADDING)`), then calls `worker.parser.iterate(linePadded.data(), line.size(), linePadded.size())` directly. The `simdjson::pad(...)` call is removed entirely from the hot path.
- **4.5.3** This path is only exercised for the last few lines of a file (those within `SIMDJSON_PADDING` bytes of EOF), so the wall-time impact is negligible — but it eliminates a per-line allocation that shows up disproportionately in flame graphs.

### 4.6 P3 — `InsertSorted` switches to `std::lower_bound` above N fields

- **4.6.1** When the per-line field count is small (≤ 8 measured on real fixtures), the linear back-scan in [`json_parser.cpp` lines 127–146](#appendix-a-bottleneck-evidence) is optimal due to branch prediction.
- **4.6.2** Above that threshold (chosen empirically using the wide fixture introduced in §4.7), switch to `std::lower_bound` for O(log N) placement.
- **4.6.3** The threshold may be made compile-time-fixed (e.g., `if constexpr (kThreshold == 8)`) or runtime-adaptive — implementer's choice. A simple `if (out.size() < 8)` is acceptable.
- **4.6.4** The duplicate-key "last write wins" semantics must be preserved.

### 4.7 P3 — Per-stage timing telemetry + new fixtures (depends on §6.1)

- **4.7.1** Add a `JsonParserOptions::collectStageTiming` opt-in flag. When set, the parser populates the `StageTimings` struct from §6.1 (capturing **per-worker CPU time per stage**, **wall-clock total**, **batch count per stage**, and the `effectiveThreads` count) on the sink (or returned from a debug callback).
- **4.7.2** The benchmark file `test/lib/src/benchmark_json.cpp` gains a per-stage breakdown emitted via `WARN` after the warm-up run for the 1 M fixture. Because Stage B is parallel, summing per-worker CPU time across `enumerable_thread_specific` workers can total many times the wall-clock duration — the printed format must distinguish the two so reviewers don't see ">100 %" and panic. Suggested format:
  `"Wall-clock: 1.55 s | Stage A CPU: 80 ms | Stage B CPU: 11.2 s (across 8 workers, 90 % utilisation = 11.2 / (8 × 1.55)) | Stage C CPU: 95 ms | Sink: 110 ms"`.
- **4.7.3** A new "wide" fixture (~30 columns: ~10 strings, ~10 numbers, ~5 booleans, ~5 nulls or arrays/objects) is added under the existing `GenerateRandomJsonLogs` helper or a new `GenerateWideJsonLogs(count, columnCount)` helper.
- **4.7.4** The wide fixture is used by a new `[.][benchmark][json_parser][wide]` test case at the same 1 M line count, with throughput, the per-stage breakdown described in 4.7.2, and `string_view` fast-path fraction reported.
- **4.7.5** No-regression checks (cf. [§7.4](#74-benchmark-methodology)) for changes after §4.7 lands must include both the original `[large]` and the new `[wide]` fixture numbers, plus the new `[stream_to_table]` number from §4.2a.7.
- **4.7.6** **Prerequisite for the `[wide]` fixture to be meaningful:** replace `LogConfigurationManager::IsKeyInAnyColumn`'s O(M·K) linear scan ([`log_configuration.cpp` lines 12–23](#a9-iskeyinanycolumn-quadratic)) with an internal `std::unordered_set<std::string>` cache of every key currently mentioned by any column. The cache is invalidated whenever `Update` / `AppendKeys` mutates the columns and rebuilt lazily on the next query. Without this fix, the `[wide]` benchmark on a 30-column configuration charges ~30 × 30 = 900 string compares per inserted key on the GUI thread, which would dominate the very metric this PRD is trying to surface.

### 4.8 P3 — Stage C / sink-consumer hot-path review

This requirement was P4 in v1 and covered only the `BufferingSink` Stage-C coalescing question. v2 promotes it to P3 and folds in two new bottlenecks the v1 draft missed: `LogTable::AppendBatch::RefreshColumnKeyIds` thrashing on the GUI thread, and the `BufferingSink` double-buffer that pays Stage C's coalescing tax for nothing.

- **4.8.1** Profile the in-process `BufferingSink::OnBatch` path **and** the streaming `QtStreamingLogSink::OnBatch` → `LogTable::AppendBatch` path with the wide fixture once §4.1 + §4.3 land. Capture both Stage C wall-clock % *and* `LogTable::AppendBatch` wall-clock % (the latter is on the GUI thread for the streaming sink, on Stage C for the buffered sink).
- **4.8.2** **`LogTable::AppendBatch::RefreshColumnKeyIds` thrashing.** [`log_table.cpp:84`](#a10-refreshcolumnkeyids-unconditional) calls `RefreshColumnKeyIds()` unconditionally on every batch. The function walks every column × every key, calling `mData.Keys().Find(key)` (allocation-heavy until §4.2 lands). Fix: only refresh when `!batch.newKeys.empty()`. Even better: incrementally patch only the inner vectors for columns whose keys appear in `batch.newKeys`, leaving the rest of the cache untouched. For a 100-column streaming parse with 1 000 batches and zero new keys after batch 1, this saves ~99 000 redundant `Find` calls on the GUI thread.
- **4.8.3** **`BufferingSink` Stage-C double-buffering.** In the legacy `Parse(path)` API, Stage C accumulates into `pending` (with `kStreamFlushLines = 1000` thresholds + per-batch `steady_clock::now()` calls), then `BufferingSink::OnBatch` immediately re-moves into `mLines` / `mLineOffsets` / `mErrors`. Stage C's coalescing is wasted work for the buffered case. Fix: add `virtual bool StreamingLogSink::PrefersUncoalesced() const { return false; }`; `BufferingSink` overrides to `true`; Stage C bypasses the `pending` accumulator and forwards each `ParsedPipelineBatch` directly to `OnBatch`. The streaming `QtStreamingLogSink` keeps the default (`false`) so the GUI still gets the 50 ms / 1 000-line coalescing.
- **4.8.4** **Stage C parallelisation for `BufferingSink`** — *only* if 4.8.2 + 4.8.3 leave Stage C > 15 % of wall time on the `[wide]` fixture. The v1 draft floated "let `BufferingSink` accept out-of-order batches that it sorts on `TakeData()`". Honest scoping note: this is **not** a tweak — it requires Stage C to become `parallel`, which means the `pending` aggregator, the `prevKeyCount` counter and `lastFlush` all become per-thread. Effectively Stage B pushes to a `tbb::concurrent_vector<ParsedPipelineBatch>` keyed by `batchIndex` and `BufferingSink::TakeData()` does the sort + concat as a post-pipeline pass. Treat as a small redesign and gate on a measured > 15 % Stage C cost; otherwise skip.
- **4.8.5** Any change here must keep `JsonParser::Parse(path)` byte-identical to the streaming path on the existing `testStreamingParityVsLegacy`.
- **4.8.6** This requirement may be partially skipped if both 4.8.2's `Find`-call delta and Stage C's wall-time % are < 5 % after §4.1 + §4.2 + §4.3 land — but record the measurement either way.

### 4.9 P5 — Forward-looking explorations (ship only if §4.1 + §4.3 saturate)

These are exploratory and only chase if the post-P1+P2 throughput plateaus below the hardware's plausible ceiling. **Each requires its own design note** appended to this PRD before implementation.

- **4.9.1** **Parallel Stage A using SIMD newline scan.** Replace the serial cursor with a parallel pre-scan that locates all newlines in the mmap (e.g. `simdjson::find_byte` or AVX2 `_mm256_cmpeq_epi8`) into a `std::vector<size_t>`, then partitions that vector into batches that workers pull from. This decouples Stage A from being a single-threaded cursor.
- **4.9.2** **`madvise(WILLNEED)` / `PrefetchVirtualMemory` per batch.** The current `HintSequential` is one-shot at file open; a per-batch prefetch ahead of the worker who will consume it could hide page-fault latency.
- **4.9.3** **`fast_float` for number parsing.** simdjson uses `fast_float` internally already, so this is unlikely to help; verify before discarding.
- **4.9.4** **Lock-free `KeyIndex` (e.g., concurrent open-addressing with linear probing + shared atomic counter).** Only worthwhile if §4.1 leaves > 5 % wall time in `GetOrInsert`.

## 5. Non-Goals (Out of Scope)

- **N1 — No public API changes.** `JsonParser::Parse(path)`, `JsonParser::ParseStreaming`, `LogTable`, `LogModel`, and the existing `JsonParserOptions` fields keep their current signatures and semantics. New `JsonParserOptions` flags are additive only.
- **N2 — No JSON library swap.** simdjson stays. Switching to `yyjson` / `RapidJSON` / a custom SIMD parser is explicitly out of scope.
- **N3 — No concurrency model swap.** `tbb::parallel_pipeline` stays. We will not migrate to `tbb::flow_graph`, raw thread pools, `std::execution`, or `coroutines`.
- **N4 — No file I/O model swap.** `mio::mmap_source` stays. `io_uring`, async I/O, and unbuffered `read()` paths are out of scope.
- **N5 — No `LogValue` representation change.** The `std::variant<...>` design is preserved; we will not collapse to a single `string_view` payload, and we will not introduce columnar per-type buffers in `LogTable` as part of this PRD.
- **N6 — No GUI redesign.** `MainWindow` streaming wiring stays as-is; only Qt sink batching internals are fair game (under §2D scope).
- **N7 — No new features.** Cancellation contract, error reporting, timestamp back-fill — all stay behaviourally identical. This PRD is **performance-only**.
- **N8 — No tunable knobs exposed to end users.** All new options live in `JsonParserOptions` for downstream callers; the GUI keeps using defaults.
- **N9 — No platform-specific intrinsics in the public path** beyond what `simdjson` and `mio` already pull in. Hand-rolled AVX2 intrinsics in §4.9.1 must be gated behind runtime CPU feature detection if added.

## 6. Design Considerations

### 6.1 Telemetry surface

The per-stage timing required by G5 / §4.7 needs a minimal data structure. Stage B is the only parallel stage, so per-stage cost must be reported as **per-worker CPU sums** alongside the **wall-clock total**, otherwise an 8-core run will show "Stage B: 800 % of wall clock" and confuse every reader. The v1 draft's single `stageBTotal` field collapsed both numbers and made multi-thread results unreadable.

```cpp
struct StageTimings
{
    /// Wall-clock duration of the entire ParseStreaming call (start to OnFinished).
    /// The denominator for any "% of wall clock" derivation in the printer.
    std::chrono::nanoseconds wallClockTotal{0};

    /// Sum of per-worker CPU time spent in each stage. Stages A and C are
    /// serial_in_order, so their *CpuTotal equals their wall-clock contribution.
    /// Stage B is parallel; stageBCpuTotal can total up to
    /// (effectiveThreads × wallClockTotal) on a perfectly-saturated parse.
    std::chrono::nanoseconds stageACpuTotal{0};
    std::chrono::nanoseconds stageBCpuTotal{0};
    std::chrono::nanoseconds stageCCpuTotal{0};

    /// Sink-side wall-clock cost. For the BufferingSink path this is folded into
    /// stageCCpuTotal; for the streaming sink it captures the queued-connection
    /// hop cost that lands on the GUI thread.
    std::chrono::nanoseconds sinkTotal{0};

    /// effectiveThreads is needed by the printer to derive Stage B utilisation
    /// = stageBCpuTotal / (effectiveThreads × wallClockTotal). Reported separately
    /// rather than re-derived so a downstream consumer that did not invoke
    /// ParseStreaming itself can still interpret the numbers.
    unsigned int effectiveThreads = 1;

    size_t stageABatches = 0;
    size_t stageBBatches = 0;
    size_t stageCBatches = 0;
};
```

The benchmark printer (cf. §4.7.2) is responsible for deriving "Stage B utilisation %" from `stageBCpuTotal / (effectiveThreads × wallClockTotal)` and rendering both the absolute CPU time and the utilisation. Stage A and Stage C percentages are derived against `wallClockTotal` directly because they are serial.

Exposed either as:
- A debug callback on `JsonParserOptions` (`std::function<void(const StageTimings&)> onFinishedTimings`), or
- A `mutable StageTimings* timings` pointer in `JsonParserOptions` populated before the call returns.

The pointer approach has zero allocation cost when unused (the parser only writes if `timings != nullptr`). Implementer's choice.

### 6.2 Per-worker cache type

`tsl::robin_map<std::string, KeyId>` with a transparent hash is the easiest drop-in:
- Header-only (matches §7.1 dependency policy).
- Heterogeneous lookup via `find(std::string_view)` is supported when `Hash::is_transparent` and `KeyEqual::is_transparent` are defined.
- Fast for the small (~5–50 entries per worker) cache size we expect.

`absl::flat_hash_map` is a fine alternative if abseil ends up pulled in for other reasons.

`std::unordered_map` works but is roughly 2× slower for the same workload — only acceptable as a placeholder commit.

### 6.3 Heterogeneous lookup in `KeyIndex` — why Route A is dead

The v1 draft listed two implementation routes for §4.2; only one of them is actually viable against oneTBB. v2 records the constraint here so a future implementer doesn't waste a day rediscovering it.

**Why "Route A — keep `tbb::concurrent_hash_map`, change the `HashCompare`" is not implementable.** `tbb::concurrent_hash_map::find(accessor&, const Key&)` has no template overload accepting a heterogeneous query type — there is no `is_transparent`-style hook in oneTBB's API. The `HashCompare`'s `hash(const Key&)` and `equal(const Key&, const Key&)` callbacks are only ever invoked on the *stored* `Key` type during the bucket scan, never on the query. To pass a `std::string_view` to `find` you would have to wrap it in a `Key`-shaped adapter — at which point you're paying the allocation Route A was trying to avoid, or you're forking the TBB header. Neither is "the smallest change" the v1 draft claimed.

**Route B (recommended for minimal-change paths).** Store `std::string_view` as the map's `Key` type; the views point into the `reverse` deque whose `data()` pointer-stability is already part of `KeyOf`'s contract. Lookup with the input `std::string_view` is now trivially a same-type call. Insert: append to the deque under `reverseMutex` (already required), then `forward.insert({deque.back(), id})` whose view is now stable.

**Route C (recommended for cleanest-code paths).** Replace `tbb::concurrent_hash_map` with a sharded `tsl::robin_map<std::string, KeyId>`, ~16 shards behind `std::shared_mutex`. `tsl::robin_map` supports `is_transparent` heterogeneous lookup natively, so the `string_view` fast path falls out for free. Per-shard `shared_lock` gives wait-free reader concurrency (the steady state after §4.1 lands). Cold-path inserts contend on at most one shard. ~80 LOC of pure C++17 vs the awkward `tbb::concurrent_hash_map` `HashCompare` interface; pulls in no new dependencies (`tsl::robin_map` is already on the allowed-headers list per §7.1).

Route choice is left to the §4.2 implementer based on a quick spike (cf. §9 Q1).

### 6.4 Stage A simplification

After §4.3 lands, Stage A is reduced to:

```cpp
auto stageA = [&](oneapi::tbb::flow_control &fc) -> PipelineBatch {
    if (cursor >= fileEnd || stopToken.stop_requested()) {
        fc.stop();
        return {};
    }
    const char *batchBegin = cursor;
    const char *target = std::min(cursor + batchSize, fileEnd);
    if (target < fileEnd) {
        const char *newline = static_cast<const char *>(
            memchr(target, '\n', static_cast<size_t>(fileEnd - target)));
        cursor = newline ? newline + 1 : fileEnd;
    } else {
        cursor = fileEnd;
    }
    return PipelineBatch{batchIndex++, batchBegin, cursor, fileEnd, /*firstLineNumber=*/0};
};
```

— a single bounded `memchr` per batch instead of one `memchr` per line in the batch.

### 6.5 Stage B in-pipeline timestamp promotion

After §4.2a lands, `WorkerState` grows to:

```cpp
struct WorkerState
{
    simdjson::ondemand::parser parser;
    std::string linePadded;
    JsonParser::ParseCache cache;
    PerWorkerKeyCache keyCache;                              // §4.1
    std::optional<LastValidTimestampParse> lastValidTimestamp; // §4.2a
};
```

`ParseStreaming` pre-resolves the `Type::time` columns in the snapshot configuration into a `std::vector<TimeColumnSpec>` (KeyId + parseFormats) **once** at pipeline start. Stage B's `ParseBatchBody` walks the spec vector after each `ParseLine` returns and promotes any matching value via `LogLine::SetValue(KeyId, TimeStamp)`. The `lastValidTimestamp` cache survives across batches on the same worker because workers are sticky (`enumerable_thread_specific`), so the `(key, format)` short-circuit from `log_processing.cpp` carries over from one batch to the next on a per-worker basis.

The legacy whole-file `BackfillTimestampColumn` in `log_table.cpp` still exists — it now fires only for time columns *auto-promoted mid-stream* (i.e. KeyIds that arrived in `batch.newKeys` and weren't in the Stage-B snapshot). The streaming-parity test must keep passing; the parsed timestamp values are byte-identical because both paths route through `date::parse` with the same format strings.

## 7. Technical Considerations

### 7.1 Dependencies

Per the user's pick `3C`:

- **Already in the build (preferred):** `oneTBB`, `simdjson`, `glaze`, `fmt`, `date`, `mio`, `Catch2`. No CMake change required.
- **Header-only additions allowed:** `tsl::robin_map` (preferred for §6.2), `xxhash`, `fast_float` if independent of simdjson.
- **Compiled libs allowed if they pull their weight:** `abseil` (only `absl/container/flat_hash_map.h` + `absl/hash/hash.h`), `folly::F14`. **Each new dep requires a one-paragraph justification in its commit message** comparing it against the header-only alternative.

CMake additions land via `FetchContent` to match the existing dependency convention (see `library/CMakeLists.txt` for the pattern).

### 7.2 Build modes covered

- `local` (MSVC 2022 Release, the user's primary box) — all benchmarks and tests must pass.
- `relwithdebinfo` and `debug` — must build cleanly and tests must pass; benchmark numbers are not gated for these.
- CI Linux jobs (`build-linux`, `build-linux-system-tbb`) — must build and tests must pass; benchmarks remain default-off in CI.

### 7.3 Compatibility constraints

- The streaming sink contract (`StreamingLogSink::Keys()` / `OnBatch` / `OnFinished`) is frozen.
- The `LogLine`, `LogValue`, `LogFile`, `LogTable` public APIs are frozen for this PRD (cf. N5).
- The legacy `JsonParser::Parse(path)` synchronous result must remain byte-identical to the streaming path on the existing parity test (`testStreamingParityVsLegacy`).

### 7.4 Benchmark methodology

Per the user's pick `4B` and `5D`, this is the reference protocol for every commit's "before/after" report:

1. **Hardware:** the developer's local machine (capture CPU / core-count / RAM in the commit message once per PR).
2. **Build:** `cmake --preset local && cmake --build --preset local --target tests` (Release, MSVC 2022 / clang on Linux).
3. **Pre-warm:** run the target benchmark **once** before measurement to warm the file-system cache and TBB worker pool.
4. **Measurement command:** `tests.exe "[.][benchmark][json_parser][large]"` and (after §4.7) `tests.exe "[.][benchmark][json_parser][wide]"`.
5. **Reported numbers:** the `WARN`-emitted `MB/s` and `lines/s` from the warm-up line plus the Catch2 mean from the 100-sample run.
6. **Acceptance gate (G4):** mean MB/s within ±3 % of the previous commit on the same machine, *or* a one-sentence architectural justification for the regression in the commit message. CI does not gate this — the developer reports honestly.
7. **Per-stage breakdown** (after §4.7 lands) must be quoted in the commit message for every change touching parser internals.

### 7.5 Risk register

| Risk | Mitigation |
|---|---|
| `tsl::robin_map` heterogeneous lookup interacts badly with the `WorkerState` move semantics | Add a unit test that constructs/moves `WorkerState` and verifies the cache survives. |
| Heterogeneous `KeyIndex` lookup races with concurrent insert | Stress test: 8 threads × 1 M `GetOrInsert` calls of overlapping random keys; assert `Size()` matches the unique key count and `KeyOf(GetOrInsert(k)) == k` always. |
| Removing Stage A's line counter breaks line numbers when an empty batch is in flight | Streaming parity test already covers this; add a targeted test that feeds a file with 100 consecutive empty lines mid-stream. |
| `simdjson` `raw_json_token()` length doesn't include the surrounding quotes the way we expect | Pin a unit test on `ExtractFieldKey` with quoted, escaped, and Unicode-escape keys. |
| TBB worker count / `ntokens` interactions cause regressions on smaller machines (e.g. 4-core CI runners) | CI Linux job already runs unit tests; add a targeted Catch2 case that runs a 10 k parse with `JsonParserOptions::threads = 1, 2, 4` and asserts non-decreasing speedup. |

## 8. Success Metrics

These metrics are **measured and reported**, not gated (G4 is the only gate). They live in the final commit message of each PR.

- **M1 — Throughput (1 M, default threads):** MB/s and lines/s on the `[large]` warm-up run. **Target trajectory:** ≥ 200 MB/s after §4.1; ≥ 250 MB/s after §4.1 + §4.3 + §4.4.
- **M1a — End-to-end stream-to-table throughput:** MB/s on the new `[stream_to_table]` benchmark (§4.2a.7). This is the perceived-UX number and the primary success metric for §4.2a. **Target trajectory:** approaches M1 within ~10 % once §4.2a lands; before §4.2a, M1a is bottlenecked by GUI-thread `date::parse` and is the dominant perceived-UX number on big files.
- **M2 — Multi-thread efficiency:** ratio of default-thread MB/s to single-thread MB/s on the same fixture. **Target trajectory:** ≥ 3× after §4.1.
- **M3 — Wide-fixture throughput:** MB/s and lines/s on the `[wide]` fixture introduced in §4.7. No specific target — establishes a second baseline.
- **M4 — Per-stage breakdown:** the §6.1 `StageTimings` struct, in particular **Stage B utilisation = stageBCpuTotal / (effectiveThreads × wallClockTotal)**. Stage B is the only stage that does real parallel work; the utilisation ratio measures how well we've saturated the parallel stage. **Target trajectory:** Stage B utilisation > 70 % once §4.1 + §4.3 land. Stage A wall-clock contribution should fall below 5 % once §4.3 lands. Stage C wall-clock contribution + GUI-thread `LogTable::AppendBatch` wall-clock contribution should together fall below 10 % once §4.8 lands.
- **M5 — Allocation footprint:** the existing `[allocations]` benchmark's "fast-path fraction" must stay ≥ 99 %, and "allocation upper bound" per line must not regress (currently ~1.0 / line).
- **M6 — Cancellation latency:** the existing `[cancellation]` benchmark's median + p95 must not regress (currently ~5.6 ms / 6.2 ms).
- **M7 — GUI-thread back-fill cost** (after §4.2a lands): time spent in `LogTable::AppendBatch::BackfillTimestampColumn` on the GUI thread per 100 k lines streamed, measured from the `[stream_to_table]` benchmark. **Target trajectory:** drops by ≥ 95 % once §4.2a lands (back-fill should fire only on auto-promoted mid-stream KeyIds, not on the snapshot configuration's time columns).

## 9. Open Questions

- **Q1 — `KeyIndex` rewrite route (§4.2).** Route B (`tbb::concurrent_hash_map<std::string_view, KeyId>` keyed against the `reverse` deque) vs Route C (sharded `tsl::robin_map` with `std::shared_mutex`). To be decided by the implementer of §4.2 based on a quick spike. Route B is the smallest diff; Route C drops a contended TBB primitive in favour of a header-only lock-light pattern. Either is acceptable — log the chosen route, the spike numbers, and the rationale in the commit message. (The v1 draft's Route A is dead, see §6.3.)
- **Q2 — Telemetry exposure surface.** Callback vs out-pointer (cf. §6.1). Junior dev's call.
- **Q3 — Wide-fixture column count.** §4.7 says "~30 columns"; final number to be tuned to mirror real-world structured logs the project's contributors actually have. If anyone can drop a sanitised real log under `test/fixtures/` it would replace the synthetic fixture.
- **Q4 — Should the per-stage timing API be public** (i.e. exposed on `JsonParser`) or internal-only (`#ifdef LOGLIB_INTERNAL_BENCHMARK`)? Default to internal until a downstream tool asks for it.
- **Q5 — Linux numbers.** Should we collect comparable Linux throughput numbers in CI for visibility (no gate)? If yes, this becomes a small follow-up CI task.
- **Q6 — Stage B timestamp promotion failure mode (§4.2a).** When Stage B fails to parse a timestamp on a given line (no format in `column.parseFormats` matches), the requirement (4.2a.5) currently says "leave the value as `string_view`/`string` and let `LogTable::AppendBatch`'s back-fill take a second pass". Confirm during implementation that this matches `BackfillTimestampColumn`'s observable behaviour exactly, including how it interacts with `LogConfigurationManager`'s mid-stream key-promotion heuristics.
- **Q7 — `LogConfigurationManager::IsKeyInAnyColumn` cache invalidation (§4.7.6).** The cache must be invalidated on every `Update` / `AppendKeys` mutation. Confirm whether any other mutation path (e.g. `RemoveColumn`, configuration import) exists and needs the same hook. Audit during the §4.7.6 PR.

---

## Appendix A — Bottleneck evidence

Direct citations to current code. All line numbers are at `feature/improve-performance-and-add-streaming` HEAD as of this PRD.

### A.1 Per-worker key cache exists but is not consulted

```614:620:library/src/json_parser.cpp
    struct WorkerState
    {
        simdjson::ondemand::parser parser;
        std::string linePadded;
        JsonParser::ParseCache cache;
        std::unordered_map<std::string, KeyId> keyCache;
    };
```

```727:729:library/src/json_parser.cpp
            (void)useThreadLocalKeyCache; // hooked in once a per-worker cache wins benchmarks
            (void)useParseCache;          // ParseCache is always-on for now; opt-out lands in 6.x
            auto values = JsonParser::ParseLine(objectValue, keys, worker.cache, sourceIsStable);
```

### A.2 `KeyIndex::GetOrInsert` always allocates a `std::string`, even on the fast path

```83:100:library/src/key_index.cpp
KeyId KeyIndex::GetOrInsert(std::string_view key)
{
    // Materialise a std::string for the TBB lookup. concurrent_hash_map
    // doesn't support heterogeneous lookup, but per-worker caches absorb this
    // cost in the parsing hot path, so this slower call site is only exercised
    // on cache misses (and from the per-batch dedup path in Stage B).
    const std::string keyOwned(key);

    // Fast path: the key already exists. concurrent_hash_map::find acquires a
    // shared lock on the matching bucket, so concurrent fast-path lookups do
    // not block each other.
    {
        Impl::ForwardMap::const_accessor acc;
        if (mImpl->forward.find(acc, keyOwned))
        {
            return acc->second;
        }
    }
```

### A.3 Stage A walks the entire input redundantly to count newlines

```846:858:library/src/json_parser.cpp
        size_t lineCount = 0;
        for (const char *p = batchBegin; p < cursor; ++p)
        {
            const char *nl = static_cast<const char *>(memchr(p, '\n', static_cast<size_t>(cursor - p)));
            if (nl == nullptr)
            {
                ++lineCount; // trailing line without '\n'
                break;
            }
            ++lineCount;
            p = nl;
        }
        nextLineNumber += lineCount;
```

### A.4 `ExtractFieldKey` byte-loop and double scan

```79:99:library/src/json_parser.cpp
        const char *p = rawData;
        bool sawBackslash = false;
        while (*p != '"' || sawBackslash)
        {
            if (*p == '\\')
            {
                sawBackslash = !sawBackslash;
            }
            else
            {
                sawBackslash = false;
            }
            ++p;
        }
        const std::string_view inner(rawData, static_cast<size_t>(p - rawData));
        if (inner.find('\\') == std::string_view::npos)
        {
            result.isView = true;
            result.view = inner;
            return result;
        }
```

### A.5 `InsertSorted` linear back-scan

```127:146:library/src/json_parser.cpp
void InsertSorted(std::vector<std::pair<KeyId, LogValue>> &out, KeyId id, LogValue value)
{
    auto it = out.end();
    while (it != out.begin())
    {
        auto prev = it - 1;
        if (prev->first < id)
        {
            break;
        }
        if (prev->first == id)
        {
            // Last write wins on duplicate keys, mirroring the previous LogMap insert behaviour.
            prev->second = std::move(value);
            return;
        }
        it = prev;
    }
    out.emplace(it, id, std::move(value));
}
```

### A.6 Padded-tail per-line allocation

```702:707:library/src/json_parser.cpp
            const size_t remaining = static_cast<size_t>(fileEnd - lineEnd);
            const bool sourceIsStable = remaining >= simdjson::SIMDJSON_PADDING;
            auto result = sourceIsStable
                              ? worker.parser.iterate(line.data(), line.size(), line.size() + remaining)
                              : worker.parser.iterate(simdjson::pad(worker.linePadded.assign(line)));
```

### A.7 Stage B timestamp promotion never wired

The original feature PRD's req. 4.2.21 specified that Stage B promotes `Type::time` columns inline using `JsonParserOptions::configuration`. The current code casts the parameter away:

```644:656:library/src/json_parser.cpp
    KeyIndex &keys,
    LogFile &logFile,
    const LogConfiguration *configuration,
    bool useThreadLocalKeyCache,
    bool useParseCache
)
{
    (void)configuration; // Stage B in-pipeline timestamp promotion lands in task 4.6/3.8.

    ParsedPipelineBatch parsed;
    parsed.batchIndex = batch.batchIndex;
    parsed.firstLineNumber = batch.firstLineNumber;
```

Result: every `Type::time` value reaches the sink as a `string_view`/`string`, and the GUI thread's `LogTable::AppendBatch` runs `BackfillTimestampColumn` over the entire `mData.Lines()` to convert them — see A.8.

### A.8 `MarkTimestampsParsed` lies today

Because Stage B does not actually promote timestamps (A.7), the unconditional `MarkTimestampsParsed()` call in `LogTable::BeginStreaming` is a load-bearing lie that suppresses the legacy `Update`-time pass without anyone having actually parsed the timestamps:

```40:55:library/src/log_table.cpp
    if (file)
    {
        std::vector<LogLine> noLines;
        LogData fresh(std::move(file), std::move(noLines), KeyIndex{});
        // Stage B already promotes timestamps inline; flag the LogData so the legacy
        // ParseTimestamps pass in Update is skipped if Update is ever called against this
        // table after streaming completes (PRD req. 4.2.21).
        fresh.MarkTimestampsParsed();
        mData = std::move(fresh);
    }
    else
    {
        // File-less initialisation path used by fixture tests that hand-craft batches.
        mData = LogData{};
        mData.MarkTimestampsParsed();
    }
```

`AppendBatch` then walks every column on every batch and back-fills any `Type::time` column outside the snapshot — but because A.7 means *no* time columns were promoted, this back-fill fires for every snapshot time column too, just with a different code path:

```86:95:library/src/log_table.cpp
    // Step 4: walk the configuration once more for time columns whose KeyId set is not a
    // subset of the Stage-B snapshot. Each such column is one that Stage B did not parse
    // (because the column did not exist in the snapshot configuration when the parse began),
    // so we back-fill it over *every* row currently in mData.Lines() — already-appended +
    // just-appended — exactly once. Append the back-filled column's keys into the snapshot
    // so subsequent batches see those keys as already-handled.
    const auto &columns = mConfiguration.Configuration().columns;
    std::optional<size_t> firstBackfilled;
    std::optional<size_t> lastBackfilled;
    for (size_t columnIndex = 0; columnIndex < columns.size(); ++columnIndex)
```

§4.2a fixes both: Stage B does the work, `MarkTimestampsParsed()` becomes truthful, and the back-fill loop fires only on auto-promoted mid-stream KeyIds.

### A.9 `IsKeyInAnyColumn` quadratic

`LogConfigurationManager::IsKeyInAnyColumn` is called once per inserted key during configuration auto-promotion. It walks every column × every key linearly:

```12:23:library/src/log_configuration.cpp
bool IsKeyInAnyColumn(const std::string &key, const std::vector<loglib::LogConfiguration::Column> &columns)
{
    for (const auto &column : columns)
    {
        if (std::find(column.keys.begin(), column.keys.end(), key) != column.keys.end())
        {
            return true;
        }
    }

    return false;
}
```

For a 30-column configuration with ~30 keys/column this is ~900 string compares per inserted key. §4.7.6 requires this to become O(1) amortised before the `[wide]` benchmark is meaningful.

### A.10 `RefreshColumnKeyIds` unconditional

`LogTable::AppendBatch` calls `RefreshColumnKeyIds()` on every batch unconditionally — even on the steady-state batches where `batch.newKeys.empty()`. The function walks every column × every key, calling `mData.Keys().Find(key)` (which is itself allocation-heavy until §4.2 lands; see A.11):

```82:84:library/src/log_table.cpp
    // Step 3: refresh the column → KeyId cache. Cheap when no new columns were appended;
    // necessary when they were so GetValue/GetFormattedValue see the new positions.
    RefreshColumnKeyIds();
```

The "Cheap when no new columns were appended" comment is wrong as long as `Find` allocates per call. §4.8.2 fixes this two ways: gate on `!batch.newKeys.empty()`, and once §4.2 lands, the per-call allocation goes away too.

### A.11 `KeyIndex::Find` also allocates

The same `std::string`-construction-on-every-call bug as A.2, in the lookup-only path used by `LogTable`, `LogConfigurationManager`, and any caller doing key-presence checks:

```134:143:library/src/key_index.cpp
KeyId KeyIndex::Find(std::string_view key) const
{
    const std::string keyOwned(key);
    Impl::ForwardMap::const_accessor acc;
    if (mImpl->forward.find(acc, keyOwned))
    {
        return acc->second;
    }
    return kInvalidKeyId;
}
```

§4.2 covers this alongside `GetOrInsert` — both allocations disappear under either Route B or Route C.

### A.12 `BufferingSink` Stage-C double-buffering

`BufferingSink::OnBatch` immediately re-moves everything Stage C just coalesced into the same kind of vectors Stage C used:

```20:48:library/src/buffering_sink.cpp
void BufferingSink::OnBatch(StreamedBatch batch)
{
    // Stage B/C built the batch's lines against this sink's mKeys (the parser
    // borrows it via Keys()) and Stage C already populated newKeys from the
    // same canonical index, so we can splice everything in without rebinding
    // back-pointers or remapping KeyIds. We deliberately ignore newKeys here:
    // the buffered final LogData re-derives the full key set from mKeys in
    // TakeData(), so any per-batch slice would be redundant for this consumer
    // (Qt-side sinks use newKeys for incremental column extension instead).
    if (!batch.lines.empty())
    {
        mLines.reserve(mLines.size() + batch.lines.size());
        std::move(batch.lines.begin(), batch.lines.end(), std::back_inserter(mLines));
    }
    if (!batch.localLineOffsets.empty())
    {
        mLineOffsets.reserve(mLineOffsets.size() + batch.localLineOffsets.size());
        std::move(
            batch.localLineOffsets.begin(),
            batch.localLineOffsets.end(),
            std::back_inserter(mLineOffsets)
        );
    }
    if (!batch.errors.empty())
    {
        mErrors.reserve(mErrors.size() + batch.errors.size());
        std::move(batch.errors.begin(), batch.errors.end(), std::back_inserter(mErrors));
    }
}
```

For the legacy `Parse(path)` path, Stage C's `kStreamFlushLines = 1000` thresholds and per-batch `steady_clock::now()` calls produce a coalesced `StreamedBatch` only for it to be immediately re-coalesced here. §4.8.3 introduces `StreamingLogSink::PrefersUncoalesced()` to skip Stage C's accumulator entirely for this consumer.

---

## Appendix B — Baseline measurements (pre-hardening)

Captured on the user's local machine (Windows, MSVC 2022, Release, Ninja, `local` preset), 2026-04-25:

| Benchmark | Mean | Throughput |
|---|---|---|
| `[large]` warm-up (1 M / 181 MB) | 1.55 s | **111.6 MB/s, 645 k lines/s** |
| `[large]` Catch2 mean of 100 | 3.16 s | ~57 MB/s |
| `[single_thread]` (10 k) | 18.78 ms | ≈ 96 MB/s |
| `[no_thread_local_cache]` (10 k) | 19.08 ms | ≈ 95 MB/s |
| `[no_parse_cache]` (10 k) | 17.98 ms | ≈ 100 MB/s |
| Default multi-thread (10 k) | 25.79 ms | ≈ 70 MB/s |
| `[allocations]` fast-path fraction | — | **99.9 %** |
| `[cancellation]` median / p95 / max | — | **5.57 ms / 6.23 ms / 6.23 ms** |
| Legacy parser (`a33a64e`) warm-up (1 M) | 2.46 s | 70.3 MB/s, 406 k lines/s |
| Legacy parser Catch2 mean of 100 (1 M) | 2.74 s | ~66 MB/s |

These are the numbers every PR in this PRD compares against. The `[single_thread]` variant being **faster** than the default multi-thread variant on the 10 k fixture is the single most damning data point — it is what this PRD exists to fix.
