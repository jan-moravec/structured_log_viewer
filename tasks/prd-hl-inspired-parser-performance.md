# PRD: hl-Inspired Parser Performance Improvements

**Status:** Draft v2 (rescoped 2026-04-26 after a code-review pass — see §A.1)
**Owner:** TBD (junior dev welcome — every remaining change is locally scoped)
**Source feature:** [tasks/prd-parser-performance-hardening.md](./prd-parser-performance-hardening.md) (fully shipped, see [tasks/tasks-parser-performance-hardening.md](./tasks-parser-performance-hardening.md))
**Inspiration:** [pamburus/hl](https://github.com/pamburus/hl) — a high-performance Rust log viewer that achieves ~9–14× single-thread parsing speed through memory-layout discipline and lazy decoding.
**Companion task list:** to be generated after this PRD is approved.

---

## 1. Introduction / Overview

The streaming JSON parser delivered by `prd-parser-performance-hardening.md` ships at **867.88 MB/s** on the `[large]` warm-up benchmark (1 M lines / ~181 MB / 5 columns), a **~7.8× speed-up** over the pre-hardening baseline of 111.6 MB/s. Stage B utilisation is now 74.65 % on `[large]` and 91.46 % on `[wide]`, Stage A wall-clock is < 0.5 %, the per-worker key cache is wired, and timestamps promote inside Stage B. The architecture is delivering close to its ceiling on synthetic fixtures.

A code review of the [`pamburus/hl`](https://github.com/pamburus/hl) Rust log viewer (which advertises ~2 GiB/s scan speed on real workloads) shows that **on top of a similar 3-stage pipeline architecture** it ships a handful of memory-layout and decoding tricks we have not adopted. After a second code-review pass against this codebase (see Appendix A.1) the candidate list narrows to two ideas with measurable, locally-scoped wins:

- The release profile uses **link-time optimisation** (`lto = true`, `codegen-units = 1`). Our CMake `Release` build does not enable `INTERPROCEDURAL_OPTIMIZATION` — easy win.
- Per-record fields are stored in a **hybrid stack/heap small-vector** (`heapopt::Vec<T, N>`), so a typical 5–15-field record is fully heap-allocation-free for its field vector. We allocate ~1 `std::vector<…>` per `LogLine` (≈ 1 M heap allocations on the `[large]` fixture). This is the single largest avoidable allocation on the parser's hot path.

Two further hl ideas are kept on the slate as **conditional follow-ups**, only ever shipped if a profile after the two changes above still implicates them:

- I/O scratch buffers are **recycled through a lock-free `crossbeam_queue::SegQueue`** (`SegmentBufFactory` / `BufFactory`). We allocate a fresh `ParsedPipelineBatch` per pipeline batch — but only ~174 of them per `[large]` parse, and the legacy `Parse(path)` flow's `BufferingSink::PrefersUncoalesced=true` fast path moves the inner vectors out wholesale, defeating any "preserve capacity for next acquire" benefit. Below the noise floor unless `[stream_to_table]` profiling says otherwise.
- The hash function for the canonical key index is `wyhash` (≈ 10 GB/s on small keys); we use `std::hash<std::string_view>` (≈ 1–2 GB/s). The per-worker key cache already absorbs ~99 % of canonical lookups so this is a cold-path-only change; chase only if `[no_thread_local_cache]` says so.

A fifth hl idea — **typed slots on the record for predefined fields** (`ts`, `level`, `message`, `logger`, `caller`) — is **out of scope** for this PRD. Our `LogConfiguration::Type` enum has only `any` and `time` (and `Type::time` is already promoted in Stage B per parent §4.2a), and our `LogValue` variant set is frozen by Decision 30 / parent PRD req. 4.1.6. Adding the level/message/logger/caller infrastructure is the kind of change that belongs with whatever filter/sort/UI work *consumes* it, not with parser throughput work. See Appendix A.

Item-by-item, none of the remaining items is the dominant bottleneck on its own. **Stacked, they are the difference between "fast" and hl-fast** — but only the small-vector change carries enough signal on its own to be worth committing to in advance. The rest are gated on measurement.

This is **not** a redesign. It is a second hardening pass that reuses the benchmark-driven workflow already established by the parent PRD's §7.4.

## 2. Goals

The goals are ordered by priority. Lower-numbered goals take precedence when they conflict.

1. **G1 — Aspirational throughput improvement.** Pursue every change that does not regress steady-state throughput on the `[large]`, `[wide]`, and `[stream_to_table]` benchmarks. Stop when diminishing returns kick in, *not* at a hard MB/s number.
2. **G2 — Allocation footprint.** Reduce per-line steady-state heap allocations from ~1 alloc/line (today) to **≤ 0.1 alloc/line** on the `[large]` fixture (post-warm-up, post-IPO). Achieved via §4.1's small-vector — that is the single load-bearing change for this metric; the conditional follow-ups in §4.2 / §4.3 add at most a small constant on top.
3. **G3 — No functional regression.** All existing unit tests, the streaming parity test (`testStreamingParityVsLegacy`), and the cancellation-latency benchmark must continue to pass with no behavioural changes.
4. **G4 — No performance regression > ±3 % per change** (inherited from the parent PRD's §7.4). Every commit reports before/after `MB/s` on `[large]`, `[wide]`, and `[stream_to_table]` in its message. A regression beyond noise requires either a documented architectural justification or revert.
5. **G5 — Visibility.** Every change re-runs the per-stage timing telemetry (`StageTimings` from the parent PRD §6.1) and quotes the **before/after Stage B utilisation** in its commit message, so reviewers can see whether the change actually moved the parallel-saturation needle.
6. **G6 — Conditional changes only ship if their own profile says so.** §4.2 and §4.3 are gated on a per-stage profile after §4.0 + §4.1 land: a conditional change only enters the queue if the headline benchmark it targets registers above its own gating threshold (called out in each section). Otherwise it is documented as "not pursued, below threshold" and closed without a commit, the same way parent PRD §4.9.x sub-tasks were closed.

## 3. User Stories

- **As a Structured Log Viewer end-user**, when I open a multi-hundred-megabyte JSON log file, I want the rows to start arriving in under 100 ms and the whole file to finish in seconds, so I can investigate incidents without staring at a frozen UI.
- **As a developer of a downstream tool that links `loglib`**, I want the synchronous `JsonParser::Parse(path)` call to be as close to the theoretical max throughput of my hardware as possible, so my batch processing jobs scale.
- **As a maintainer of the parser**, I want every shipped change to come with a per-stage timing report so I can see whether the next bottleneck is in the parser, the sink, or the GUI thread.
- **As a reviewer of a parser PR**, I want every commit message to carry a before/after `MB/s` measurement against a fixed methodology, so I can confidently approve performance work without re-running it locally.

## 4. Functional Requirements

The improvements are listed in **descending priority** with the conditional follow-ups grouped below the always-ship items. **Each requirement is a separate PR** that ships with its own benchmark numbers.

The "ease" rating is days-of-junior-dev work. The "promise" rating is the expected wall-time improvement on the `[large]` benchmark, where stated. Both are informational, not gated.

Recommended order of work:

1. §4.0 — IPO/LTO. Trivial, repo-wide, unconditional.
2. §4.1 — Small-vector for `LogLine` field storage. Biggest single per-line allocation on the hot path. Unconditional once §4.0 is in.
3. **Re-measure** with `[large]`, `[wide]`, `[stream_to_table]`, and `[allocations]` per §7.4. Then evaluate §4.2 and §4.3 below against their own gating thresholds.
4. §4.2 — Faster hash. **Conditional**: ship only if the post-§4.1 `[no_thread_local_cache]` benchmark or the GUI-thread `KeyIndex::Find` profile in `[wide]` shows the std-hash path above its noise floor.
5. §4.3 — `ParsedPipelineBatch` recycling. **Conditional**: ship only if a post-§4.1 profile of `[stream_to_table]` shows the per-batch outer-vector allocations registering above the ±3 % noise gate.
6. §4.4 — Dropped (see stub for rationale).
7. §4.5 — Forward-looking explorations (already gated on Stage B utilisation < 90 % on `[large]`).

### 4.0 P0 — Enable interprocedural optimisation (LTO/IPO) on Release builds

**Ease:** trivial (~1 hour). **Promise:** 5–15 % wall time, repo-wide.

The hl `Cargo.toml` sets `lto = true, codegen-units = 1, opt-level = 3`. Our `CMakePresets.json` `release` preset only sets `CMAKE_BUILD_TYPE=Release`, which on MSVC translates to `/O2` *without* `/GL` / `/LTCG`. Enabling `INTERPROCEDURAL_OPTIMIZATION` (CMake's portable LTO knob) gets the linker to inline across translation-unit boundaries for the templated hot path (simdjson value visitors, `tsl::robin_map` probing, the `LogValue` variant visitors).

Reference snippet to drop into the top-level `CMakeLists.txt` after the `project()` call (use the per-config form so `Debug` builds keep their fast iteration time):

```cmake
include(CheckIPOSupported)
check_ipo_supported(RESULT loglib_ipo_supported OUTPUT loglib_ipo_error LANGUAGES CXX)
if(loglib_ipo_supported)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
else()
    message(STATUS "IPO/LTO not supported: ${loglib_ipo_error}")
endif()
```

- **4.0.1** Add the snippet above to the top-level `CMakeLists.txt` immediately after the `project()` call.
- **4.0.2** Verify the link line on Windows (MSVC) actually picks up `/GL` (compiler flag) and `/LTCG` (linker flag). On clang/gcc, verify `-flto` appears.
- **4.0.3** Build clean Release on the local Windows MSVC machine; the link step is expected to take 2–3× longer (typical for IPO). Capture the link-time delta in the commit message so future reviewers know the cost.
- **4.0.4** Run `[large]`, `[wide]`, and `[stream_to_table]` warm-up + Catch2 mean. The new numbers become the baseline against which §4.1 (always) and §4.2 / §4.3 (conditional) are measured.
- **4.0.5** Verify all 80+ existing unit tests still pass (`ctest --preset release`).
- **4.0.6** **CI implication:** the Linux jobs (`build-linux`, `build-linux-system-tbb`) and the macOS job will also pick up IPO via `check_ipo_supported`. Verify each green CI job after the commit lands; if any platform fails the IPO check, the `else` branch above keeps the build running without IPO, and a one-line note is added to the PR description listing which platforms got IPO and which did not.
- **Expected impact:** 5–15 % wall time across the board, amplifies every subsequent measurement in this PRD.

### 4.1 P1 — Hybrid small-vector for `LogLine` field storage

**Ease:** medium (~3–5 days, mostly testing). **Promise:** 5–10 % wall time on `[large]`, 10–15 % on `[wide]`, removes ~1 alloc/line system-wide.

**Status:** **Always ship** (after §4.0). This is the single load-bearing change for G2 and the largest individual win in the PRD.

Today `LogLine::mValues` is `std::vector<std::pair<KeyId, LogValue>>` ([`log_line.hpp:248`](../library/include/loglib/log_line.hpp)). Every line allocates one heap block for its field backing; on `[large]` that is **1 M `operator new`/`operator delete` pairs per parse** (the `[allocations]` benchmark's "1.0 alloc/line" baseline). For 5-field records, the actual payload is ~5 × `sizeof(std::pair<KeyId, LogValue>)` ≈ 5 × 40 = **200 bytes**, less than a single cache line of inline storage on `LogLine` would carry.

hl uses `heapopt::Vec<T, N>` (a hybrid `heapless::Vec<T, N>` head + `Vec<T>` tail; see `crates/heapopt/src/vec.rs` in their tree) for `RawRecord::fields`, with `RECORD_EXTRA_CAPACITY = 24`. Records ≤ 24 fields are 100 % heap-free for the field vector.

This requirement makes additive changes to `LogLine` only; the `LogValue` variant set stays frozen per N5.

- **4.1.1** Introduce `library/include/loglib/small_vector.hpp` — a 60–80-line header-only `loglib::SmallVector<T, N>` with the `std::vector<T>`-equivalent subset we need: `push_back`, `emplace_back`, `clear`, `reserve`, `size`, `empty`, `data`, `begin`/`end`, `operator[]`, `back`, the move ctor / move-assign, the `std::span<const T>` conversion. The first `N` elements live in an aligned `std::byte[N * sizeof(T)]` buffer inside the struct; overflow goes to a heap `std::vector<T>` member. Lookup is a single branch on `size() <= N`. Keep the implementation small — this is a single internal type, not a general-purpose library replacement.
- **4.1.2** Pick **N = 16** as the default. Justification: the `[wide]` fixture (parent PRD §4.7.3) has ~30 columns, so `[wide]` lines push past N (heap fallback), while the `[large]` fixture (5 columns) and typical real-world structured logs (≤ 12 fields per Datadog 2024 logs survey or OpenTelemetry default schemas) fit inline. Re-tune by measuring on `[wide]` if Stage B utilisation drops noticeably below the 91 % baseline.
- **4.1.3** Replace `std::vector<std::pair<KeyId, LogValue>> mValues;` ([`log_line.hpp:248`](../library/include/loglib/log_line.hpp)) with `loglib::SmallVector<std::pair<KeyId, LogValue>, 16> mValues;`. Update the `LogLine(std::vector<std::pair<KeyId, LogValue>>, …)` constructor to move-construct into the small-vector via a slice loop; the public constructor signature is unchanged.
- **4.1.4** **Pre-sorted-by-KeyId contract** ([`log_line.hpp:122–129`](../library/include/loglib/log_line.hpp)) is preserved exactly — small-vector storage is contiguous either way, so the existing linear-scan `GetValue(KeyId)` works without modification.
- **4.1.5** **`IndexedValues()` returning `std::span<const std::pair<KeyId, LogValue>>`** still works because the small-vector exposes a contiguous backing through `data()` regardless of whether the storage is inline or heap.
- **4.1.6** **Move-construct semantics test.** `LogLine(LogLine&&) = default` ([`log_line.hpp:146`](../library/include/loglib/log_line.hpp)) currently moves a `std::vector<…>` (cheap, pointer-swap). With small-vector, moves from inline storage become element-wise — for 16 elements × 40 bytes that is ~640 bytes copied per move. The pipeline currently moves each `LogLine` ~3–4× end-to-end (Stage B `parsed.lines.push_back`, Stage C's `make_move_iterator` insert into `pending.lines`, `LogTable::AppendBatch`, `LogData::AppendBatch`), so the worst-case extra memcpy traffic per parse on `[large]` is ~16 × 40 × 4 × 1 M ≈ 2.5 GB of memcpy. **Confirm via benchmark** that this does not eat the alloc-savings: quote `[get_value_micro]` median, `[large]` MB/s, and `[wide]` MB/s in the commit. If a regression appears, drop to N = 8 (see 4.1.10) before declaring the requirement done.
- **4.1.7** **Stage C / sink-side moves** in `json_parser.cpp:1362–1385` and `LogTable::AppendBatch` move `LogLine`s into the model. Re-run the streaming parity test (`testStreamingParityVsLegacy`) to confirm byte-identical output.
- **4.1.8** **`[allocations]` benchmark drops to ≤ 0.1 alloc/line** for the `[large]` fixture (5-column lines fit inline, no heap allocation per `mValues`). Update the benchmark's printout to call out "inline-storage fraction" alongside the existing `string_view` fast-path fraction.
- **4.1.9** **`sizeof(LogLine)` invariants.** Pre-confirm `sizeof(LogValue) == 40` on the target build with a `static_assert` so a future variant addition that bumps the size cannot silently double the small-vector's footprint. Add a second `static_assert(sizeof(LogLine) <= 1024)` adjacent to it (matching §7.5's risk-register guardrail) so the structure cannot grow past the budget without an explicit code change.
- **4.1.10** **Memory footprint check.** `sizeof(LogLine)` grows by 16 × 40 = 640 bytes minus the existing 24-byte `std::vector` header ≈ **+616 bytes per `LogLine`**. For the `[large]` fixture (1 M lines) that is +616 MB resident at peak. **Reject the N = 16 default** if peak resident-set growth exceeds 2× the input file size on `[large]` and fall back to N = 8 (which fully inlines the 5-field `[large]` fixture but heap-spills the 12-field median real-world record). Decide empirically; document the final N in the commit message and update §6.1's table.
- **4.1.11** **Cold-path constructor** `LogLine(const LogMap&, KeyIndex&, …)` ([`log_line.hpp:141`](../library/include/loglib/log_line.hpp)) keeps working: it builds a temporary `std::vector`, sorts, then move-constructs into the small-vector via 4.1.3's converted constructor. No public API shift.
- **4.1.12** Run `[large]`, `[wide]`, `[stream_to_table]`, `[get_value_micro]`, `[allocations]`, and `[cancellation]`. Quote: before/after MB/s on the throughput benchmarks, the `[allocations]` per-line count (target: ≤ 0.1), the `[get_value_micro]` median (must not regress beyond ±3 %), peak resident-set on `[large]`.
- **Expected impact:** −5 % to −15 % wall time, primarily by removing 1 M `malloc`/`free` pairs from the `[large]` parse. The improvement on `[wide]` is bounded because 30-column lines spill to the heap path; expect ~5 % there.

### 4.2 P3 (conditional) — Faster hash for `tsl::robin_map` (xxh3 / wyhash)

**Ease:** very low (~half a day). **Promise:** 2–5 % wall time on `[no_thread_local_cache]` and `[wide]`.

**Status:** **Conditional** — only ship if, after §4.0 + §4.1 land, one of the following gates trips:

- `[no_thread_local_cache]` MB/s is more than 10 % below `[large]` MB/s (i.e. the canonical `KeyIndex` hash genuinely shows up when the per-worker absorber is bypassed), OR
- `[wide]`'s GUI-thread `KeyIndex::Find` time (instrument `LogTable::AppendBatch::RefreshColumnKeyIdsForKeys`) registers above 5 % of `[wide]` wall-clock.

Otherwise close the requirement as "below threshold, not pursued" — the per-worker key cache (parent PRD §4.1) absorbs ~99 % of canonical lookups on `[large]`, so the steady-state hot path almost never hits this hash.

When it does ship: every cold-path lookup, every `LogTable::AppendBatch::Find`, and every `LogConfigurationManager::IsKeyInAnyColumn` call still hits `std::hash<std::string_view>`. xxh3-64 (header-only, MIT-licensed via `xxHash`) is roughly 5–10× faster than the libstdc++/libc++/MSVC default for short keys.

- **4.2.1** Add `xxhash-cpp` (header-only) or `xxHash` C library to `cmake/FetchDependencies.cmake` via `FetchContent`. Prefer `xxhash-cpp` if it cleanly exposes `xxh::xxhash<64>(string_view)` as a `constexpr`-friendly callable. **Per parent PRD §7.1, the new dep needs a one-paragraph justification in the commit message** comparing it to keeping the `std::hash` fallback.
- **4.2.2** In `library/src/json_parser.cpp` (anonymous namespace), replace `TransparentStringHash::operator()`'s body with `xxh::xxhash<64>(sv.data(), sv.size(), /*seed=*/0)` (or the C-API equivalent). Keep the `is_transparent` typedef and the `std::string` / `const char*` / `string_view` overloads — only the hash body changes.
- **4.2.3** In `library/src/key_index.cpp`, swap **both** the `TransparentStringHash::operator()` body **and** `KeyIndex::Impl::ShardIndex(std::string_view)` (the static at lines ~123–126 today) in the same commit. The shard function and the per-shard map's hash must agree on the bucket distribution: if only one is swapped, every key collides into the same shard. This is a non-optional dependency, not a "if applicable" follow-on.
- **4.2.4** Run the existing `[key_index][stress]` test (parent PRD task 2.5) — must remain green. Add a new `[json_parser][hash_distribution]` test that inserts 10 k unique 5–20-byte keys and asserts the bucket distribution variance is within 2× the theoretical optimum (catches a hash-function regression that would silently cause clustering) and that **per-shard occupancy variance** is within the same factor (catches a `ShardIndex` regression).
- **4.2.5** Run `[large]`, `[wide]`, `[stream_to_table]`, `[no_thread_local_cache]`. Quote before/after MB/s on all four. The biggest delta is expected on `[no_thread_local_cache]` because that variant disables the per-worker absorber and exercises the canonical `KeyIndex` path on every line.
- **Expected impact (informational):** −2 % to −5 % on `[large]`, larger on `[no_thread_local_cache]` and `[wide]` (where wide configurations cause more GUI-thread `Find` calls).

### 4.3 P3 (conditional) — Recycle `ParsedPipelineBatch` storage via a token pool

**Ease:** low (~1–2 days). **Promise:** Below ~1 % wall time on the Buffering-Sink-driven benchmarks; possibly 1–3 % on `[stream_to_table]` only.

**Status:** **Conditional** — only ship if, after §4.0 + §4.1 land, the `[stream_to_table]` per-stage breakdown shows the per-batch outer-vector allocation cost (i.e. Stage B time spent inside `ParsedPipelineBatch`'s constructor / its inner `std::vector` allocations, separated from per-line `LogLine.mValues` work) above the ±3 % G4 noise gate. Otherwise close as "below threshold, not pursued".

The promise was rescoped down from the original "3–8 % on `[large]`" after measurement: every Stage B invocation builds a fresh `ParsedPipelineBatch` (`json_parser.cpp:761`) with three inner `std::vector`s, but at `kDefaultBatchSizeBytes = 1 MiB` and a 181 MB `[large]` fixture that is **~174 batches × 3 = ~520 outer-vector allocations per parse**, not the ~2100 originally cited. The per-line `mValues` allocation that §4.1 already eliminates is ~1 M allocs — three orders of magnitude bigger.

Worse, the `BufferingSink` flow (which drives `Parse(path)`, `[large]`, `[wide]`, and `[allocations]`) sets `PrefersUncoalesced = true` ([`buffering_sink.hpp:63`](../library/src/buffering_sink.hpp)). Stage C's uncoalesced branch ([`json_parser.cpp:1320–1323`](../library/src/json_parser.cpp)) does whole-vector `std::move` on `parsed.lines`, `parsed.localLineOffsets`, and `parsed.errors`, which is a pointer-swap that empties the source vector's capacity. By the time the batch returns to a hypothetical pool, all three inner vectors have zero capacity to preserve. Only `[stream_to_table]` (which goes through the coalesced `pending` accumulator on `QtStreamingLogSink`) sees any benefit — and even there, the coalescer's element-wise `make_move_iterator` insert leaves source capacity intact, but on the destination side the pool would compete against `pending.lines`'s already-warm capacity for any meaningful win.

If the gate trips and the requirement does ship: hl avoids the equivalent allocations entirely with `BufFactory` / `SegmentBufFactory` (lock-free recycling pool keyed on a `crossbeam_queue::SegQueue`). Reference shape:

```cpp
class ParsedBatchPool {
public:
    ParsedPipelineBatch Acquire();
    void Release(ParsedPipelineBatch batch);
private:
    oneapi::tbb::concurrent_queue<ParsedPipelineBatch> mPool;
};
```

- **4.3.1** Introduce `library/src/parsed_batch_pool.hpp` containing the small RAII-friendly pool above. `Release` calls `batch.lines.clear()`, `batch.localLineOffsets.clear()`, `batch.errors.clear()` (which **preserve capacity**) before pushing back. Acquire reuses the largest-capacity batch that fits. No new dep — `tbb::concurrent_queue` is already pulled in (PRIVATE in `library/CMakeLists.txt`).
- **4.3.2** Pass the pool by reference into `JsonParser::ParseStreaming`. Stage B's `stageB` lambda calls `auto parsed = pool.Acquire();` instead of constructing inline; populates as today; returns. Stage C calls `pool.Release(std::move(parsedAfterMoveOut))` *after* it has finished moving the inner vectors to the sink — note the order matters: the inner vectors are now empty/moved-from and must be `.clear()`-able without UB (`std::vector`'s moved-from state is "valid but unspecified"; reset via `vec = {};` if needed, then re-`reserve` on the next `Acquire`). For the `prefersUncoalesced` branch the pool is effectively a no-op (whole-vector moves already drained the capacity); document this in a code comment so the next reader does not expect a benefit there.
- **4.3.3** **Capacity hysteresis to bound memory.** A batch that grows to N lines never needs to drop back to a smaller capacity — but a one-off pathological `[wide]` line could cause `errors`'s capacity to balloon and stay ballooned. Cap each inner vector's preserved capacity at `4 × kStreamFlushLines` (≈ 4000) on `Release`; if the vector is larger, `vec = std::vector<T>{}; vec.reserve(4 × kStreamFlushLines);`.
- **4.3.4** **Lifetime contract.** The pool must outlive the `parallel_pipeline` call. Stage A doesn't touch it; Stages B and C share the pool via lambda capture. On pipeline cancellation (stop-token request), in-flight batches that Stage C has not seen yet are abandoned — they leak their vectors into the void. Tighten this in 4.3.5 below.
- **4.3.5** **Cancellation drain.** After `parallel_pipeline` returns, drain the pool back into a final destructor pass to ensure no vector is leaked. Add a Catch2 stress test: 100 parses-with-cancellation-after-batch-1; assert no growth in resident set (use `_CrtMemCheckpoint` on Windows, `mallinfo2()` on Linux, behind an `#ifdef LOGLIB_LEAK_CHECKS`).
- **4.3.6** Add a `[json_parser][batch_pool]` test that parses the same `[stream_to_table]` fixture twice in the same process and asserts the second parse's allocation count (via the existing `[allocations]` benchmark instrumentation) is **strictly less than** the first parse's count by at least 80 % — i.e. the second parse reuses pool storage. Use `[stream_to_table]` (coalesced sink) rather than `[large]` (uncoalesced) so the test exercises a path the pool can actually help.
- **4.3.7** Run `[large]`, `[wide]`, `[stream_to_table]`, `[cancellation]`. Quote before/after MB/s, the second-parse allocation delta from 4.3.6, and confirm `[cancellation]` median + p95 are unchanged.
- **Expected impact:** Below ~1 % wall time on the `BufferingSink`-driven benchmarks (`[large]`, `[wide]`, `[allocations]`); 1–3 % wall time on `[stream_to_table]` if the gate trips. UX win remains for downstream tools that link `loglib` and parse multiple files in a single process.

### 4.4 — Predefined-field typed slots on `LogLine` *(dropped)*

**Status: dropped.** Originally proposed as "P3 — typed slots on `LogLine` for `Type::level` / `Type::message` / `Type::logger` / `Type::caller`" mirroring hl's `Record { ts, level, message, logger, caller }` shape. The requirement does not match this codebase's data model and is left out of scope:

1. **`LogConfiguration::Type` only has `any` and `time`** ([`log_configuration.hpp:22–26`](../library/include/loglib/log_configuration.hpp)). The `level`, `message`, `logger`, `caller` types the requirement assumed do not exist; introducing them touches the configuration loader, the GUI's column-type editor, and downstream callers — none of which is "performance work".
2. **The only currently-existing predefined type, `Type::time`, is already promoted in Stage B** ([`json_parser.cpp:1021–1031`](../library/src/json_parser.cpp); parent PRD §4.2a). So on the as-written codebase the requirement adds zero new fast paths until step (1) lands.
3. **The reference slot shape requires storage types that `LogValue` does not have** (a level enum). Either `LogValue` would have to grow a new alternative — forbidden by N5 / Decision 30 / parent PRD req. 4.1.6 — or the slot would store an `int32_t` and convert back to a string at every read, defeating the GetValue micro-benchmark win.
4. **The bigger prize the requirement chased ("filter/sort reads `mLevel` directly in O(1)")** is filter/sort work, not parser work. It belongs in whatever PRD eventually owns the filter/sort engine and can decide on the configuration-type-set / `LogValue`-set widening as part of *that* design.

If a future PRD wants to revive this idea, the prerequisite is: either a separate PRD that widens `LogConfiguration::Type` (and updates the GUI / config loader / persistence) **or** a measurement that shows `Type::time` promotion wins more by sharing infrastructure with other types, on real-world fixtures with non-trivial level/message coverage.

### 4.5 P5 — Forward-looking explorations (ship only if §4.0–§4.3 leave Stage B utilisation < 90 % on `[large]`)

These are exploratory and only chase if the post-§4.1 (and possibly post-§4.2 / §4.3) throughput plateaus below the hardware's plausible ceiling. **Each requires its own design note** appended to this PRD before implementation.

- **4.5.1 Vendored memchr / `__builtin_memchr` in Stage A's chunk-end finder.** hl uses the `memchr` Rust crate, which ships hand-tuned AVX2 / NEON kernels. MSVC's `memchr` is okay but not always vectorised at small sizes. Worth a spike if Stage A wall-clock fraction creeps back above 5 %.
- **4.5.2 SIMD newline-counter in Stage C.** Today Stage C walks `parsed.lines` linearly. If the `[wide]` Stage C wall-clock fraction climbs back above 15 % (parent PRD §4.8.4), parallelising it via `tbb::concurrent_vector` post-pipeline is the redesign route. Same caveat the parent PRD already documented.
- **4.5.3 `mio` vs `Win32 PrefetchVirtualMemory`.** The parent PRD's §4.9.2 was cancelled because Stage B utilisation hit 74 % on `[large]`. Re-evaluate if SSDs and the `[stream_to_table]` flow expose page-fault stalls.

## 5. Non-Goals (Out of Scope)

- **N1 — No public API changes.** `JsonParser::Parse(path)`, `JsonParser::ParseStreaming`, `KeyIndex`, `LogTable`, `LogModel`, and the existing `JsonParserOptions` fields keep their current signatures and semantics (inherited from parent PRD §5).
- **N2 — No JSON library swap.** simdjson stays.
- **N3 — No concurrency model swap.** `tbb::parallel_pipeline` stays.
- **N4 — No file I/O model swap.** `mio::mmap_source` stays.
- **N5 — `LogValue` variant set is unchanged.** Per Q2 = B: additive changes to `LogLine`'s storage shape (e.g. small-vector backing) are allowed; adding new `LogValue` alternatives is not. The `std::variant` index contract ([`log_line.hpp:31–34`](../library/include/loglib/log_line.hpp)) is part of an on-disk-stable promise per parent PRD req. 4.1.6 / Decision 30. This is the constraint that puts §4.4 (predefined typed slots) out of scope.
- **N6 — No persistent on-disk parse cache.** Per Q3 = D, hl's cap'n proto block-hash index (its "10 GiB/s reindex" headline) is **not** in scope and **not** planned as a follow-up. The roadmap is parsing-speed-focused, not cache-feature-focused.
- **N7 — No GUI redesign.** Qt streaming sink batching internals are fair game (parent PRD §4.8); model/view layers are frozen.
- **N8 — No new features.** Cancellation contract, error reporting, timestamp back-fill semantics — all stay behaviourally identical.
- **N9 — No tunable knobs exposed to end users.** All new options live in `JsonParserOptions` for downstream callers; the GUI keeps using defaults.
- **N10 — No platform-specific intrinsics in the public path** beyond what `simdjson` and `mio` already pull in (inherited from parent PRD §5 N9). xxh3 (§4.2 if it ships) is portable C++17 with optional SIMD acceleration; that is fine.
- **N11 — Predefined-field typed slots (`Type::level`, `Type::message`, `Type::logger`, `Type::caller`) are out of scope.** See §4.4 stub for the rationale. Reviving this idea requires a separate PRD that owns the `LogConfiguration::Type` set and the GUI / config-loader / persistence consequences of widening it.

## 6. Design Considerations

### 6.1 Small-vector capacity (§4.1.2)

Real-world structured-log field counts are bimodal: ≤ 12 fields (typical: timestamp, level, msg, span, trace_id, service, env, host, latency_ms, status_code, request_id) or 30+ fields (Kubernetes pod metadata logs, GCP audit logs).

The `[large]` fixture sits at 5 fields, the `[wide]` fixture at 30. **N = 16** picks up the 12-field median fully inline and lets the 30-field tail spill to heap. **N = 8** fully inlines the 5-field `[large]` but heap-spills the 12-field median. The trade-off is `LogLine` size (per §4.1.10):

| N | `sizeof(LogLine)` (approx) | 1M-line peak resident | Inlined fraction (real-world median ≈ 12 fields) |
| --- | --- | --- | --- |
| 8 | ~360 B | +360 MB | ~50% |
| 16 | ~700 B | +700 MB | ~95% |
| 24 | ~1040 B | +1.0 GB | ~100% |

Given the `[large]` fixture is 181 MB on disk, anything over +700 MB resident-set growth feels excessive. **Default to N = 16, drop to N = 8 if 4.1.10's empirical check trips.**

### 6.2 Hash function selection (§4.2)

Only relevant if §4.2's gate trips. `xxh3-64` is the modern default for non-cryptographic hashing of small keys (1–32 bytes). Alternative candidates considered:

| Hash | Throughput (small keys) | Library shape | Verdict |
| --- | --- | --- | --- |
| `xxh3-64` | ~10 GB/s | `xxhash-cpp` header-only OR `xxHash` C library | **Pick.** Header-only. |
| `wyhash` | ~12 GB/s | Header-only, MIT | Acceptable alt; slightly less battle-tested than xxh3. |
| `ahash` | ~14 GB/s | Rust-only | Skip (no C++ binding). |
| `absl::Hash` | ~7 GB/s | abseil compiled lib | Skip — pulls in abseil for one feature. |
| `std::hash<string_view>` (status quo) | ~1–2 GB/s | std | Status quo. |

The §4.2 commit-message paragraph should cite the throughput numbers from the chosen library's own benchmarks (xxhash and wyhash both publish them), not synthesised ones.

`KeyIndex::Impl::ShardIndex` and the per-shard `tsl::robin_map`'s `TransparentStringHash` *must* swap together (see §4.2.3) — otherwise shard occupancy collapses onto a single shard. Both call sites are in `library/src/key_index.cpp`; the swap is a single commit.

### 6.3 Pool drainage on cancellation (§4.3.5)

Only relevant if §4.3's gate trips. `tbb::parallel_pipeline` stops dispatching new tokens when the stop-token fires, but tokens already in flight finish their current stage. Stage C may not see every batch. The pool's destructor must therefore **own** any unreturned batches' memory; this falls out naturally from `tbb::concurrent_queue`'s standard ownership semantics, but the test in §4.3.5 needs to confirm it.

## 7. Technical Considerations

### 7.1 Dependencies

Per parent PRD §7.1 and the user's pick `3C`:

- **Already in the build:** `oneTBB`, `simdjson`, `glaze`, `fmt`, `date`, `mio`, `Catch2`, `tsl::robin_map`. No CMake change required for §4.0, §4.1, §4.3.
- **New header-only addition (only if §4.2 ships):** `xxhash-cpp` or `xxHash` C library via `FetchContent`. **Each new dep requires a one-paragraph justification in its commit message** comparing it against the header-only alternative (parent PRD §7.1).
- **No compiled-lib additions planned.**

### 7.2 Build modes covered

- `release` (MSVC 2022, the user's primary box) — IPO **on** after §4.0, all benchmarks and tests must pass.
- `relwithdebinfo` and `debug` — must build cleanly and tests must pass; benchmark numbers are not gated. IPO is **on** for `relwithdebinfo`, **off** for `debug` (per §4.0.1).
- CI Linux jobs (`build-linux`, `build-linux-system-tbb`) — IPO conditionally on (depends on the toolchain's `check_ipo_supported` result); must build and tests must pass.

### 7.3 Compatibility constraints

- The streaming sink contract (`StreamingLogSink::Keys()` / `OnBatch` / `OnFinished` / `PrefersUncoalesced`) is frozen.
- The `LogLine`, `LogValue`, `LogFile`, `LogTable` public APIs are frozen — §4.1 changes `LogLine`'s storage (`mValues` becomes a small-vector) but every public method's signature and semantics stay the same. No public function signature changes.
- The legacy `JsonParser::Parse(path)` synchronous result must remain byte-identical to the streaming path on the existing parity test (`testStreamingParityVsLegacy`).

### 7.4 Benchmark methodology

Inherited verbatim from parent PRD §7.4. Reference protocol for every commit:

1. **Hardware:** the developer's local machine (capture CPU / core-count / RAM in the commit message once per PR).
2. **Build:** `cmake --preset release && cmake --build --preset release --target tests`.
3. **Pre-warm:** run the target benchmark **once** before measurement.
4. **Measurement command:** `tests.exe "[.][benchmark][json_parser][large]"`, `[wide]`, and `[stream_to_table]`.
5. **Reported numbers:** the `WARN`-emitted `MB/s` and `lines/s` from the warm-up + the Catch2 mean from the 100-sample run.
6. **Acceptance gate (G4):** mean MB/s within ±3 % of the previous commit on the same machine, *or* a one-sentence architectural justification for the regression in the commit message.
7. **Per-stage breakdown** (`StageTimings` from parent PRD §6.1) must be quoted in the commit message for every change touching parser internals. Stage B utilisation is the headline per-stage metric.
8. **Allocation footprint** must be quoted via the `[allocations]` benchmark for every change touching `LogLine`, `LogValue`, or pipeline batch storage (i.e. §4.1 and §4.3 if it ships).

### 7.5 Risk register

| Risk | Affects | Mitigation |
| --- | --- | --- |
| IPO/LTO blows up CI link times | §4.0 | Verify each platform's link time before merge; if any platform regresses unacceptably, gate IPO per-platform via `if(WIN32)` etc. |
| Small-vector blows resident-set on huge files | §4.1 | §4.1.10's resident-set check rejects N = 16 if growth > 2× input size on `[large]`; fall back to N = 8. |
| Small-vector move cost eats the alloc-savings | §4.1 | §4.1.6 mandates a `[get_value_micro]` / `[large]` / `[wide]` benchmark spike before declaring done; if a regression appears, drop to N = 8. |
| `LogLine` size growth breaks an undocumented sizeof assumption in test code | §4.1 | §4.1.9 adds `static_assert(sizeof(LogLine) <= 1024, …)` adjacent to a `static_assert(sizeof(LogValue) == 40, …)` so any future structure growth fails the build immediately. |
| `xxh3-64` adopts an undocumented binary format and changes between versions | §4.2 (if shipped) | Pin the dep version in `cmake/FetchDependencies.cmake` (`GIT_TAG vN.N.N`); never bump without re-running the bucket-distribution test (§4.2.4). |
| Hash swap forgets the `KeyIndex::Impl::ShardIndex` callsite, collapsing all keys onto one shard | §4.2 (if shipped) | §4.2.3 makes the lockstep swap a single-commit requirement; §4.2.4's `[hash_distribution]` test asserts both bucket and per-shard occupancy variance. |
| Pool capacity hysteresis pessimises memory on a bursty `[wide]` parse | §4.3 (if shipped) | §4.3.3 caps preserved capacity at `4 × kStreamFlushLines`; verify on a 2× wide fixture (60 columns). |
| Pool ships but moves no needle on the only path it can help (`[stream_to_table]`) | §4.3 (if shipped) | The §4.3 gate forces a measurement before the work even starts; if the gate is borderline, prefer "below threshold, not pursued" over an inconclusive 1 % win. |

## 8. Success Metrics

Inherited semantics from parent PRD §8 — measured and reported, not gated.

- **M1 — `[large]` throughput.** MB/s on the 1 M warm-up run. **Trajectory:** ≥ 950 MB/s after §4.0; aspirational ≥ 1 GB/s after §4.0 + §4.1. §4.2 / §4.3 are conditional and not factored into the trajectory.
- **M2 — `[stream_to_table]` throughput.** Should track M1 within ~10 % across the PRD. The `[stream_to_table]` number is the relevant gate for whether §4.3 is worth pursuing (per §4.3's status block).
- **M3 — `[wide]` throughput.** No specific target — establishes a second baseline. Stage B utilisation on `[wide]` should remain ≥ 91 % (today's baseline).
- **M4 — Stage B utilisation on `[large]`.** **Trajectory:** ≥ 80 % after §4.1. Today's baseline is 74.65 %. The §4.5 forward-looking gate (Stage B utilisation < 90 %) keys off this.
- **M5 — Allocation footprint per line on `[large]`.** **Trajectory:** ≤ 0.1 alloc/line after §4.1 (§4.1.8). Today's baseline is ~1.0 alloc/line. §4.3 might push this slightly lower if it ships, but §4.1 is the load-bearing change for the gate.
- **M6 — Cancellation latency.** The existing `[cancellation]` benchmark's median + p95 must not regress (currently ~5.6 ms / 6.2 ms).
- **M7 — `LogLine` size.** Must stay ≤ 1024 bytes per the §7.5 static_assert. Today's baseline is ~96 bytes; after §4.1 with N = 16 the projection is ~700 bytes.
- **M8 — IPO link time delta.** Reported once after §4.0; informational. Aim to keep the link step under 30 s on the local box; if it exceeds 60 s, gate IPO to `relwithdebinfo` only.

## 9. Open Questions

- **Q1 — Small-vector N (§4.1.2).** Default N = 16 unless the §4.1.10 resident-set check trips, then drop to N = 8. The decision is empirical, not architectural; document the final number and the measurement that drove it in the §4.1 commit.
- **Q2 — Hash library choice (only if §4.2 ships).** `xxhash-cpp` (modern C++ wrapper, less battle-tested) vs `xxHash` C library (the upstream, header-only with `XXH_INLINE_ALL`). Junior dev's call; both are acceptable. Document the choice + spike numbers in the §4.2 commit.
- **Q3 — Should `[stream_to_table]` become the headline benchmark in this PRD's commit messages** (instead of `[large]`)? Today commits quote `[large]` first because that is parent-PRD parity. After §4.1 lands, the `[stream_to_table]` number is the user-visible UX number on big files; consider promoting it.
- **Q4 — Linux numbers in CI.** Same as parent PRD Q5 — no gate, just visibility. If green, copy the parent PRD's posture: collect Linux MB/s in CI as informational, not gated.

---

## Appendix A — hl tricks deliberately not adopted

These were considered and explicitly excluded; recording here so a future reviewer doesn't waste time reproposing them.

- **Predefined-field typed slots (`Type::level` / `Type::message` / `Type::logger` / `Type::caller`) on `LogLine`.** Was the original v1 §4.4. Dropped after the v2 rescope: the codebase's `LogConfiguration::Type` has only `any` and `time`, the only existing predefined type (`Type::time`) is already promoted in Stage B (parent PRD §4.2a), and the requirement's reference slot shape needs a level enum that `LogValue` does not have and N5 forbids adding. The big claimed win ("filter/sort reads `mLevel` directly in O(1)") is filter/sort work, not parser work; that is where the configuration-type-set widening should be designed.
- **Lazy `RawValue::Number(string_view)` / lazy unescape.** Excluded by Q2 = B: changing `LogValue`'s variant set is forbidden under this PRD's scope. May revisit if a future PRD widens N5.
- **Persistent on-disk parse cache (cap'n proto block index).** Excluded by Q3 = D. Out of roadmap.
- **`memchr2_iter` JSON delimiter for pretty-printed input.** We don't support multi-line JSON streams; the `\n`-delimited fast path is sufficient.
- **Hand-rolled SPMC pipeline replacing `tbb::parallel_pipeline`.** The pipeline-spike numbers in `pr-description-fast-streaming-json-parser.md` show oneTBB matches a hand-rolled SPMC within noise. No measured gain to chase.
- **`logos`-style query DSL parser.** Orthogonal to JSON parsing.

### A.1 v2 rescope record (2026-04-26)

The original v1 of this PRD listed five always-ship requirements (§4.0 IPO, §4.1 hash, §4.2 pool, §4.3 small-vector, §4.4 typed slots) ordered by ease × promise. A code-review pass against the actual codebase produced four corrections:

1. **§4.4 (typed slots)** assumed `LogConfiguration::Type` had `level` / `message` / `logger` / `caller` values and that `LogValue` had a level enum alternative. Neither is true; the requirement was ported wholesale from hl's `Record` shape without checking. **Dropped** to Appendix A.
2. **The original §4.2 (pool) quantification** cited "~700–1000 batches per `[large]` parse and ~1000 lines/batch" → "~2100 fresh small allocations". With `kDefaultBatchSizeBytes = 1 MiB` and a 181 MB `[large]` fixture, the actual count is **~174 batches × 3 = ~520 allocations** (parent PRD task list confirms 174). The legacy `Parse(path)` flow's `BufferingSink::PrefersUncoalesced=true` further drains the inner-vector capacity through whole-vector `std::move`, defeating the pool's "preserve capacity" benefit on three of four headline benchmarks. **Demoted to a conditional follow-up** (new §4.3) gated on `[stream_to_table]` profiling.
3. **The original §4.1 (hash)** missed that `KeyIndex::Impl::ShardIndex` must be swapped in lockstep with the per-shard `TransparentStringHash`. The v1 wording made this conditional ("if `KeyIndex` ever defines its own hash adapter"); it does. **Demoted to a conditional follow-up** (new §4.2) gated on `[no_thread_local_cache]` and the GUI-thread `Find` cost — the per-worker key cache absorbs ~99 % of canonical lookups, so the steady-state `[large]` win is unlikely to clear the noise floor.
4. **The original §4.3 (small-vector)** was at P2 in v1 despite eliminating ~1 M allocations per `[large]` parse and being the single largest individual win in the PRD. **Promoted to P1** (new §4.1) — it is the load-bearing change for the G2 / M5 allocation-footprint goal and underpins the M1 trajectory.

The new ordering is §4.0 (IPO, always) → §4.1 (small-vector, always) → measurement gate → §4.2 (hash, conditional) and §4.3 (pool, conditional). The typed-slots prize is left to whatever future PRD owns the filter/sort engine.

## Appendix B — Baseline measurements (post-parent-PRD)

Captured at the close of `tasks-parser-performance-hardening.md` task 10.0:

| Benchmark | Mean | Throughput |
| --- | --- | --- |
| `[large]` warm-up (1 M / 181 MB) | 0.21 s | **867.88 MB/s** |
| `[wide]` warm-up (1 M / 30-column) | 0.57 s | TBD (varies; see task 8.0 commit) |
| `[stream_to_table]` warm-up (1 M) | TBD (see task 9.0 commit) | TBD |
| Stage B utilisation `[large]` | — | **74.65 %** |
| Stage B utilisation `[wide]` | — | **91.46 %** |
| Stage A wall-clock %, `[large]` | — | **0.33 %** |
| Stage A wall-clock %, `[wide]` | — | **0.46 %** |
| Stage C wall-clock %, `[wide]` | — | **22.6 %** (post-9.5 fix) |
| `[allocations]` per-line count | — | **~1.0** |
| `[allocations]` `string_view` fast-path fraction, `[wide]` | — | **99.9999 %** (11 999 988 / 12 000 000) |
| `[cancellation]` median / p95 | — | **~5.6 ms / ~6.2 ms** |

These are the numbers every PR in this PRD compares against.

### B.1 Post-hl-PRD baseline (captured at task 6.0 close-out, 2026-04-27)

Captured at the close of `tasks-hl-inspired-parser-performance.md` task 6.0 — i.e. with task 1.0 (IPO/LTO) shipped and tasks 2.0 / 4.0 / 5.0 closed per G6 / below-threshold (no code on disk). All numbers are isolated per-process readings on `benchmarks.exe`. Hardware: Windows 10 / x64, 8 logical cores, clang-cl 22 (LLVM 22) targeting `x86_64-pc-windows-msvc`.

| Benchmark | Pre-PRD baseline (Appendix B) | Post-PRD final | Delta |
| --- | --- | --- | --- |
| `[large]` warm-up (1 M / 181 MB) | 867.88 MB/s | **1158.08 MB/s** | **+33.4 %** |
| `[large]` Catch2 mean of 5 | — | 1388.82 ms ± 14.13 ms | — |
| `[wide]` warm-up (1 M / 30-col / 735 MB) | TBD (see task 8.0 commit) | **1354.46 MB/s** | — |
| `[wide]` Catch2 mean of 5 | — | 3297.53 ms ± 53.79 ms | — |
| `[stream_to_table]` warm-up (1 M) | TBD (see task 9.0 commit) | **150.20 MB/s**, 868.5 k lines/s | — |
| `[stream_to_table]` `LogTable::AppendBatch` | — | 51.47 ms / 174 batches (5.15 ms / 100 k lines) | — |
| `[stream_to_table]` Catch2 mean of 5 | — | 1304.28 ms ± 9.72 ms | — |
| Stage B utilisation `[large]` | 74.65 % | **85.08 %** | **+10.4 pp** |
| Stage B utilisation `[wide]` | 91.46 % | **91.66 %** | within noise |
| Stage A wall-clock %, `[large]` | 0.33 % | **0.43 %** | within noise |
| Stage A wall-clock %, `[wide]` | 0.46 % | **0.55 %** | within noise |
| Stage C wall-clock %, `[wide]` | 22.6 % | **33.7 %** | **+11.1 pp** ⚠ — drives the §4.5.2 follow-up PRD recommendation; Stage C is now the dominant non-parallelised cost |
| Stage C wall-clock %, `[large]` | — | **42.3 %** (54.4 ms / 128.7 ms) | — same dominance shape as `[wide]` |
| `[allocations]` per-line count | ~1.0 | **1.004** | unchanged (M5 forfeit per task 2.0 closure) |
| `[allocations]` `string_view` fast-path fraction, `[large]` | — | **99.9 %** | — |
| `[allocations]` `string_view` fast-path fraction, `[wide]` | 99.9999 % | **99.9999 %** | unchanged |
| `[no_thread_local_cache]` warm-up (10 k) | — | **327.55 MB/s**, 1.88 M lines/s | — |
| `[no_thread_local_cache]` Catch2 mean of 5 | — | 15.94 ms ± 0.36 ms | — |
| `[get_value_micro]` slow path (string_view lookup) | — | 1.272 ms ± 0.119 ms | — |
| `[get_value_micro]` fast path (KeyId lookup) | — | 0.174 ms ± 0.006 ms | **7.3×** speed-up over slow path |
| `[cancellation]` median / p95 | 5.6 ms / 6.2 ms | **3.36 ms / 6.91 ms** | median **−40 %**, p95 +11.5 % (within p95-on-20-samples noise) |
| `sizeof(LogLine)` | ~96 bytes | ~96 bytes | unchanged (§4.1's `static_assert` not added because §4.1 reverted) |
| IPO link-time delta (§4.0 / task 1.0) | n/a | < 1 s incremental on Ninja, full-clean < 30 s | informational |

These are the numbers the **next** performance PRD compares against. Headline movers were §4.0's IPO/LTO (drove M1 from 867.88 → 1158.08 MB/s and M4 from 74.65 % → 85.08 %) and the parent PRD's §4.8 / §4.9 work (drove `[cancellation]` median from 5.6 → 3.36 ms). Stage C wall-clock fraction climbed to ~42 % on `[large]` and ~34 % on `[wide]` — that climb is the most actionable signal for the follow-up §4.5 PRD seed (§4.5.2 SIMD / parallelised newline-counter and post-pipeline Stage C).
