# PRD: High-Performance Streaming JSON Log Parser

## 1. Introduction / Overview

Parsing JSON log files in `structured_log_viewer` is currently a single-threaded, blocking operation. The GUI shows nothing until `JsonParser::Parse` has finished walking the entire file, which can take a long time for large logs. The hot path in `JsonParser::ParseLine` also spends significant time constructing a `std::string` key for every JSON field of every line and inserting it into a `tsl::robin_map<std::string, LogValue>`.

This feature replaces that pipeline with a high-throughput, multi-threaded, **streaming** JSON parser:

1. A **shared, lock-light, append-only** key-dictionary (`KeyIndex`) that maps every log field key to a dense integer id. Storage in `LogLine` is index-addressed, eliminating per-field string allocations in the hot loop and eliminating any per-batch KeyId-remap pass.
2. **`std::string_view`-backed log values** that point directly into the memory-mapped file for the common case of unescaped JSON strings (and always for raw object/array values). This eliminates the per-value `std::string` copy that dominates RSS and a large slice of CPU on 2 GB inputs. `LogFile` is reworked to own the `mio::mmap_source` so the views remain valid for the lifetime of the `LogData`.
3. A producer/consumer pipeline where one thread finds **batch boundaries** in the file and dispatches them to worker threads for parsing, built on **oneTBB**'s `parallel_pipeline` (library choice validated by a spike — see req. 4.2.17). Stage A no longer touches per-line bookkeeping; Stage B builds line offsets locally during parsing and Stage C concatenates them into the destination `LogFile`.
4. A streaming path from the parser to the Qt `LogModel` so rows appear in the UI while parsing is still running, and so the pipeline can later be driven by a live source (`tail -f`-style).

**Goal:** parse GB-scale JSON log files in seconds — not minutes — while keeping the Qt GUI responsive and rows visible as they stream in.

## 2. Goals

1. **Throughput:** The new `JsonParser` pipeline **must** be measurably faster than the current `main` implementation on the `benchmark_json.cpp` micro-benchmark and on the new large-file benchmark (req. 4.5.34). The exact speedup is not pre-committed; instead, **the PR description must report before/after wall-clock numbers** for both benchmarks on the author's machine, and any regression on either benchmark must be explained.
2. **Single-thread efficiency:** With `JsonParser::Options::threads = 1`, the hot `ParseLine` path **must** match or beat the current `main` implementation thanks to key indexing — no per-field `std::string` allocations and no per-line hash-map rehash on cache miss. The PR description **must** include a single-thread before/after number from the new `[benchmark][json_parser][single_thread]` benchmark variant (req. 4.5.34).
3. **Scaling:** The parallel pipeline **must** scale with worker count on a representative 500 MB – 2 GB JSON log file: more workers up to `min(hardware_concurrency, 8)` produce a measurable wall-clock improvement vs. fewer workers on the same input, and adding workers beyond that point **must not** regress wall clock. Specific scaling curves are not pre-committed; the PR description **must** include a small thread-scaling table (1 / 2 / 4 / 8 / hw_concurrency) from the large-file benchmark.
4. **Streaming UI:** When opening a file in the Qt app, the first rows **must** appear in the `LogModel` quickly enough to feel non-blocking on a modern desktop (target: under a few hundred milliseconds for a 500 MB file), and the table **must** continue to populate while parsing proceeds in the background. The UI thread **must** remain responsive — no perceptible freezes — during parsing. The PR description **should** note the observed time-to-first-row and any UI hitches seen during manual testing.
5. **Ordering:** Final `LogData.Lines()` **must** be in strict original file order (line number ascending). Order **must** also be preserved as lines stream into the model — every `OnBatch` call delivers a contiguous range of lines whose first line number is greater than the last line number of all previously delivered batches.
6. **Correctness:** All existing tests in `test/lib` and `test/app` **must** continue to pass after being updated for the new storage shape (req. 4.1.16). Public behavior of `LogParser`, `LogLine`, `LogTable`, `LogModel` **must** keep working semantically; signatures **may** change where the new design requires it (see req. 4.1.14 and the public-API note there), and call sites **must** be updated in the same PR.
7. **Benchmarks in CI:** A Catch2 benchmark section (extending `test/lib/src/benchmark_json.cpp`) **must** be in place so that future PRs can re-run before/after numbers locally. CI **must not** fail purely on benchmark variance, but the new benchmarks **must** be runnable from the existing CI matrix (gated behind their `[benchmark]` tag).

## 3. User Stories

- **As a developer analyzing a 2 GB service log**, I want to open the file and immediately start scrolling and filtering the first records, so I don't have to wait for the whole file to load before I can work.
- **As a developer on a multi-core machine**, I want the log viewer to use all my cores when parsing a big file, so I spend seconds instead of minutes waiting.
- **As a developer debugging a live system (future follow-up)**, I want to point the viewer at a growing log file and have new lines appear as they are written, so I can watch events in near-real-time.
- **As a library user calling `loglib::JsonParser::Parse`**, I want the function to remain usable in a blocking "give me everything at once" mode for scripts/tests, while also offering a streaming/callback mode for UI use.
- **As a maintainer**, I want parse errors surfaced both as they happen (live status-bar counter, updated by `MainWindow` via Qt signals from `LogModel`) and in a final summary dialog, so I never miss errors in large files.

## 4. Functional Requirements

### 4.1 Key-Indexed Log Storage (full refactor — answer 2C)

#### 4.1.1 KeyIndex / KeyId types (new public header)

1. The system **must** introduce a new public header `loglib/key_index.hpp` that defines:
   - `using KeyId = uint32_t;`
   - `constexpr KeyId kInvalidKeyId = std::numeric_limits<KeyId>::max();`
   - A `class KeyIndex` that maps each distinct log-field key (string) to a stable, dense integer id, with at minimum: `GetOrInsert(std::string_view) -> KeyId`, `Find(std::string_view) const -> KeyId` (returns `kInvalidKeyId` if absent), `KeyOf(KeyId) const -> std::string_view`, `Size() const -> size_t`, and `SortedKeys() const -> std::vector<std::string>`.
2. **`KeyIndex` is shared by all Stage B workers and is internally thread-safe** (see req. 4.2.20 and Decision 13). It is append-only for the duration of a parse: an inserted `KeyId` is never invalidated, reassigned, or removed. After parsing finishes the `KeyIndex` owned by `LogData` is effectively read-only.
2a. **Thread-safety contract** (mandatory for the implementation; the choice of underlying container is left to the implementer):
    - `GetOrInsert(std::string_view)` **must** be safe to call concurrently from any number of Stage B workers without external synchronization. The signature **must** take `std::string_view` (not `const std::string&`) so the hot path does not allocate a `std::string` on every probe.
    - `Find(std::string_view)` and `KeyOf(KeyId)` **must** be safe to call concurrently with `GetOrInsert` and with each other.
    - **The forward-lookup container must support heterogeneous `string_view` lookup**, i.e. probing without constructing a `std::string`. For `tbb::concurrent_hash_map`, this means a `HashCompare` whose `hash(const std::string_view&)` and `equal(const std::string&, const std::string_view&)` overloads are provided. For `std::shared_mutex + tsl::robin_map`, this means C++20-style transparent hashing (`is_transparent`).
    - `KeyOf(id)` **must** return a `std::string_view` whose backing storage remains valid until the owning `LogData` is destroyed, even if other threads are concurrently calling `GetOrInsert`. The reverse-lookup storage **must** therefore be pointer-stable across concurrent inserts — `std::vector<std::string>` is *not* sufficient because `push_back` can reallocate; use `std::deque<std::string>`, `std::vector<std::unique_ptr<std::string>>`, a chunked arena, or `tbb::concurrent_vector`.
    - `KeyId`s **must** be assigned as a dense, monotonically increasing sequence starting at `0`, so the "high-water-mark" computation of newly-introduced keys per flush in req. 4.3.25 (`new_keys = canonical.KeyOf([prev_size, current_size))`) is correct and lock-free on the read side.
    - The recommended implementation is `tbb::concurrent_hash_map<std::string, KeyId, TransparentHashCompare>` for the forward lookup plus a `std::deque<std::string>` (guarded by the `concurrent_hash_map` write accessor on insert) for the reverse lookup. An `std::shared_mutex` + `tsl::robin_map` (with transparent hashing) variant is acceptable if benchmarks show comparable performance. The choice is an implementation detail and **must** be hidden behind `loglib/key_index.hpp`.
2b. **Per-worker KeyId cache** (data-driven optimization, opt-in via `JsonParserOptions::useThreadLocalKeyCache`, default `true`): when enabled, each Stage B worker maintains a thread-local cache (e.g. a `tsl::robin_map<std::string, KeyId>` with **heterogeneous (`std::string_view`) lookup** so probing does not allocate a `std::string`) via `tbb::enumerable_thread_specific`. A key already seen by this worker hits a single thread-local hash lookup with no atomic operation and no shared-state read. On a thread-local miss the worker calls `KeyIndex::GetOrInsert`, which is itself lock-free in the common-key case (TBB's `concurrent_hash_map` `find` is a brief shared-accessor read with `string_view` heterogeneous lookup so it also does not allocate) and only takes a write accessor for genuinely new keys. With the cache enabled, thread-local hit rate after warm-up (typically the first ≤ 1 % of the file) is essentially 100 % and the canonical `KeyIndex` is written-through once per key for the entire parse.

    Rationale for opt-in rather than mandatory: the steady-state win is going from "one shared-accessor read on TBB's `concurrent_hash_map`" to "one thread-local hash lookup", which is real but small relative to the per-line JSON parse cost. The PR description **must** report a benchmark variant `[benchmark][json_parser][no_thread_local_cache]` that disables the cache so the actual saving is measured rather than assumed; if the cache buys < 5 % on the 1 000 000-line benchmark the implementer **may** propose flipping the default to `false` to reduce code surface.
3. `LogData` **must** own the canonical `KeyIndex` for its lines and expose it via a const accessor (e.g. `const KeyIndex& Keys() const`). The existing `std::vector<std::string>& Keys()` accessor is replaced by/derived from this; `LogData::Keys()` returning a `std::vector<std::string>` may remain as a convenience that internally calls `KeyIndex::SortedKeys()`.
4. **Appending batches** (the merge-and-remap dance from earlier drafts is removed — it is unnecessary because all workers share the canonical `KeyIndex` directly). `LogData` **must** expose:

    ```cpp
    // Used by Stage C (req. 4.2.18). Because Stage B workers parsed `lines`
    // directly against this->Keys() (the shared canonical KeyIndex), there
    // is no KeyId remap pass and no back-pointer rewire. Stage C only:
    //   1. Concatenates the per-line stream offsets the worker built (the
    //      starting byte offset of each line in the file, see req. 4.2.18
    //      Stage A/B/C split) into the destination LogFile's mLineOffsets.
    //   2. Appends the lines to this->Lines().
    void AppendBatch(std::vector<LogLine>&& lines,
                     std::vector<uint64_t>&& localLineOffsets);

    // Cross-`LogData` merge (used by future "open multiple files" UX). Walks
    // `other.Keys()` to populate this->Keys() (computing a remap), then walks
    // moved-in lines applying the remap and rewiring back-pointers. NOT used
    // by the streaming pipeline.
    void Merge(LogData&& other);
    ```

    Cross-`LogData` `Merge` keeps the explicit remap path because two independently-parsed `LogData`s have independent `KeyIndex`es. The streaming pipeline does not exercise this path.

#### 4.1.2 LogLine storage and API

5. `LogLine` **must** be refactored to store values in an index-addressed container. The chosen storage **must** be a contiguous **`std::vector<std::pair<KeyId, LogValue>>` sorted in ascending `KeyId` order** so that `IndexedValues()` can return a `std::span` (req. 4.1.8). Other layouts (parallel vectors, dense vector with sentinel) are explicitly rejected to keep the public `IndexedValues` return type stable.
    - **Sort policy (mandatory, was previously ambiguous).** Stage B receives JSON fields in document order, not `KeyId` order. The parser **must** push `(KeyId, LogValue)` pairs onto its thread-local scratch vector (req. 4.2.20a) in JSON document order and call `std::sort` exactly once on the scratch vector at end-of-line, keyed on `KeyId` (`std::sort(begin, end, [](auto& a, auto& b){ return a.first < b.first; })`). Insertion-sort or per-field `std::lower_bound`-then-`insert` are **explicitly rejected**: typical log lines have 5–20 fields, where `std::sort` is faster (one pass, no shifts) and dramatically simpler than maintaining sorted invariants per insert.
    - `LogLine::SetValue(KeyId, LogValue)` (the user-facing fast-path setter, req. 4.1.8) is **not** on the parse hot path; it **may** use a `std::lower_bound`-then-`insert`/update because correct API semantics matter more than throughput here.
    - **No `count_fields()` pre-pass.** The current `JsonParser::ParseLine` calls `simdjson::ondemand::object::count_fields()` (`library/src/json_parser.cpp:209`) just to size the destination `tsl::robin_map`. With the new sorted-vector storage there is no rehash to avoid; `count_fields` is a full extra walk over the JSON object and **must** be removed. The new parser **must** instead grow the per-line vector incrementally (a thread-local scratch vector reused across lines, req. 4.2.20a, makes this allocation-free in steady state).
6. `LogValue` **must** be extended to hold the following alternatives, in this order so that the variant tag is stable across builds:

    ```cpp
    using LogValue = std::variant<
        std::string_view,  // NEW: zero-copy view into the owning LogFile's mmap.
                           //      Used by JsonParser::ParseLine for unescaped string
                           //      values and for raw_json of objects/arrays.
                           //      Lifetime is bounded by the LogFile that owns the
                           //      mmap (req. 4.1.6a, req. 7 mmap-ownership).
        std::string,       // Owned. Used for escape-decoded string values, for
                           //        values written via SetValue from user code, and
                           //        for any LogValue produced by paths that do not
                           //        have an mmap to point into.
        int64_t,
        uint64_t,
        double,
        bool,
        TimeStamp,
        std::monostate>;
    ```

    Downstream display/sort/serialize code **must** treat `std::string_view` and `std::string` as semantically equivalent string carriers. To avoid sprinkling `std::visit` across the codebase, `loglib` **must** provide free helpers in `loglib/log_line.hpp`:

    ```cpp
    // Returns the string content of v as a string_view if v holds either
    // string alternative; std::nullopt otherwise.
    std::optional<std::string_view> AsStringView(const LogValue& v);

    // Convenience: true iff v holds either std::string or std::string_view.
    bool HoldsString(const LogValue& v);

    // Lifetime-promotion helper: if v holds std::string_view, returns a
    // LogValue holding an OWNED std::string with a copy of the bytes;
    // otherwise returns v unchanged. Use this whenever a LogValue must
    // outlive the LogFile that backs the original view (e.g. when copying
    // a LogValue out of a LogLine for clipboard, for a detail dialog, or
    // for any persistent UI state that survives a reload).
    LogValue ToOwnedLogValue(const LogValue& v);
    ```

    All in-tree string-typed reads (`LogTable::FormatLogValue`, `log_processing.cpp::ParseTimestampLine`, `log_model.cpp::data`, sort/filter paths) **must** be updated to use these helpers. Direct `std::holds_alternative<std::string>` / `std::get<std::string>` against a `LogValue` is **forbidden** in new code because it silently misses the `string_view` alternative.

    **Lifetime contract for `LogValue::std::string_view`** (mandatory; the fast `SetValue(KeyId, LogValue)` path is otherwise a footgun): any `LogValue` holding a `std::string_view` alternative borrows from a backing buffer. Inside `loglib`, that buffer is the `LogFile`'s mmap, which lives until `LogData` is destroyed. **Callers that copy a `LogValue` out of a `LogLine`** (clipboard, detail dialog, filter state, undo stack, anything that persists across a `LogModel::Clear()`) **must** call `ToOwnedLogValue(v)` first. In debug builds, `LogLine::SetValue(KeyId, LogValue)` **should** assert that, when the value holds a `std::string_view`, either (a) the line was constructed by Stage B against the parent `LogData`'s `LogFile` mmap, or (b) the call site has been annotated with a `LogValue::TrustView{}` tag (a zero-byte tag type added for the few callers that genuinely own the underlying buffer). Outside debug, no runtime check is feasible; this is a documented contract.
6a. **`LogFile` owns `mio::mmap_source`** (corollary of req. 4.1.6 and a prerequisite for it). The mmap is currently a function-local in `JsonParser::Parse` (`library/src/json_parser.cpp:59`) that dies at function exit. It **must** move into `LogFile` itself (e.g. `mio::mmap_source mMmap` member, opened in `LogFile`'s constructor or by an `Open()` member, closed in the destructor) so the mmap stays alive for the lifetime of the `LogData` that owns the `LogFile`. This keeps every `std::string_view` alternative of `LogValue` valid until `LogData` is destroyed.
    - **Drop the `std::ifstream` member entirely** (currently `LogFile::mFile` at `library/include/loglib/log_file.hpp:110`). With the mmap owned by `LogFile`, every read path can use `std::string_view{mMmap.data() + mLineOffsets[i], length}`; there is no remaining caller for the `ifstream`. Removing it shrinks `LogFile` by ~256 bytes and eliminates a footgun (an unsynchronized stream object on a `noexcept`-movable type). `LogFile::GetLine(size_t)` **must** be reimplemented to slice the mmap directly using `mLineOffsets[i]` and `mLineOffsets[i+1]` (or `mMmap.size()` for the last line), trimming a trailing `'\r'` if present.
    - **Replace `std::vector<std::streampos>` with `std::vector<uint64_t>`** for `mLineOffsets`. `std::streampos` is an `fpos_t` wrapper and is bigger than `size_t` on MSVC (24 bytes vs 8 bytes); for a 2 GB file with ~20 M lines the storage difference is ~320 MB vs ~160 MB. `uint64_t` is also faster to compare/index. The `LogFile::CreateReference(std::streampos)` overload **must** be removed (the `size_t` overload survives); call sites are updated in the same PR. This also matches Stage B's new `local_line_offsets: std::vector<uint64_t>` (req. 4.2.18) so Stage C's concatenation is a single `vector::insert` of the same element type.
    - **Reserve `mLineOffsets` upfront at `BeginStreaming`.** Use a heuristic such as `std::filesystem::file_size(path) / 100` (≈ 100 bytes per JSON log line is conservative for typical service logs). One `reserve` call up front amortizes Stage C's per-batch `insert` to amortized O(1) per line and avoids the 5–6 reallocations a 20 M-element vector would otherwise see. Implemented via a new `LogFile::ReserveLineOffsets(size_t)` method called from `LogModel::BeginStreaming` (which knows the file size).
    - **Mmap address stability under move (mandatory test).** `LogValue::std::string_view` alternatives are pointers into `mMmap.data()`. `LogFile` is `std::vector<std::unique_ptr<LogFile>>`-owned, so the `LogFile*` is pointer-stable, but the implementation also relies on `mio::mmap_source`'s move constructor preserving the **mapped page address** (it transfers the kernel mapping handle without remapping; this is the documented behavior of `mio::mmap_source`). The PR **must** add a Catch2 test in `test/lib/src/test_log_file.cpp` that:
        1. Constructs a `LogFile` from a fixture file and captures `const char* original = lf.Data();` (a new accessor returning `mMmap.data()`, also added in this PR for the parser to use).
        2. Move-constructs a second `LogFile` from the first.
        3. Asserts the moved-to instance's `Data()` returns the same pointer.
        4. Reads through that pointer and asserts the bytes round-trip.
        Failure of this test means an `mio` upgrade has broken the lifetime contract and Stage B's `string_view` storage is unsafe — every `LogLine`'s string values would silently become dangling.
6b. **OS prefetch hint on the mmap.** Immediately after the mmap is established in `LogFile` (req. 4.1.6a), the implementation **must** advise the kernel that the access pattern is sequential, so reads in Stage A and Stage B benefit from page-cache prefetching:
    - On POSIX: `posix_madvise(mMmap.data(), mMmap.size(), POSIX_MADV_SEQUENTIAL)` (or `madvise(..., MADV_SEQUENTIAL)`).
    - On Windows: `PrefetchVirtualMemory` over the mapped range, or `WIN32_MEMORY_RANGE_ENTRY` via `PrefetchVirtualMemory` with `WIN32_MEMORY_RANGE_ENTRY{ data, size }`.
    - The platform branches **must** be wrapped in a small helper (`loglib::HintSequential(const mio::mmap_source&)`) in `library/src/log_file.cpp`; failures are non-fatal (best-effort hint).
    - Worth measuring: the PR description **should** include before/after numbers with the hint disabled vs. enabled on the cold-cache 1 000 000-line benchmark.
7. **Compatibility (slow) accessors** — preserved 1:1 from the current API so existing app code, tests, and downstream tools keep compiling and behaving identically:
    - `LogValue LogLine::GetValue(const std::string &key) const` — returns `std::monostate` when missing. Internally resolves `key → KeyId` via the owning `LogData`'s `KeyIndex` and forwards to the fast path.
    - `void LogLine::SetValue(const std::string &key, LogValue value)` — looks up or inserts into the `KeyIndex`, then forwards to the fast path.
    - `std::vector<std::string> LogLine::GetKeys() const` — resolves all stored `KeyId`s back to strings.
8. **Fast (indexed) accessors** — additive, public on `LogLine`, used by the parser, by `JsonParser::ToString`, by `LogTable`, and by any future view that iterates many lines:
    - `LogValue LogLine::GetValue(KeyId id) const` — returns `std::monostate` when missing. **Lookup strategy is implementation-defined** but **must** exploit the sorted invariant for correctness, not necessarily for asymptotic complexity. For typical log lines (5–20 fields) a **linear scan with early exit on `id < entry.first`** is generally faster than binary search on modern CPUs because the entire vector fits in one cache line and the branch predictor learns the iteration. The implementation **may** use a hybrid that switches to `std::lower_bound` above a small threshold (e.g. `mValues.size() > 32`). The PR description **must** include a Catch2 micro-benchmark of `GetValue(KeyId)` over realistic field counts (5, 10, 20, 50) to justify the chosen strategy; pure binary search is **not** required.
    - `void LogLine::SetValue(KeyId id, LogValue value)` — inserts in sorted position or updates the existing entry without touching strings or hashes. May use `std::lower_bound` for placement; not on the hot path.
    - `std::span<const std::pair<KeyId, LogValue>> LogLine::IndexedValues() const` — returns the backing vector as a span in `KeyId`-ascending order. The concrete `std::span` return type pins the storage layout in req. 4.1.5; consumers (`JsonParser::ToString`, future bulk-walk consumers) can iterate without virtual dispatch or template instantiation.
9. To make the slow accessors work, `LogLine` **must** carry a non-owning back-pointer to the `KeyIndex` it currently resolves against, stored as a dedicated member on `LogLine` itself (`const KeyIndex* mKeys = nullptr;`). With the shared canonical `KeyIndex` (req. 4.1.2/2a), Stage B workers construct each `LogLine` with `mKeys = &canonicalKeys` from the very first parse, so **no rewire ever happens during the streaming path**. The rewire-in-place capability is preserved for the cross-`LogData` `Merge` path only:
    - The owning `LogData` exposes `const KeyIndex& Keys() const` (req. 4.1.3) and the canonical `KeyIndex` lives there.
    - `LogData::Merge` (the only path that takes `LogLine`s built against a *different* `KeyIndex`) **must** rewrite each merged-in line's back-pointer to the destination `KeyIndex` in the same pass that rewrites its `KeyId`s.
    - `LogLine`'s constructor **must** take the `KeyIndex` reference explicitly so it cannot be forgotten. A `LogLine` whose `mKeys` is null is a programmer error and the slow accessors **may** assert on it in debug builds.
9a. **`LogLine::mFileReference` is no longer `const`-qualified, and `LogFileReference::mLineNumber` is no longer `const`-qualified either.** The current headers declare `const LogFileReference mFileReference;` (`library/include/loglib/log_line.hpp:99`) and `const size_t mLineNumber = 0;` (`library/include/loglib/log_file.hpp:52`), which makes both `LogLine` and `LogFileReference` move-constructible but **not** move-assignable. The streaming pipeline pushes `LogLine`s into and out of `std::vector` between TBB filters and inside `ParsedBatch`, which requires move-assignment whenever a vector grows. Additionally, Stage C overwrites the line number on each line as it computes the prefix sum (req. 4.2.18), which requires `LogFileReference::SetLineNumber(size_t)` to be writable. Both `const` qualifiers **must** be dropped so `LogLine` is fully `noexcept`-movable and `LogFileReference` exposes a `SetLineNumber(size_t)` method. The rest of `LogFileReference`'s API stays the same.
10. The public type alias `LogMap` (`tsl::robin_map<std::string, LogValue>`) is **removed**, and so is `LogLine::Values()` (both `const` and non-`const` overloads). All callers that currently iterate `LogLine::Values()` as a `tsl::robin_map` are updated to use `IndexedValues()` (fast) or `GetKeys()` + `GetValue(string)` (slow). Affected files (full list — verify each compiles and passes after the refactor):
    - `library/src/log_table.cpp`
    - `library/src/log_processing.cpp`
    - `library/src/json_parser.cpp` (`ToString` + `ParseLine`)
    - `library/include/loglib/log_parser.hpp` (virtual signature change, see req. 4.1.14)
    - `app/src/log_model.cpp` (verify; no current `LogMap` use, but `LogLine` consumers may need updating once `Values()` is gone)
    - `test/lib/src/test_log_line.cpp`
    - `test/lib/src/test_log_data.cpp`
    - `test/lib/src/test_log_table.cpp`
    - `test/lib/src/test_log_processing.cpp`
    - `test/lib/src/test_log_configuration.cpp`
    - `test/lib/src/test_json_parser.cpp`

#### 4.1.3 LogTable hides indexes (per design note)

11. `LogTable`'s public API **must not** expose `KeyId` or `KeyIndex` types. Its existing accessors stay byte-for-byte the same:
    - `std::string GetHeader(size_t column) const`
    - `LogValue GetValue(size_t row, size_t column) const`
    - `std::string GetFormattedValue(size_t row, size_t column) const`
    - `size_t ColumnCount() const`, `size_t RowCount() const`, etc.
    Rationale: most consumers (Qt model, find widget, filter editor, copy-line) think in row/column ints and don't need to learn about `KeyId`.
12. `LogTable` **must** internally precompute and cache a `column → KeyId` table when its configuration / data is set, then dispatch every per-cell call (`GetValue`, `GetFormattedValue`) through `LogLine::GetValue(KeyId)` — never through the string-keyed slow path. This is what makes the indexed storage actually faster at the GUI level, not just at parse time.
13. **Mid-stream column extension is append-only.** When `LogTable::Update(LogData&&)` is called, or when new keys arrive mid-stream via the new `LogTable::AppendBatch` (req. 4.3.27a), `LogTable` **must extend** its `column → KeyId` cache by appending new entries at the end. Existing column indices **must never** change position or `KeyId`. Rationale:
    - The Qt `LogModel` caches column indices in views/sort/filter state; reordering would corrupt those.
    - `beginInsertColumns(...)` (req. 4.3.27) only models tail insertion; reordering would force a `beginResetModel`.
    - `LogConfigurationManager::Update(data)` **must** be amended (or wrapped at the `LogTable` boundary) to honor this contract: new columns are appended, never inserted in the middle, and never reordered. The implementation PR **must** add a Catch2 test that drives `LogTable::AppendBatch` with a series of batches that progressively introduce new keys and asserts that already-known columns retain their indices and that new columns appear strictly at the end.
13a. **`LogTable::AppendBatch` (new public API for streaming).** The streaming path **must not** route per-batch arrivals through `LogTable::Update(LogData&&)` (which is built around merging two complete `LogData` objects). Instead `LogTable` **must** expose:

    ```cpp
    // Streaming-mode batch append. Called once per OnBatch flush from the
    // QtStreamingLogSink on the GUI thread. Atomically:
    //   1. Extends the column set if `batch.new_keys` is non-empty (per req.
    //      4.1.13). New columns are always appended at the end.
    //   2. Appends the lines to mData via LogData::AppendBatch.
    //   3. If any newly-added column has type `Type::time`, OR if any pre-existing
    //      time column's KeyId set is not a subset of mStageBSnapshotTimeKeys,
    //      runs BackfillTimestampColumn over all rows for those columns
    //      (req. 4.1.13b).
    //
    // Returns void. Callers (i.e. LogModel::AppendBatch) drive Qt model
    // updates by bracketing this call with their own RowCount()/ColumnCount()
    // observations — see req. 4.3.27. Returning a metadata struct from here
    // would couple loglib to Qt's beginInsertRows API shape; loglib stays
    // Qt-agnostic.
    void AppendBatch(StreamedBatch batch);
    ```

    `LogTable::Update(LogData&&)` continues to exist for non-streaming callers (factory tests, scripts, the legacy code path) and runs the legacy whole-data `ParseTimestamps` pass.
13b. **Mid-stream timestamp back-fill (auto-promotion only).** With the configuration locked while streaming (req. 4.2.21), the only remaining mid-stream change is **auto-promotion**: a new key arrives in batch N (added to `batch.new_keys`), and `LogConfigurationManager::Update`'s auto-promotion logic recognises it as a `Type::time` column header. Stage B's snapshot configuration did not contain this column, so the rows already streamed (batches 1..N-1) and the rows in batch N have un-parsed timestamps for the new column. `LogTable::AppendBatch` **must** therefore:
    - Compare the current `LogConfigurationManager` against the Stage-B snapshot it stamped at `BeginStreaming` time (a `std::unordered_set<KeyId> mStageBSnapshotTimeKeys` is sufficient).
    - For every `Type::time` column in the current configuration whose key set is NOT a subset of `mStageBSnapshotTimeKeys`, run `BackfillTimestampColumn(column, mData.Lines())` over **all** rows (already-appended + the rows just appended). After back-filling, add the column's keys to `mStageBSnapshotTimeKeys` so subsequent batches don't re-back-fill.
    - `BackfillTimestampColumn` reuses the per-line logic from `loglib::ParseTimestamps` (`library/src/log_processing.cpp`) including the `LastValidTimestampParse` fast path; it walks rows and updates `LogLine` values via `SetValue(KeyId, TimeStamp{...})`.
    - For implementation simplicity the back-fill runs synchronously on the GUI thread. In practice this fires at most once per file (the first time a previously-unseen time-column key is observed), and walks rows that are already in memory. Profiling **must** be reported in the PR description; if the all-rows back-fill ever shows up as a hitch on the 2 GB benchmark, the implementation **may** offload it to `tbb::parallel_for` over rows and emit `dataChanged(...)` when done.
    - The back-fill **must** emit `dataChanged(topLeft, bottomRight, {Qt::DisplayRole, SortRole})` from `LogModel` so visible cells refresh without invalidating selection/scroll state.
    - Errors produced by the back-fill **must** be appended to the same per-batch error stream as Stage B errors and surfaced via `errorCountChanged` (req. 4.3.27).
    - The **user-edits-time-column-mid-stream** branch from earlier drafts is **gone**: req. 4.2.21 forbids configuration mutation while streaming is active, and the GUI **must** disable the configuration editor between `BeginStreaming` and `EndStreaming` (see req. 4.3.29).

#### 4.1.4 Parser and serializer adaptation

14. `LogParser::ToString` (the public virtual in `library/include/loglib/log_parser.hpp`) **must** be changed from `virtual std::string ToString(const LogMap &values) const = 0;` to `virtual std::string ToString(const LogLine &line) const = 0;`. This is an **intentional public-API break of `loglib`** — the only in-tree caller (`JsonParser::ToString`) and any out-of-tree caller must adapt. The implementation in `JsonParser::ToString` iterates `line.IndexedValues()` (the `std::span` from req. 4.1.8) and resolves keys via the line's owning `KeyIndex` (req. 4.1.9). It **must** treat `std::string` and `std::string_view` alternatives of `LogValue` as equivalent (use `loglib::AsStringView` from req. 4.1.6). It **must** produce identical JSON output for the same logical record as the current `main` implementation, and the existing round-trip tests **must** be updated to drive the new signature and assert this equivalence.
15. `ParseCache` is **opt-in via `JsonParserOptions::useParseCache`, default `true`**. When enabled, it **must** key its type caches by `KeyId` (e.g. `std::vector<simdjson::ondemand::json_type>` and `std::vector<simdjson::ondemand::number_type>` indexed by `KeyId`, plus a parallel `std::vector<bool>` "have-info" bitmap or a sentinel value) instead of by `std::string`. No `std::string` hashing on the per-line path. Because Stage B workers each maintain their own `ParseCache` (req. 4.2.18 thread-local state), and because `KeyId`s are stable across the whole parse (req. 4.1.2/2a), each worker's cache vectors **may** simply `resize(canonical.Size())` lazily on demand when a `KeyId` exceeds the current vector size.

    Rationale for opt-in: the original `ParseCache` (in `library/include/loglib/json_parser.hpp:49`) was introduced to skip `value.type()` and `value.get_number_type()` calls. simdjson on-demand executes both lazily anyway, so the saved work is small. With the per-worker `KeyId` cache (req. 4.1.2/2b) absorbing the string-keyed lookup that originally dominated the cache cost, the remaining benefit is one branch per field. The PR description **must** report a benchmark variant `[benchmark][json_parser][no_parse_cache]` that disables `ParseCache` so the actual saving is measured; if it buys < 3 % on the 1 000 000-line benchmark, the implementer **may** drop `ParseCache` entirely (default `useParseCache = false` and remove the field on a follow-up).
15b. **Field-key access uses `field.key()` for unescaped keys.** The current `JsonParser::ParseLine` calls `field.unescaped_key()` (`library/src/json_parser.cpp:225`), which always allocates if the key contains escape sequences and copies into simdjson's internal buffer. For log-field keys (almost universally ASCII identifiers like `"level"`, `"timestamp"`, `"message"`), the new parser **must** use `simdjson::ondemand::field::key()` (which returns the raw quoted-key span as a `string_view` into the mmap) and:
    - Strip the surrounding quotes (the returned span includes them).
    - If the span contains no backslash, use it directly as the key passed to `KeyIndex::GetOrInsert`. This is the common case and is allocation-free per field.
    - If the span contains a backslash, fall back to `field.unescaped_key()` to get the decoded key.
    - The implementation **must** make this branch a small inline helper (e.g. `loglib::FastFieldKey(simdjson::ondemand::field&) -> std::string_view OR std::string`) so the hot path stays readable.
15a. **`LogValue` string-typed reads** in `JsonParser::ParseLine` (and any new value-emitting paths) **must** prefer the `std::string_view` alternative whenever the value bytes live in the mmap and require no escape decoding:
    - For object/array values (`raw_json()`), the returned `string_view` already points into the source document (the mmap, by construction in Stage B); emit it as `std::string_view`.
    - For scalar string values, use `simdjson::ondemand::value::raw_json_string` to get the un-decoded bytes pointing into the mmap. If they contain no backslash (the common case in log payloads), emit the inner bytes (without surrounding quotes) as `std::string_view` — this is the win that turns most string allocations into pointer copies. If they contain a backslash, fall back to `value.get_string()` and store the decoded result as `std::string`.
    - All other value alternatives (`int64_t`, `uint64_t`, `double`, `bool`, `TimeStamp`, `std::monostate`) are unchanged.
    - The PR description **must** report what fraction of string values landed in the `string_view` fast path on the 1 000 000-line benchmark, so the win is visible.
16. All existing tests touching `LogLine` / `LogMap` / `LogTable` / `LogData` keys (`test_log_line.cpp`, `test_log_data.cpp`, `test_log_table.cpp`, `test_log_processing.cpp`, `test_json_parser.cpp`) **must** be updated to compile and pass against the new storage. New tests **must** be added covering:
    - `KeyIndex::GetOrInsert` get-or-insert semantics under sequential use.
    - `KeyIndex` thread-safety: a `parallel_for` of N threads inserting overlapping key sets results in a single dense KeyId space, every key is mapped to exactly one id, and `KeyOf` returns stable views across the whole test.
    - `LogData::Merge` correctness (cross-`LogData` remap, no key duplication, back-pointer rewire).
    - `LogLine` fast-vs-slow accessor parity (both return the same value for the same logical key).
    - `LogValue` string-vs-string_view equivalence: `AsStringView` returns the same bytes, sort/format paths produce the same output, `JsonParser::ToString` round-trips both alternatives identically.
    - `ToOwnedLogValue` lifetime promotion: a `LogValue` holding a `string_view` into a temporary buffer, after `ToOwnedLogValue`, holds an independent `std::string` whose bytes equal the original and whose lifetime is independent of the source buffer (test by destroying the source buffer and asserting the owned value's bytes).
    - `LogFile` mmap address stability under move (req. 4.1.6a): construct, capture `Data()`, move-construct, assert `Data()` is unchanged and bytes are still readable.
    - **Parity test for the parallel pipeline** (req. 7 / S7): for every fixture in `test/data/`, parsing with `Options::threads = 1` and parsing with `Options::threads = hardware_concurrency` **must** produce equal `LogData`. Equality is defined as: `Lines().size()` equal, `Keys().SortedKeys()` equal, and for every `(line_index, sortedKeyIndex)` pair the two `LogValue`s satisfy `LogValueEquivalent(a, b)` where `LogValueEquivalent` is a free function in `loglib/log_line.hpp` defined as: same active alternative if both are non-string; same byte content (via `AsStringView`) if both are string-typed regardless of `string`/`string_view`; both `monostate`. The test **must** add this helper rather than relying on `operator==` of the variant (which would reject `string_view` ≠ `string` byte-equal cases).

### 4.2 Parallel Parsing Pipeline (answer 3C)

17. The system **must** add **oneTBB** (`https://github.com/uxlfoundation/oneTBB`, Apache 2.0) as a new dependency, fetched via `FetchContent` in `cmake/FetchDependencies.cmake` with a matching `USE_SYSTEM_TBB` option, consistent with existing dependencies. The choice is backed by a committed spike (see `C:\code\pipeline_spike\RESULTS.md` — to be ported into the PR description) showing that, compared to Taskflow and a hand-rolled `std::jthread` pipeline:
    - All three are within ±5 % on steady-state throughput on the reference workload.
    - `tbb::parallel_pipeline` with filter modes `serial_in_order → parallel → serial_in_order` is a **literal** match for our reader → parse → ordered-merge shape, minimizing glue code and ordering bugs.
    - oneTBB's implicit global pool wins measurably on small-file startup (~30 % faster than alternatives on a 10 000-line file), which matters for the "open several files" UX.
    - oneTBB ships adjacent primitives we're likely to use in later work (`concurrent_hash_map`, `scalable_allocator`, `parallel_for` — e.g. for bulk `ParseTimestamps`).
    - The 346 KB `tbb12.dll` runtime is acceptable: shipping a runtime next to the binary is already a solved pattern in this repo (`tzdata/` is copied by `cmake/FetchDependencies.cmake`).
18. `JsonParser::Parse` / `JsonParser::ParseStreaming` **must** be restructured around `tbb::parallel_pipeline` with three filters. The split between stages is designed so Stage A does only O(file_size / batch_size) work, Stage B does the per-line work in parallel, and Stage C is essentially append-only:

    - **Stage A — `tbb::filter_mode::serial_in_order` (reader / batch boundary finder)**: holds the `mio::mmap_source` (which is owned by `LogFile`, req. 4.1.6a) alive and scans forward for **batch boundaries only**. Starting at the previous boundary, advance ≥ `kBatchSizeBytes` (default **256 KiB**, tunable) and then advance to the next `'\n'` so the batch ends on a whole-line boundary. Stage A **must not** find or push per-line offsets — that is now Stage B's job (see below). Stage A **must not** stamp `first_line_number` either; that field is computed in Stage C (see below). The earlier "Stage A reads `prev_line_count` back from Stage C" feedback channel is removed because `tbb::filter_mode::serial_in_order` only orders a filter against itself — Stage A invocation N+1 is allowed to (and routinely does) run before Stage C invocation N completes, so Stage A cannot legally read Stage C state. For each batch, Stage A:
        1. Captures `(batch_index, bytes_begin, bytes_end, file_end)`.
        2. Checks the cooperative `std::stop_token` (req. 4.2.22a). On stop request, calls `tbb::flow_control::stop()` and returns nothing further. Cancellation latency is therefore bounded by the Stage A round trip — see req. 4.2.22b.
        3. Emits the `Batch` token:

        ```cpp
        struct Batch {
            uint64_t       batch_index;         // monotonic, used by Stage C ordering
            const char*    bytes_begin;         // points into the mmap
            const char*    bytes_end;           // exclusive; one past last newline (or EOF)
            const char*    file_end;            // exclusive end of the whole mmap
        };
        ```

        Notably **no `std::vector<LogFileReference>` and no `first_line_number`**. The triple `(bytes_begin, bytes_end, file_end)` exists specifically so Stage B can reproduce the existing simdjson padding fallback at batch boundaries — see req. 4.2.19. **Batches always contain at least one whole line**, even if a single line is larger than `kBatchSizeBytes`. The total Stage A work for a 2 GB file at 256 KiB per batch is ~8 000 `memchr` calls — negligible.

    - **Stage B — `tbb::filter_mode::parallel` (parse + per-batch line-offset construction + per-batch timestamp parse)**: each invocation parses one `Batch` using a **thread-local** `simdjson::ondemand::parser` and (when enabled, see req. 4.1.15 / 4.2.20a) a **thread-local** `ParseCache`. The shared canonical `KeyIndex` (`LogData`'s) is referenced directly — no worker-local index — resolved either directly through `KeyIndex::GetOrInsert` or through the optional per-worker `KeyId` cache from req. 4.1.2/2b. Stage B **must**:
        1. `memchr`-walk `[bytes_begin, bytes_end)` to split the batch into individual lines, building `local_line_offsets: std::vector<uint64_t>` — one entry per line, holding the line's **starting byte offset in the file** (i.e. `static_cast<uint64_t>(line_begin - mmap.data())`). The vector is preallocated with a heuristic capacity (e.g. `(bytes_end - bytes_begin) / 100`).
        2. For each line, parse JSON via `simdjson::ondemand` honoring the padding contract in req. 4.2.19, build a `LogLine` constructed against the canonical `KeyIndex` (no remap, no rewire), and append it to `local_lines`. The `LogFileReference` is constructed deferred — Stage B does **not** know the absolute file line number yet (Stage A no longer provides it; Stage C stamps it). Stage B therefore constructs each `LogLine` with a `LogFileReference(*mLogFile, /*lineNumberPlaceholder=*/0)`; Stage C overwrites the line number on each line as it computes the prefix sum (see Stage C below). `LogFileReference`'s `lineNumber` member is therefore mutable through a dedicated `LogFileReference::SetLineNumber(size_t)` method that **must** be added in this PR; the rest of `LogFileReference`'s API stays the same. (Alternative: keep the line number out of `LogFileReference` entirely and store `local_first_line_number` once per `ParsedBatch`, computing the absolute line number on demand. The implementer **may** pick either; the `SetLineNumber` variant matches today's storage shape and is the smallest delta.)
        3. Reuse a **thread-local scratch `std::vector<std::pair<KeyId, LogValue>>`** (managed via `tbb::enumerable_thread_specific`, req. 4.2.20a) as the destination for each line's indexed values. Stage B emits fields in JSON document order (not `KeyId` order) and **must** sort the scratch vector exactly once at end-of-line via `std::sort` keyed on `KeyId` (see req. 4.1.5 for the sort policy). After sorting, the scratch vector is `std::move`'d into the `LogLine` and its capacity is recovered for the next line — eliminating per-line vector allocation.
        4. Run timestamp parsing on `local_lines` using the snapshot `LogConfiguration` from `Options::configuration` (req. 4.2.21), including the `LastValidTimestampParse` fast-path optimization. Errors are appended to `local_errors`.

        Produces a `ParsedBatch`:

        ```cpp
        struct ParsedBatch {
            uint64_t                       batch_index;
            std::vector<LogLine>           lines;               // KeyIds already against canonical KeyIndex;
                                                                // timestamps already parsed for snapshot columns;
                                                                // LogFileReference line numbers still placeholder (set in Stage C)
            std::vector<uint64_t>          local_line_offsets;  // file byte offset of each line.
                                                                // size() == lines.size()
            std::vector<std::string>       errors;              // parse + timestamp errors for this batch
            // NOTE: no first_line_number here. Stage C computes the prefix sum
            //       from lines.size() across batches it observes (Stage C is
            //       serial_in_order, so the running counter is race-free).
        };
        ```

        Workers **must not** touch shared mutable state other than the thread-safe canonical `KeyIndex`; all other thread-local state **must** be managed via `tbb::enumerable_thread_specific` or equivalent.

    - **Stage C — `tbb::filter_mode::serial_in_order` (appender / sink-flusher)**: `parallel_pipeline`'s `serial_in_order` guarantee means `ParsedBatch` tokens arrive in ascending `batch_index` — **no explicit reorder buffer is needed**. Stage C maintains a **single local `size_t mNextLineNumber = 1`** counter (Stage C is the only writer; serial_in_order makes the read/write sequence race-free). On every incoming `ParsedBatch`, Stage C **must**:
        1. Stamp absolute line numbers: `for (size_t i = 0; i < parsed.lines.size(); ++i) parsed.lines[i].FileReference().SetLineNumber(mNextLineNumber + i);` then advance `mNextLineNumber += parsed.lines.size()`. The first-line-number of this batch is captured as `first_line_number = mNextLineNumber - parsed.lines.size()` for forwarding to the sink.
        2. Call `LogData::AppendBatch(std::move(parsed.lines), std::move(parsed.local_line_offsets))` (req. 4.1.4), which:
            a. Concatenates `parsed.local_line_offsets` into the destination `LogFile::mLineOffsets` via a single `vector::insert` of a contiguous range — no per-line `push_back`, no per-line atomic. `LogFile::mLineOffsets` **should** be `reserve()`'d once at `BeginStreaming` time using a heuristic (`std::filesystem::file_size(path) / 100`) so the vector grows at most once or twice during the parse.
            b. Appends the lines to `LogData::mLines`.

        Stage C then computes the `new_keys` slice `canonical.KeyOf([prevSize, currentSize))` (req. 4.1.2/2a high-water-mark trick), tracks `prevSize` for the next batch, coalesces the `ParsedBatch` into the in-flight `StreamedBatch` (stamping `StreamedBatch::first_line_number` from the value captured in step 1) per req. 4.3.26, and (when the flush condition fires) hands the `StreamedBatch` to the sink. Errors and `new_keys` are accumulated and forwarded with the batch.

19. **simdjson padding contract at batch boundaries** (replaces the previous note in Technical Considerations): each Stage B worker **must** apply the existing per-line padding logic against `Batch::file_end`, not just a single global "last line" check:
    - For any line whose end position satisfies `file_end - line_end >= simdjson::SIMDJSON_PADDING`, the worker **may** call `parser.iterate(line.data(), line.size(), line.size() + (file_end - line_end))` directly (zero-copy fast path).
    - For any line whose end position is too close to `file_end`, the worker **must** fall back to `parser.iterate(simdjson::pad(linePadded.assign(line)))` exactly as the current `json_parser.cpp` does. `linePadded` is a thread-local scratch `std::string` to avoid per-line allocation.
    - Note that `bytes_end` (the batch boundary) is **irrelevant** for padding decisions — only `file_end` matters, because the next batch's bytes are still inside the same mmap and are valid memory to look ahead into. The fallback fires only at the actual end of the file.

20. Thread-local `ParseCache` hits may diverge between batches; this is acceptable (no cross-thread sharing). The shared canonical `KeyIndex` (req. 4.1.2/2a, req. 4.1.2/2b) **must** preserve the invariant that `LogData::Keys().SortedKeys()` is sorted and deduplicated, matching current behavior.

20a. **Thread-local scratch storage in Stage B** (mandatory optimization from §3.C of the design review): each worker **must** keep a `tbb::enumerable_thread_specific` set of reusable scratch buffers and reuse them across lines and across batches:
    - `std::vector<std::pair<KeyId, LogValue>> mScratchValues;` — destination for the line currently being parsed; `clear()` between lines so the backing capacity is preserved (`capacity()` only grows). `std::move`d into the `LogLine` when the line is complete; on the next line, the scratch is rebuilt fresh from a freshly-acquired empty vector via swap-with-default (or, if the storage layout permits, the `LogLine`'s vector is move-assigned back into the scratch as the empty husk it left behind). The exact mechanism is implementation-defined; the requirement is "no `std::vector` allocation per line in steady state".
    - `std::string mLinePadded;` — the existing simdjson padding fallback scratch (req. 4.2.19), continued to be reused per-worker.
    - `simdjson::ondemand::parser mParser;` — always per-worker, always reused.
    - `ParseCache mParseCache;` — per-worker, only allocated when `Options::useParseCache` is true (req. 4.1.15); otherwise omit.
    - `tsl::robin_map<std::string, KeyId> mKeyCache;` (with **transparent / `is_transparent` hashing** so `string_view` probes don't allocate, req. 4.1.2/2a) — per-worker, only allocated when `Options::useThreadLocalKeyCache` is true (req. 4.1.2/2b); otherwise omit and probe the canonical `KeyIndex` directly.
    - `std::vector<uint64_t> mLineOffsetsScratch;` — destination for `parsed.local_line_offsets` from Stage B step 1 (see req. 4.1.6a for the type choice); `clear()` between batches and `std::move`d into the `ParsedBatch`. Same reuse pattern as `mScratchValues`.
    - The PR description **must** report a Catch2 benchmark variant (`[benchmark][json_parser][allocations]`) using a heap-allocator-counting allocator to demonstrate that the per-line allocation count after warmup is bounded by a small constant (target: ≤ 1 per line for owned-string fallbacks; 0 in the all-`string_view` case).

21. **Per-batch timestamp parsing using a value-or-shared-ptr snapshot** (replaces the previous raw-pointer "snapshot" wording — that left a shutdown-crash window when the GUI destroyed the configuration before the parser thread finished). Timestamp parsing **must** happen inside Stage B so that lines streamed to the Qt model already have `TimeStamp`-typed values for the columns Stage B knows about. Concretely:
    - `JsonParserOptions::configuration` (req. 4.2.22) is a `std::shared_ptr<const LogConfiguration>` taken at the start of `ParseStreaming`. Stage B workers receive a `const LogConfiguration&` derived from the shared pointer; the shared pointer is captured by the parser thread for the duration of the parse, so the configuration cannot be destroyed out from under workers even if the GUI tears down its `LogConfigurationManager` during a parse. Cost: one `shared_ptr` copy at parse start; the configuration object itself is a small `std::vector<Column>` plus filters (typically < 1 KiB), so no copy avoidance optimization is needed.
    - The configuration object pointed to **must** be treated as immutable for the duration of the parse — `shared_ptr<const LogConfiguration>` enforces this at the type level. Callers that want to construct the snapshot from a mutable `LogConfigurationManager` **should** use `std::make_shared<const LogConfiguration>(manager.Configuration())` (a value copy).
    - For each `Batch`, Stage B walks `parsed.lines` and runs the same timestamp-parsing logic that `loglib::ParseTimestamps` runs today (`library/src/log_processing.cpp`), per `LogConfiguration::Type::time` column, including the `LastValidTimestampParse` fast-path optimization (which **must** be reset between batches — it is per-call state).
    - Timestamp-parse errors **must** be appended to `ParsedBatch::errors` so they reach the sink with the same flush guarantees as parse errors.
    - When `Options::configuration` is null (e.g. `LogTable`-less unit tests calling `Parse` directly), Stage B **must skip** timestamp parsing — `LogTable::Update` will continue to do the legacy whole-data `ParseTimestamps` pass, preserving the current `Parse(path) -> ParseResult` behavior for non-streaming callers.
    - This means `LogTable::Update` (`library/src/log_table.cpp:13`) **must** be updated: when called with already-streamed data (i.e. via the streaming path where Stage B already parsed timestamps), the redundant `ParseTimestamps` call **must** be skipped. The cleanest way is a `LogData::TimestampsAlreadyParsed() const -> bool` flag set by `AppendBatch` whenever Stage B has run, defaulting to `false` for the legacy `Parse` path.
    - **Configuration is locked while streaming is active** (resolves the dual-source-of-truth previously bridged by the back-fill in req. 4.1.13b). The GUI **must** disable the configuration editor (`MainWindow`'s configure-columns action / dialog) between `LogModel::BeginStreaming` and `LogModel::EndStreaming`. This eliminates the "user adds a time column mid-parse" branch entirely. The remaining mid-stream concern — *new keys appearing in later batches that the snapshot configuration could have promoted to a time column had it seen them* — is handled by the much smaller back-fill in req. 4.1.13b (which now only handles auto-promotion of newly-discovered keys, not user edits).
    - Stage B **must not** attempt to detect or react to configuration changes; the `shared_ptr<const>` makes this impossible by construction.

22. **`JsonParser::Options` struct (full specification — was previously implicit).** Add to `loglib/json_parser.hpp`:

    ```cpp
    namespace loglib {

    struct JsonParserOptions {
        // 0 = std::min(std::thread::hardware_concurrency(), kDefaultMaxThreads).
        // Set to 1 for benchmarks and the parity test; set to a fixed N for the
        // thread-scaling table required by req. 4.5.35.
        size_t threads = 0;
        static constexpr size_t kDefaultMaxThreads = 8;

        // Stage A target batch size in bytes (rounded up to next newline).
        size_t batchSizeBytes = 256 * 1024;

        // tbb::parallel_pipeline ntokens. 0 = 2 * effectiveThreads (req. 4.2.24).
        size_t ntokens = 0;

        // Snapshot LogConfiguration for Stage B per-batch timestamp parsing
        // (req. 4.2.21). Lifetime is owned by the shared_ptr, so the
        // configuration cannot be destroyed by the GUI while parsing is in
        // flight. Null for non-streaming/test callers; see req. 4.2.21.
        std::shared_ptr<const LogConfiguration> configuration;

        // Cooperative cancellation token (req. 4.2.22a, req. 4.3.30). When the
        // token is requested-stop, Stage A calls tbb::flow_control::stop() on
        // its next invocation. Default-constructed (no stop_source) means
        // cancellation is impossible for this call (used by blocking Parse).
        std::stop_token stopToken{};

        // Per-worker optimization toggles (see req. 4.1.2/2b, req. 4.1.15,
        // req. 4.2.20a). Defaults are conservative; the implementation PR
        // MUST report benchmarks both with and without each flag set so the
        // chosen defaults are data-driven (req. 4.5.35).
        bool useThreadLocalKeyCache = true;
        bool useParseCache          = true;
    };

    // The existing nested JsonParser::Options alias is kept as an alias for
    // JsonParserOptions so old call sites continue to compile.
    } // namespace loglib
    ```

    Default construction yields the recommended values for the GUI streaming path. The current implicit `Options::threads = 1` shorthand used in §2 (Goals) and §4.5 (benchmarks) refers to setting `threads = 1` in this struct.
22a. **`std::stop_token` plumbing** (replaces the previous hand-wave). The `stop_token` is consumed in two places only:
    - Stage A: between batches, calls `if (opts.stopToken.stop_requested()) { tbb::flow_control::stop(); return; }`. Stage A is the only TBB filter allowed to terminate the pipeline (an `tbb::filter_mode::serial_in_order` Stage C noticing the token can do nothing useful with it — it cannot abort upstream work that's still in flight).
    - `JsonParser::ParseStreaming` itself: at function entry, if `opts.stopToken.stop_requested()` is already true, return immediately without even setting up the pipeline.

    Stage B workers **must not** check the stop token — preempting a half-parsed line is unsafe (the `LogLine`'s `LogValue` `string_view`s may reference into the still-being-iterated simdjson document) and the latency win would be negligible. In-flight Stage B tokens at cancellation time **must** be allowed to complete and feed Stage C as normal; the bridging adapter's generation-id mechanism (req. 4.3.28) ensures the resulting `OnBatch` calls are dropped on the GUI thread.
22b. **Cancellation latency contract.** A `stop_source::request_stop()` followed by waiting for `OnFinished(true)` **must** complete in bounded time. The bound is:
    - **Best case:** one `Batch` worth of Stage A work (a `memchr` over ~256 KiB), then drain of `ntokens` in-flight Stage B / Stage C tokens. On a 16-thread, 16-core machine with default `ntokens = 32`, that drain is ≤ `32 × time_to_parse_one_batch ≈ a few tens of milliseconds`.
    - **Wasted work bound:** at most `ntokens × kBatchSizeBytes` bytes of input are parsed after `request_stop()` returns (ntokens batches already in flight + the just-emitted Stage A batch). With defaults that is ~8 MiB on a 16-thread machine — invisible to the user.
    - The PR description **should** include a measured `Clear()`-to-`OnFinished(true)` time on the 1 000 000-line benchmark.
22c. **Worker-count selection.** `effectiveThreads = (opts.threads != 0) ? opts.threads : std::min<size_t>(std::thread::hardware_concurrency(), JsonParserOptions::kDefaultMaxThreads)`. Worker count **must** be applied via `tbb::global_control(global_control::max_allowed_parallelism, effectiveThreads)` scoped (RAII) to the `Parse`/`ParseStreaming` call (Decision 5 — keep simple, no owned `tbb::task_arena`). `loglib` **must not** permanently configure the process-wide TBB arena.

23. Ordering (Decision 4 — strict): the output of `Parse` **must** contain lines in ascending original file line number. `parallel_pipeline`'s `serial_in_order` merger is sufficient to guarantee this because each batch contains consecutive lines; no post-hoc sort is permitted.

24. Back-pressure: the pipeline **must** bound memory by limiting the number of in-flight tokens via the `parallel_pipeline` `ntokens` argument (default `2 * effectiveThreads`, configurable via `Options::ntokens`). The reader filter is automatically stalled by TBB when the bound is hit, so RAM stays bounded on huge files. With the new `string_view` storage (req. 4.1.6) the per-`ParsedBatch` payload is materially smaller than in the earlier design (no per-value `std::string` copies), so peak in-flight RSS for the 2 GB benchmark is bounded by `ntokens × kBatchSizeBytes × ~1.5` (the 1.5 multiplier accounts for `LogLine` overhead beyond the source bytes the views point into).

### 4.3 Streaming Output Interface (answer 4B, 6C, 7C, 4-A)

25. The system **must** introduce a `StreamingLogSink` abstraction in `loglib`. **Errors and newly-introduced keys both ride with their batch** so the sink receives a single, ordered, atomic delivery per flush — no separate `OnKeysAppended` channel exists, eliminating any opportunity for the worker to deliver lines whose `KeyId`s reference keys the sink hasn't yet seen:

    ```cpp
    struct StreamedBatch {
        size_t                    first_line_number; // 1-based original file line number of lines.front()
        std::vector<LogLine>      lines;             // KeyIds against LogData's canonical KeyIndex
                                                     //   (no remap was needed; req. 4.1.2/2a);
                                                     // timestamp values already parsed for snapshot
                                                     //   columns (req. 4.2.21).
        std::vector<std::string>  new_keys;          // keys introduced since the prior flush, in
                                                     //   id-assignment order. Computed by Stage C
                                                     //   as canonical.KeyOf([prev_size, current_size))
                                                     //   (req. 4.1.2/2a high-water-mark); empty when
                                                     //   the batch introduced no new keys.
        std::vector<std::string>  errors;            // parse + timestamp errors for this batch's lines
    };

    class StreamingLogSink {
    public:
        virtual ~StreamingLogSink() = default;

        // Called exactly once before any OnBatch. No arguments: there is no
        // pre-pass for JSON logs (no header line to peek at), and the shared
        // canonical KeyIndex (req. 4.1.2/2) means initial keys would always be
        // empty. The earlier OnStarted(initialKeys) signature has been removed.
        virtual void OnStarted() = 0;

        // Called once per flush. The sink MUST process new_keys before lines
        // (the LogModel implementation does this internally). Successive
        // OnBatch calls are guaranteed to be in ascending first_line_number
        // because Stage C is serial_in_order.
        virtual void OnBatch(StreamedBatch batch) = 0;

        // Called exactly once when parsing completes (success OR cancelled).
        // Stage C MUST flush its in-flight StreamedBatch (if non-empty) via
        // one final OnBatch IMMEDIATELY before invoking OnFinished, so that
        // no buffered lines, errors, or new_keys are ever lost on shutdown.
        // `cancelled` is true if the pipeline stopped because of a stop_token
        // request (see req. 4.3.30).
        virtual void OnFinished(bool cancelled) = 0;
    };
    ```

    `JsonParser` **must** gain a new method `ParseStreaming(path, StreamingLogSink&, Options)` that returns when parsing is fully done (whether by completion or by cancellation). The existing `Parse(path) -> ParseResult` **must** continue to exist and **should** be implemented on top of the streaming path using an internal buffering sink (`BufferingSink`) that accumulates the same `LogData` + `errors` tuple the current API returns. `BufferingSink` is also the cleanest target for non-Qt unit tests of the pipeline.

    **Empty-lines-only batches are legal.** `StreamedBatch` may have `lines.empty() && !errors.empty()` — for example, a batch composed entirely of malformed JSON. Sinks **must** tolerate this and **must not** invoke any "insert N rows" UI primitive when N is zero (see req. 4.3.27 step 5 for the Qt model). Sinks may also see `lines.empty() && errors.empty() && new_keys.empty()` only as the optional final flush from `OnFinished` (req. 4.3.26a) — implementations **may** suppress empty final flushes, but consumers must tolerate them either way.

26. Flush policy (Decision 4): `OnBatch` **must** be invoked at most once per **1000 lines** OR once per **50 ms**, whichever comes first. Values **must** live in one place as named constants (`kStreamFlushLines`, `kStreamFlushInterval`). Flush granularity **must** preserve ordering across `OnBatch` calls (Stage C is `serial_in_order`, so this is automatic). The 50 ms timer **must** be driven by the Stage C thread itself — Stage C tracks a `std::chrono::steady_clock::time_point last_flush` and, on every incoming `ParsedBatch`, decides whether to coalesce into the in-flight `StreamedBatch` or flush it; if no `ParsedBatch` arrives for ≥ 50 ms, the next arrival flushes immediately. **No dedicated timer thread is created** — for the streaming-from-disk workload addressed by this PRD, batches arrive frequently enough that this is sufficient. (For the future tailing source, where batches may genuinely stop arriving, a follow-up item is captured in §5.)
26a. **Final flush before `OnFinished`** (mandatory — was previously implicit). When Stage C observes that the pipeline is shutting down (Stage A returned no further token, *or* `tbb::flow_control::stop()` was called), it **must** flush the in-flight `StreamedBatch` via one final `OnBatch` call before invoking `OnFinished(cancelled)`. This guarantees zero data loss across (a) the natural end of a finite file (last batch is typically partial) and (b) a cancelled parse where some Stage B tokens have already produced rows that should be visible if the user un-cancels and resumes. The bridging adapter (req. 4.3.28) treats this final `OnBatch` exactly like any other; the GUI's generation check (req. 4.3.30) is what drops it on a true cancellation.

27. The Qt `LogModel` **must** be updated to support incremental appends. Public additions:
    - `LogModel::BeginStreaming(std::unique_ptr<loglib::LogFile> file)` — sets up an empty table for the given `LogFile`. No `initialKeys` argument (mirrors `OnStarted()` in req. 4.3.25).
    - `LogModel::AppendBatch(loglib::StreamedBatch batch)` — observes `mLogTable.RowCount()` / `ColumnCount()` before and after `mLogTable.AppendBatch(std::move(batch))` and drives Qt's model updates from the deltas:
        1. Capture `oldRowCount = mLogTable.RowCount()` and `oldColumnCount = mLogTable.ColumnCount()`.
        2. Call `mLogTable.AppendBatch(std::move(batch))` (req. 4.1.13a), capturing the batch's `errors.size()` and `lines.size()` first because the batch is consumed by the call.
        3. Compute `newRowCount = mLogTable.RowCount()` and `newColumnCount = mLogTable.ColumnCount()`.
        4. **Column delta**: if `newColumnCount > oldColumnCount`, call `beginInsertColumns(QModelIndex(), oldColumnCount, newColumnCount - 1)` and `endInsertColumns()`.
        5. **Row delta**: if `newRowCount > oldRowCount`, call `beginInsertRows(QModelIndex(), oldRowCount, newRowCount - 1)` and `endInsertRows()`. **The `newRowCount == oldRowCount` case is mandatory and must skip the row-insert pair entirely** — `beginInsertRows` with `last < first` is a Qt assertion failure (and on release builds, undefined). The empty-rows case occurs naturally when a batch contains only errors (`StreamedBatch{ lines.empty(), !errors.empty() }`, called out in req. 4.3.25).
        6. **Back-fill notify**: if `LogTable::AppendBatch` performed a timestamp back-fill (req. 4.1.13b), it sets a flag observable via `mLogTable.LastBackfillRange()` returning `std::optional<std::pair<size_t, size_t>>` (column range); when present, `LogModel::AppendBatch` emits `dataChanged(index(0, firstCol), index(newRowCount-1, lastCol), {Qt::DisplayRole, SortRole})`.
        7. Update internal `mLineCount = newRowCount` and `mErrorCount += capturedErrorCount`; emit `lineCountChanged(qsizetype)` and (when `capturedErrorCount > 0`) `errorCountChanged(qsizetype)`.
        - `LogModel` **must not** touch any status bar widget directly — that layering belongs to `MainWindow`.
    - **No standalone `ExtendKeys`** in the public API. Column extension is a side effect of `AppendBatch` (driven by `batch.new_keys`), so callers cannot accidentally insert columns without inserting the rows that depend on them.
    - `LogModel::EndStreaming(bool cancelled)` — emits a `streamingFinished(bool cancelled)` signal so `MainWindow` can flip its status bar, re-enable the configuration UI (req. 4.3.29), and show the final error summary. **Does not** call `ParseTimestamps` — Stage B and `LogTable::AppendBatch` have already done all timestamp parsing (req. 4.2.21, req. 4.1.13b).
    - **New Qt signals on `LogModel`**:
        - `errorCountChanged(qsizetype count)` — emitted after every `AppendBatch` whose `batch.errors` was non-empty. `MainWindow` connects this to its status-bar label.
        - `lineCountChanged(qsizetype count)` — emitted after every `AppendBatch`, for status-bar progress display.
        - `streamingFinished(bool cancelled)` — emitted from `EndStreaming`.
27a. **Column extension uses `beginInsertColumns` (mandatory).** Because `LogTable::AppendBatch` is contractually append-only for columns (req. 4.1.13), `LogModel::AppendBatch` **must** drive column growth via `beginInsertColumns` / `endInsertColumns` — never `beginResetModel`. The fallback-to-reset escape hatch from earlier drafts is removed: with the append-only contract, incremental column insertion is the simple, correct path. If implementation difficulties arise (e.g. proxy models misbehaving), they **must** be fixed in the implementation PR rather than papered over with a model reset, because a reset invalidates selection, scroll position, sort, and filter state, which is precisely what streaming was meant to preserve.

28. **Thread-bridging adapter with a generation id and owned `std::stop_source`** (replaces previous "use Qt::QueuedConnection" hand-wave). A `QObject`-derived sink `QtStreamingLogSink` lives on the GUI thread, owns the lifecycle state for one or more sequential parses, and bridges TBB-thread callbacks into the Qt event loop. Concretely:

    ```cpp
    class QtStreamingLogSink : public QObject, public loglib::StreamingLogSink {
        Q_OBJECT
    public:
        explicit QtStreamingLogSink(LogModel* model, QObject* parent = nullptr);

        // Called from the GUI thread by LogModel::BeginStreaming. Resets the
        // stop_source, bumps the generation, and remembers the LogModel as a
        // QPointer so out-of-order destruction is detected. Returns the
        // stop_token to be installed on JsonParserOptions::stopToken.
        std::stop_token BeginParse();

        // Called from the GUI thread by LogModel::Clear() to cancel the
        // in-flight parse. Bumps the generation FIRST (so any queued events
        // already in the GUI event loop are dropped), then requests stop on
        // the source. Non-blocking — does NOT wait for OnFinished.
        void RequestStop();

        // StreamingLogSink interface — called from a TBB worker thread.
        // Each implementation captures the current generation, then posts the
        // call to the GUI thread via QMetaObject::invokeMethod(...,
        // Qt::QueuedConnection). The GUI-thread receiver re-checks the
        // generation against mGeneration and drops the call on mismatch.
        void OnStarted() override;
        void OnBatch(loglib::StreamedBatch batch) override;
        void OnFinished(bool cancelled) override;

    private:
        QPointer<LogModel>           mModel;
        std::atomic<uint64_t>        mGeneration{0};
        std::optional<std::stop_source> mStopSource;  // one per parse, replaced by BeginParse()
    };
    ```

    Required behavior:
    - `BeginParse()` **must** be called on the GUI thread, **must** atomically `++mGeneration`, replace `mStopSource` with a fresh `std::stop_source`, and return `mStopSource->get_token()`. The token is then placed on `JsonParserOptions::stopToken` (req. 4.2.22) before `JsonParser::ParseStreaming` is invoked from a background QThread/QtConcurrent::run.
    - `RequestStop()` **must** be called on the GUI thread, **must** atomically `++mGeneration` *before* `mStopSource->request_stop()` so that any `OnBatch` already queued by a worker is dropped on receipt. After `request_stop`, the worker may still produce up to `ntokens` more batches (req. 4.2.22b), all of which the generation check drops.
    - The TBB-thread `OnBatch`/`OnStarted`/`OnFinished` overrides **must** stamp the queued call with the generation captured at queue time. The GUI-thread receiver compares against the live `mGeneration.load(std::memory_order_acquire)` and silently returns on mismatch.
    - `mModel` is held as `QPointer<LogModel>`; if it ever becomes null (out-of-order destruction), every queued call **must** be a no-op.
    - The adapter **must not** itself own the parse thread — that is `MainWindow`'s responsibility (e.g. via `QtConcurrent::run` returning a `QFuture<void>` it tracks). The adapter only owns the cancellation-and-bridging state.

29. `MainWindow::OpenFilesWithParser` / `OpenFileInternal` **must** be updated to use the streaming path when the chosen parser supports it (for now: `JsonParser`). Behavior:
    - Construct (or reuse) a `QtStreamingLogSink` bound to `mModel`; call `LogModel::BeginStreaming(...)` (which calls `sink->BeginParse()` and remembers the returned `stop_token`).
    - Build the configuration snapshot as `auto cfg = std::make_shared<const LogConfiguration>(mModel->Configuration().Configuration());` (a value copy — see req. 4.2.21).
    - Launch `JsonParser::ParseStreaming(path, *sink, options)` on a background thread (e.g. `QtConcurrent::run`), passing `options.stopToken = sinkStopToken` and `options.configuration = cfg`.
    - **Disable configuration-editing UI while streaming is active** (req. 4.2.21). Specifically: disable the "Configure columns" menu/toolbar action, disable the column header context menu's edit entries, and disable any inline header-editing affordances. Re-enable them all from the `streamingFinished` signal. Rationale: the parser thread holds an immutable snapshot, so any user edit during streaming would either silently affect only post-streaming rows (confusing) or trigger an expensive whole-data re-parse (defeats streaming).
    - Show a modeless progress indicator (non-blocking — e.g. a status bar label with "Parsing… N lines, M errors") while streaming is active. The label **must** be updated by connecting to the new `LogModel` signals (`lineCountChanged`, `errorCountChanged`, `streamingFinished`) — `MainWindow` is the only place that owns the status bar.
    - Append rows to the table as batches arrive (handled by `LogModel::AppendBatch`).
    - Keep the current post-parse `QMessageBox` error summary, triggered from `streamingFinished(false)`. On `streamingFinished(true)` (cancelled), no summary is shown.
    - Keep the current UX of clearing the model before a new file is opened (which triggers `LogModel::Clear()` → `sink->RequestStop()` per req. 4.3.30).

30. `LogModel::Clear()` **must** also cancel any in-flight streaming parse gracefully:
    - `Clear()` **must** call `mSink->RequestStop()` (req. 4.3.28) which atomically bumps the generation and requests stop on the owned `std::stop_source`. Stage A picks up the stop request on its next batch (req. 4.2.22a); cancellation latency is bounded per req. 4.2.22b.
    - `Clear()` **must not** block waiting for the parser thread to join — the parser is allowed to keep running for a brief moment as it walks out of `parallel_pipeline`. The combination of `request_stop` + generation bump makes this safe: the in-flight `OnBatch` calls already queued in the GUI event loop are dropped because their generation no longer matches.
    - `OnFinished(true)` will eventually fire on the GUI thread; the adapter drops it via the generation check, so the cancelled stream is fully invisible to the user.
    - `Clear()` **must** then proceed to reset `mLogTable` as it does today (`beginResetModel` / `endResetModel` are appropriate here because the model is genuinely going to be empty afterwards).

### 4.4 Interface Preparation for Live / Real-Time Sources (answer 6B — interface only)

31. The line-producing side of the pipeline **must** be abstracted behind a minimal interface (e.g. `ILineSource`) with at least:
    - `std::optional<std::string_view> NextLine()` (or a batch equivalent).
    - `bool IsEof() const`.
    - An optional signal for "no more data will ever come" vs. "no data right now, try again".
32. Two implementations **must** exist:
    - `MmapFileLineSource` — the fast path used by `Parse(path)`. This is the only one that ships in this feature.
    - (Future, out of scope — see section 5) a `TailingFileLineSource` that blocks/waits for new bytes.
33. The interface **must not** leak into the public `loglib` headers if it is purely internal. It can live in a private header under `library/src/`.

### 4.5 Benchmarking & Performance Reporting

34. `test/lib/src/benchmark_json.cpp` **must** be extended:
    - Keep the existing 10 000-line benchmark.
    - Add a "Parse 1 000 000 JSON log entries" benchmark (~170 MB) gated behind a Catch2 tag (`[benchmark][json_parser][large]`) so CI can opt-in/out based on runner size.
    - Add a variant that forces single-threaded execution (via `JsonParser::Options::threads = 1`) gated behind tag `[benchmark][json_parser][single_thread]`, so the single-thread effect of key indexing is tracked separately from the parallel speedup.
    - Report both **throughput (MB/s of input parsed)** and **lines/s** in benchmark output.
35. The PR description **must** include:
    - Before/after numbers for the 10 000-line benchmark.
    - Before/after numbers for the 1 000 000-line benchmark.
    - Before/after numbers for the single-thread variant.
    - A small thread-scaling table (1 / 2 / 4 / 8 / hw_concurrency workers) on the 1 000 000-line benchmark.
    - The `RESULTS.md` content from `C:\code\pipeline_spike\RESULTS.md` (or a link if accessible to reviewers), so the oneTBB choice rationale is captured in the PR.
36. No public API test (unit test) may be removed except the ones that assert on the old `LogMap` type directly. Those **must** be rewritten to assert equivalent behavior against the new indexed storage.

### 4.6 Build / Dependencies

37. Add oneTBB via `FetchContent` in `cmake/FetchDependencies.cmake`, mirroring the existing dependency pattern:

    ```cmake
    if(NOT USE_SYSTEM_TBB)
        FetchContent_Declare(
            tbb
            GIT_REPOSITORY https://github.com/uxlfoundation/oneTBB.git
            GIT_TAG        v2022.3.0   # latest stable as of 2026-04; bump as newer releases land
            SYSTEM
            EXCLUDE_FROM_ALL
        )
        block()
            set(TBB_TEST OFF)
            set(TBB_EXAMPLES OFF)
            set(TBB_STRICT OFF)        # avoid failing the build on oneTBB's own compiler warnings
            FetchContent_MakeAvailable(tbb)
        endblock()
    else()
        # 2021.5.0 is the first release that ships the oneAPI-style tbb::filter_mode /
        # tbb::parallel_pipeline API we depend on. Older legacy-API system packages
        # (TBB 2020.x and earlier) will fail at use sites, not at find_package, so the
        # version pin here is what makes the failure mode early and obvious.
        find_package(TBB 2021.5 REQUIRED)
    endif()
    ```

    Add a matching `option(USE_SYSTEM_TBB "Use system uxlfoundation oneTBB library" OFF)` alongside the other `USE_SYSTEM_*` options. Link `loglib` with `TBB::tbb`.
38. On Windows the `structured_log_viewer` executable **must** ship `tbb12.dll` next to it so it can be run both from the build tree and after install. Add a `POST_BUILD` copy step in `app/CMakeLists.txt` (modeled on the existing `tzdata` copy in `cmake/FetchDependencies.cmake`) **and** an `install(FILES ...)` rule that puts the DLL next to the installed executable:

    ```cmake
    if(WIN32)
        add_custom_command(TARGET structured_log_viewer POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:TBB::tbb>
                $<TARGET_FILE_DIR:structured_log_viewer>)

        # Install the runtime DLL alongside the installed binary. The destination
        # MUST match the existing `install(TARGETS structured_log_viewer ...)` rule.
        install(FILES $<TARGET_FILE:TBB::tbb>
                DESTINATION ${CMAKE_INSTALL_BINDIR})
    endif()
    ```

    On non-Windows platforms `TBB::tbb` is resolved through the system loader (`rpath` for the FetchContent build, distro paths for `USE_SYSTEM_TBB=ON`); no copy step is needed.
39. CI (`.github/workflows/build.yml`) **must** continue to pass on all matrix entries. The Windows job **must** run the installed binary (not just the build-tree one) so a missing `tbb12.dll` is caught. Linux/macOS runners that already have a system `libtbb` ≥ 2021.5 **may** opt in to `USE_SYSTEM_TBB=ON` to keep build times low.
40. The new dependency **must** be documented in `README.md` alongside existing FetchContent dependencies, including the Windows runtime-copy note and the `USE_SYSTEM_TBB` minimum version.
41. Continue to require C++23 (already in `CMakeLists.txt`). Do not lower the standard. oneTBB v2022.3.0 supports C++17+ and works under C++23 cleanly.

## 5. Non-Goals (Out of Scope)

- **Live tailing / real-time log following.** Only the *interface* (`ILineSource`) is prepared in this PRD. A working `TailingFileLineSource` with filesystem watching, log rotation handling, truncation detection, etc. is explicitly deferred to a follow-up feature (Decision 6B). The flush-timer simplification in req. 4.3.26 (Stage C drives the 50 ms flush from incoming batches, no dedicated timer thread) is **only** safe for the file-streaming workload — the tailing follow-up will need a real timer (e.g. a Qt `QTimer` on the GUI side, or a dedicated `tbb::task_arena` enqueue) to flush partial batches when the producer goes silent.
- **Parallelizing non-JSON parsers.** Only `JsonParser` is optimized in this feature. Any future `LogParser` can opt into the new streaming path by adopting the same sink interface later.
- **Incremental filtering/sorting while streaming.** The Qt filter model and sort state only need to behave correctly **after** streaming completes. Fancy partial-sort behavior is out of scope.
- **GPU / SIMD-beyond-simdjson optimizations.** We rely on `simdjson` for the parse-level SIMD.
- **String interning / cross-line de-duplication of value bytes.** This PRD already lands the `std::string_view`-into-mmap optimization (req. 4.1.6, req. 4.1.15a) which captures the bulk of the available win. A separate string-interning step (one-`std::string` per unique value across all lines) is a possible future win for files with many repeated values (e.g. component names, levels) but is out of scope here.
- **Small-string-optimization-tuned `LogValue` variant size reduction.** The variant size is dictated by `std::string` (24 bytes on most stdlibs); shrinking it would require a dedicated SSO type. Out of scope.
- **Network / remote log sources.**
- **Changing the on-disk log file format** or adding non-JSON input formats.

## 6. Design Considerations

### 6.1 Data Flow (text sketch — `tbb::parallel_pipeline`)

```
  ┌────────────────┐    ┌──────────────────────────────────────────┐
  │ LogFile        │───▶│ Stage A (serial_in_order, ~1 thread)     │
  │ owns mmap      │    │  - finds BATCH BOUNDARIES only:          │
  │ + posix_madvise│    │      advance ≥ kBatchSizeBytes,          │
  │   SEQUENTIAL   │    │      then to next '\n'                   │
  │  (req. 4.1.6b) │    │  - assigns batch_index ONLY              │
  │                │    │  - DOES NOT assign first_line_number     │
  │                │    │    (Stage C does, via prefix sum;        │
  │                │    │     no Stage C → Stage A feedback —      │
  │                │    │     that would deadlock the pipeline)    │
  └────────────────┘    │  - checks stop_token; calls              │
                        │    flow_control::stop() on request       │
                        │    (req. 4.2.22a)                        │
                        │  - emits Batch token                     │
                        │    {bytes_begin, bytes_end, file_end}    │
                        │    NO per-line offsets, NO line_refs,    │
                        │    NO first_line_number                  │
                        └─────────────────┬────────────────────────┘
                                          │ token flow (bounded by ntokens)
                                          ▼
                          ┌──────────────────────────────────────────┐
                          │ Stage B (parallel) (×N workers)          │
                          │  - simdjson::ondemand (per-thread,       │
                          │    enumerable_thread_specific)           │
                          │  - per-thread ParseCache (KeyId-keyed,   │
                          │    OPT-IN via Options.useParseCache)     │
                          │  - per-thread KeyId cache (string_view→  │
                          │    KeyId, OPT-IN via Options             │
                          │    .useThreadLocalKeyCache, req.4.1.2/2b)│
                          │    — feeds the SHARED canonical KeyIndex │
                          │    on miss                               │
                          │  - per-thread scratch vectors (line      │
                          │    values, line offsets uint64_t,        │
                          │    padding buffer) reused across lines   │
                          │    (req. 4.2.20a)                        │
                          │  - per-line padding fallback uses        │
                          │    file_end only (req. 4.2.19)           │
                          │  - LogValue prefers std::string_view     │
                          │    into mmap (req. 4.1.6, req. 4.1.15a)  │
                          │  - field.key() → string_view (req.       │
                          │    4.1.15b)                              │
                          │  - runs ParseTimestamps for this batch   │
                          │    using shared_ptr<const LogConfig>     │
                          │    snapshot (req. 4.2.21)                │
                          │  - builds local_line_offsets vector      │
                          │  - emits per-line scratch SORTED by KeyId│
                          │    (one std::sort per line, req. 4.1.5)  │
                          │  - emits ParsedBatch token (no           │
                          │    first_line_number — Stage C stamps it)│
                          └──────────────────┬───────────────────────┘
                                             │ token flow
                                             ▼
                          ┌──────────────────────────────────────────┐
                          │ Stage C (serial_in_order, ~1 thread)     │
                          │  - TBB delivers tokens in batch_index    │
                          │    order AUTOMATICALLY                   │
                          │  - stamps absolute line numbers via      │
                          │    local mNextLineNumber prefix sum,     │
                          │    sets each LogLine's LogFileReference  │
                          │    line number                           │
                          │  - LogData::AppendBatch (req. 4.1.4):    │
                          │      concatenate local_line_offsets into │
                          │        LogFile::mLineOffsets             │
                          │        (reserved at BeginStreaming),     │
                          │      append LogLines to mLines.          │
                          │      NO KeyId remap, NO rewire           │
                          │      (workers used canonical index)      │
                          │  - computes new_keys via canonical       │
                          │    high-water-mark slice (req. 4.1.2/2a) │
                          │  - coalesces into in-flight StreamedBatch│
                          │    until kStreamFlushLines or            │
                          │    kStreamFlushInterval (req. 4.3.26)    │
                          │  - on shutdown OR cancel: ALWAYS flushes │
                          │    in-flight StreamedBatch first         │
                          │    (req. 4.3.26a)                        │
                          │  - flushes StreamedBatch to sink         │
                          │    (lines + new_keys + errors atomic)    │
                          └──────────────────┬───────────────────────┘
                                             │
                                             ▼
                          ┌──────────────────────────────────────────┐
                          │ StreamingLogSink                         │
                          │  - QtStreamingLogSink (req. 4.3.28):     │
                          │      owns std::stop_source for parse,    │
                          │      stamps queued calls with generation,│
                          │      drops stale calls on GUI thread     │
                          │      → LogTable::AppendBatch -> void     │
                          │        (back-fills timestamps if a new   │
                          │        time column appeared, req. 4.1.13b│
                          │        — only the auto-promotion case;   │
                          │        config UI is gated during stream) │
                          │      → LogModel observes RowCount/       │
                          │        ColumnCount deltas, drives        │
                          │        beginInsertColumns/Rows           │
                          │        (skips when delta == 0 — empty-   │
                          │        rows batches are legal)           │
                          │  - BufferingSink: blocking Parse(path)   │
                          └──────────────────────────────────────────┘
```

### 6.2 Indexed storage (illustrative)

```cpp
// loglib/key_index.hpp  (new public header)
namespace loglib {

using KeyId = uint32_t;
constexpr KeyId kInvalidKeyId = std::numeric_limits<KeyId>::max();

// THREAD-SAFE, append-only for the lifetime of a parse (req. 4.1.2/2a).
// All Stage B workers share a single instance owned by LogData.
// All probing methods take std::string_view directly so the hot path
// does not allocate a std::string per probe.
class KeyIndex {
public:
    KeyId            GetOrInsert(std::string_view key);  // safe to call concurrently
    KeyId            Find(std::string_view key) const;   // kInvalidKeyId if absent
    std::string_view KeyOf(KeyId id) const;              // pointer-stable for parse lifetime
    size_t           Size() const;                       // for high-water-mark slicing
    std::vector<std::string> SortedKeys() const;         // for LogData::Keys()
private:
    // Recommended (req. 4.1.2/2a): forward lookup via tbb::concurrent_hash_map
    // with TRANSPARENT (string_view) hashing, reverse storage via
    // std::deque<std::string> for pointer stability under concurrent inserts.
    // shared_mutex + tsl::robin_map (with transparent hashing) is acceptable.
    /* concurrent_hash_map<std::string, KeyId, TransparentHashCompare> mLookup; */
    /* std::deque<std::string>                                         mKeys;   */
};

} // namespace loglib

// loglib/log_line.hpp  (refactored)
namespace loglib {

// LogValue gains std::string_view as the FIRST alternative (req. 4.1.6).
// Variant tag stability matters for serialization tests, hence the explicit
// alternative order.
using LogValue = std::variant<
    std::string_view, std::string,
    int64_t, uint64_t, double, bool, TimeStamp, std::monostate>;

std::optional<std::string_view> AsStringView(const LogValue&);
bool                            HoldsString(const LogValue&);

// Lifetime-promotion helper (req. 4.1.6): if v holds string_view, returns a
// LogValue holding an OWNED std::string copy; otherwise returns v unchanged.
// Use whenever a LogValue must outlive its source LogFile.
LogValue                        ToOwnedLogValue(const LogValue&);

class LogLine {
public:
    // KeyIndex reference is mandatory. Stage B constructs lines DIRECTLY
    // against the canonical KeyIndex (no rewire ever happens in the streaming
    // path — req. 4.1.9). Cross-LogData Merge still rewires.
    LogLine(std::vector<std::pair<KeyId, LogValue>> sortedValues,
            const KeyIndex&  keys,
            LogFileReference fileReference);

    // ---- Slow / compatibility accessors (unchanged signatures) ----
    LogValue                 GetValue(const std::string& key) const;
    void                     SetValue(const std::string& key, LogValue value);
    std::vector<std::string> GetKeys() const;
    const LogFileReference&  FileReference() const;

    // ---- Fast / indexed accessors (additive) ----
    LogValue GetValue(KeyId id) const;             // implementation-defined lookup
                                                   //   (linear scan recommended for n<32; req. 4.1.8)
    void     SetValue(KeyId id, LogValue value);   // sorted insert/update via std::lower_bound

    // Concrete std::span return (req. 4.1.8) — pins the storage layout to a
    // contiguous sorted vector and enables zero-overhead bulk iteration.
    std::span<const std::pair<KeyId, LogValue>> IndexedValues() const;

    // Only used by LogData::Merge for cross-LogData merges (req. 4.1.9).
    void RebindKeys(const KeyIndex& keys);

private:
    std::vector<std::pair<KeyId, LogValue>> mValues;        // sorted by KeyId
    const KeyIndex*                         mKeys = nullptr; // canonical index
    LogFileReference                        mFileReference;  // NOT const (req. 4.1.9a)
};

} // namespace loglib

// loglib/log_table.hpp  (UNCHANGED public API — no KeyId leaks, no Qt leaks)
//   - GetHeader(size_t column)
//   - GetValue(size_t row, size_t column)
//   - GetFormattedValue(size_t row, size_t column)
//   - ColumnCount() / RowCount()
//   - NEW for streaming: void AppendBatch(StreamedBatch) (req. 4.1.13a).
//     Append-only column extension; back-fills timestamps for newly-discovered
//     time columns over already-appended rows (req. 4.1.13b). Returns void —
//     LogModel::AppendBatch reads RowCount()/ColumnCount() before & after to
//     drive Qt's beginInsertRows/Columns deltas (req. 4.3.27).
//   - NEW: std::optional<std::pair<size_t, size_t>> LastBackfillRange() const
//     — column range affected by the most recent AppendBatch's back-fill, if
//     any. LogModel uses this to emit dataChanged after AppendBatch.
// Internally caches column→KeyId and dispatches to LogLine::GetValue(KeyId).
```

### 6.3 Qt integration

- `LogModel::AppendBatch` must call `beginInsertRows(QModelIndex(), firstRow, lastRow)` and `endInsertRows()` exactly — no `beginResetModel/endResetModel` during streaming, because that invalidates selection, sort, scroll position, etc.
- When a batch introduces a new column key (`StreamedBatch::new_keys` non-empty, req. 4.3.25), `LogTable::AppendBatch` extends columns at the **end** only (req. 4.1.13). `LogModel::AppendBatch` then drives `beginInsertColumns(QModelIndex(), firstNew, lastNew)` / `endInsertColumns()` (req. 4.3.27a — no `beginResetModel` fallback). Steady-state batches (keys already known) emit no column-change at all.
- When a newly-discovered column has type `Type::time`, `LogTable::AppendBatch` runs the timestamp back-fill from req. 4.1.13b synchronously over `LogData.Lines()` (already-appended + this batch); `LogModel::AppendBatch` follows up with `dataChanged(...)` so the affected cells refresh without invalidating selection/scroll state.
- **Layering**: `LogModel` exposes `lineCountChanged(qsizetype)`, `errorCountChanged(qsizetype)`, and `streamingFinished(bool cancelled)` Qt signals (req. 4.3.27). `MainWindow` is the only owner of the status bar and is the only place that subscribes to these signals to render `"Parsing file.log — 12 345 678 lines, 3 errors"`. `LogModel` itself **must not** know that a status bar exists.
- Progress indication is a plain `QStatusBar` label rendered by `MainWindow`. No modal dialog, no progress bar percentage (we don't know total lines cheaply).
- The Qt main-thread bridging adapter (`QtStreamingLogSink`, req. 4.3.28) owns the per-parse `std::stop_source`, holds a generation id that is bumped on every `LogModel::Clear()`/`BeginParse()`, and drops queued `OnBatch` calls from cancelled or stale streams.

## 7. Technical Considerations

- **mmap ownership + ondemand**: `mio::mmap_source` is now owned by `LogFile` (req. 4.1.6a) so it stays alive for the lifetime of `LogData`. Stage A and Stage B both read directly from this mmap; the `std::string_view` alternative of `LogValue` (req. 4.1.6) points into it. Immediately after the mmap is opened, `loglib::HintSequential(...)` advises the kernel that access is sequential (`POSIX_MADV_SEQUENTIAL` on POSIX, `PrefetchVirtualMemory` on Windows — req. 4.1.6b). The simdjson padding contract at batch boundaries is fully specified in req. 4.2.19 — each worker uses `Batch::file_end` (not the batch boundary) to decide between the zero-copy `parser.iterate(line.data(), line.size(), capacity)` fast path and the `simdjson::pad(linePadded.assign(line))` fallback.
- **Thread safety of `LogFile`**: `LogFile::mLineOffsets` is **only mutated by Stage C** via `LogData::AppendBatch`, which appends a contiguous range of offsets per batch under the implicit `serial_in_order` lock of TBB. Stage A and Stage B never touch `mLineOffsets`; Stage B builds its own `local_line_offsets` vector (typed as `std::vector<uint64_t>` per req. 4.1.6a / req. 4.2.18) that Stage C concatenates in. `mLineOffsets` is `reserve()`'d once at `BeginStreaming` (req. 4.1.6a) so the per-batch concatenation is amortized O(1). `LogFileReference` construction in Stage B uses a placeholder line number (since Stage A no longer stamps `first_line_number` — req. 4.2.18); Stage C overwrites the line number in the same pass that calls `AppendBatch`, using a serial prefix-sum counter that is race-free by `serial_in_order` semantics. `LogFile` itself **must** remain pointer-stable for the lifetime of any `LogFileReference` (the `std::unique_ptr<LogFile>` ownership in `LogData::mFiles` provides this).
- **`KeyIndex` thread-safety contract**: workers share `LogData::Keys()` directly. The contract (req. 4.1.2/2a) requires `GetOrInsert`/`Find`/`KeyOf` to be safe to call concurrently, requires `KeyOf`'s returned `string_view` to remain valid under concurrent inserts (i.e. pointer-stable reverse storage — `std::deque<std::string>`, `tbb::concurrent_vector`, or chunked arena), and requires the forward-lookup container to support transparent (heterogeneous) `string_view` lookup so probing does not allocate. The optional per-worker key cache (req. 4.1.2/2b, gated by `JsonParserOptions::useThreadLocalKeyCache`, default on) absorbs the steady-state hot path, so the canonical index is touched only on the rare new-key path; the implementation must benchmark with the cache disabled to validate the default.
- **simdjson parser reuse**: `simdjson::ondemand::parser` keeps an internal buffer and benefits from reuse. Each TBB worker must keep its own instance via `tbb::enumerable_thread_specific<simdjson::ondemand::parser>` (preferred) or plain `thread_local`. **Never share** one parser across threads.
- **TBB arena scoping**: do not rely on the implicit global TBB arena surviving across `loglib` calls. Scope worker-count limits with `tbb::global_control` inside each `Parse`/`ParseStreaming` call so a single misconfigured caller cannot affect the rest of the process (req. 4.2.22c).
- **Windows runtime**: shipping `tbb12.dll` next to the binary is a mandatory part of this feature (see req. 4.6.38). Both the `POST_BUILD` copy and the `install(FILES ...)` rule **must** be in place before the PR merges; CI **must** run the installed binary on Windows to catch a missing DLL (req. 4.6.39).
- **Cancellation**: `std::stop_token` is plumbed via `JsonParserOptions::stopToken` (req. 4.2.22) and consumed only by Stage A and at `ParseStreaming` entry (req. 4.2.22a). The owning `std::stop_source` lives on `QtStreamingLogSink` (req. 4.3.28); `LogModel::Clear()` calls `mSink->RequestStop()`, which atomically bumps the bridging adapter's generation id *first* and then calls `request_stop()` on the source. Cancellation latency is bounded per req. 4.2.22b (best case ~one batch round-trip; wasted work bounded by `ntokens × kBatchSizeBytes`). The Qt-side race (queued events outliving the cancellation) is handled by the generation-id check, not by blocking the GUI thread.
- **Mid-stream timestamp correctness**: Stage B parses timestamps using a `std::shared_ptr<const LogConfiguration>` snapshot taken at `BeginStreaming` time (req. 4.2.21). The shared pointer is held by the parser thread for the duration of the parse, so the configuration cannot be destroyed by the GUI mid-parse — eliminating the shutdown-crash window that the previous raw-pointer design had. The configuration UI is disabled between `BeginStreaming` and `EndStreaming` (req. 4.3.29), so the only mid-stream change that needs handling is auto-promotion of newly-discovered keys to time columns; that is back-filled synchronously by `LogTable::AppendBatch` on the GUI thread (req. 4.1.13b).
- **Determinism in tests**: the parallel path must produce equivalent `LogData` (same lines, same order, same keys, same parsed `TimeStamp` values) as the single-threaded path (`JsonParserOptions::threads = 1`) for the same input. The parity test in `test_json_parser.cpp` (req. 4.1.16) **must** assert this on several fixture files using the `LogValueEquivalent` helper (which treats `std::string` and `std::string_view` of equal byte content as equivalent). The same parity must hold whether or not `Options::configuration` is provided (S7).
- **UB / races**: `ParsedBatch` values must be move-constructed/moved between TBB filters — no shared mutable references to `LogFile` or `LogData` from Stage B beyond the canonical `KeyIndex` (whose thread-safety contract is documented above). The `LogFile&` referenced from each `LogFileReference` is read-only from Stage B's perspective. The mmap's `string_view`s held by `LogValue` are bytes-only reads with no concurrent mutation.
- **`LogValue` lifetime**: `LogValue::std::string_view` alternatives carry pointers into the `LogFile`'s mmap. They are valid as long as the `LogFile` is alive — which means as long as the owning `LogData` is alive. Tests **must** cover the lifetime contract: a `LogLine` outliving its `LogData` is undefined behavior, but the existing ownership pattern (`LogData` owns `std::vector<std::unique_ptr<LogFile>>`, `LogTable` owns `LogData`, `LogModel` owns `LogTable`) makes this an "uphill" mistake for callers.
- **Pre-commit / clang-format**: all new files must pass existing `.pre-commit-config.yaml` hooks. No new warnings under project `CompilerWarnings.cmake` (note: oneTBB's own headers must be included via `SYSTEM` so their warnings don't pollute the project; the `FetchContent` block above sets this correctly).

## 8. Success Metrics

The headline success criterion is "the optimized parser is meaningfully faster than `main` and the GUI feels non-blocking on big files". Hard numerical thresholds are intentionally avoided because they depend on machine, file shape, and TBB pool warm-up cost — the rule instead is **"report the numbers in the PR so a reviewer can judge"**.

| # | Metric | Target |
|---|---|---|
| S1 | Wall-clock change on `benchmark_json.cpp` 10 000-line case | Faster than `main` baseline; before/after numbers reported in PR |
| S2 | Wall-clock change on new 1 000 000-line large-file benchmark | Substantially faster than `main` baseline (multi-core); before/after numbers + thread-scaling table reported in PR |
| S3 | Single-thread efficiency on 1 000 000-line benchmark with `JsonParser::Options::threads = 1` | No regression vs. `main`; before/after number reported in PR |
| S4 | Time-to-first-row in GUI when opening a 500 MB JSON log | Feels non-blocking on author's machine; observed value noted in PR |
| S5 | UI frame time during streaming parse of 2 GB file | No perceptible freeze in manual testing; any hitch noted in PR |
| S6 | Peak RSS during parse of 2 GB file | Bounded by mmap + `2 * worker_count * batch_size` in-flight |
| S7 | Final `LogData` parity vs. serial path on 10 fixture files | 100 % match under `LogValueEquivalent` (req. 4.1.16, asserted by parity test in `test_json_parser.cpp`) |
| S8 | Existing `test/lib` and `test/app` Catch2 tests | All pass after being updated for the new storage shape |
| S9 | CI benchmark runnability | New benchmarks are runnable from CI behind their `[benchmark]` tag; CI does not gate on absolute numbers |

## 9. Decision Log

All previously open questions have been resolved; remaining work is implementation-only. The history is preserved here for context.

1. ~~**KeyIndex life cycle across files**~~ — **Resolved:** per-parse (one `KeyIndex` per `LogData`). `LogData` exposes a streaming `AppendBatch` (req. 4.1.4) used by Stage C and a separate `Merge(LogData&&)` for cross-`LogData` workflows so future "open multiple files" or cross-file diff use cases can merge indices on demand without changing the per-parse default.
2. ~~**Column growth strategy in `LogModel`**~~ — **Resolved:** **`beginInsertColumns` / `endInsertColumns` is mandatory** (req. 4.3.27a). The fallback-to-`beginResetModel` escape hatch was removed in the second-pass review because the new append-only column contract on `LogTable::AppendBatch` (req. 4.1.13, Decision 14) makes incremental insertion the simple, correct path and a model reset would defeat the entire reason streaming exists.
3. ~~**Choice between Taskflow and oneTBB**~~ — **Resolved:** oneTBB chosen, backed by spike results in `C:\code\pipeline_spike\RESULTS.md` (see rationale in req. 4.2.17). The full spike `RESULTS.md` will be quoted in the implementation PR description (req. 4.5.35).
4. ~~**Error batching granularity**~~ — **Resolved:** **Option A — errors ride with their batch**, plus newly-introduced keys also ride with the batch (single `OnBatch(StreamedBatch)` callback carrying `lines`, `new_keys`, and `errors` atomically — req. 4.3.25). Pros vs. a separate low-latency `OnError` channel:
    - **Pros**: ordering is automatic (errors from batch 7 cannot arrive before lines from batch 5; new keys cannot arrive after the lines that reference them), atomic delivery per flush, fewer cross-thread Qt postings, simpler bridging adapter, easy to render "lines 12 000-13 000 had 3 errors" in UI.
    - **Cons**: error-counter latency equals flush latency (≤ 50 ms — sub-perceptible for the status bar); a bad-only batch carries `lines.empty() && !errors.empty()` (sink must tolerate this).
    The 50 ms bound makes the latency cost invisible to the user, while the ordering and chatter wins are concrete; the architecture does not preclude adding a separate fast-path `OnError` later if needed.
5. ~~**Thread-pool / arena lifecycle under oneTBB**~~ — **Resolved:** keep it simple — per-call `tbb::global_control(max_allowed_parallelism, N)` scoped to `Parse` / `ParseStreaming` (req. 4.2.22c). No owned `tbb::task_arena` for now. Revisit if embedding `loglib` in a host that also uses TBB becomes important.
6. ~~**`glz::generic_sorted_u64` in `ToString`**~~ — **Resolved (deferred):** keep the current Glaze round-trip behavior in `JsonParser::ToString` for now (signature changed to take a `LogLine` and walk `IndexedValues()` per req. 4.1.14). Revisit only if a future profiler run shows `ToString` on the hot path; emitting JSON manually ordered by `KeyIndex::SortedKeys()` is a known fallback that does not change the public API.
7. ~~**Timestamp parsing during streaming**~~ — **Resolved:** push `ParseTimestamps` into Stage B so streamed lines have already-parsed `TimeStamp` values when they reach the GUI (req. 4.2.21). The legacy `LogTable::Update` whole-data pass is preserved for non-streaming callers via a `LogData::TimestampsAlreadyParsed()` flag. **Mid-stream column-discovery correctness** is handled by Decision 16 below (option B back-fill on the GUI thread), not by Stage B.
8. ~~**`KeyIndex` back-pointer location on `LogLine`**~~ — **Resolved:** dedicated `const KeyIndex* mKeys` member on `LogLine` itself, set at construction. With the shared canonical `KeyIndex` (Decision 13), Stage B constructs lines directly against the canonical index, so **no rewire ever happens in the streaming path**. Cross-`LogData` `Merge` still rewires (req. 4.1.9). Routing the back-pointer via `LogFile` is rejected because `LogData::Merge` would leave merged-in lines pointing at a stale per-file index.
9. ~~**Cancellation race between `LogModel::Clear()` and queued `OnBatch` events**~~ — **Resolved:** generation id on the bridging adapter; `Clear()` bumps the generation, queued events with stale generations are dropped on the GUI thread (req. 4.3.28, req. 4.3.30). The `std::stop_source` itself is owned by `QtStreamingLogSink` (Decision 18). `Clear()` does not block on the parser thread.
10. ~~**Status-bar layering**~~ — **Resolved:** `LogModel` exposes Qt signals (`lineCountChanged`, `errorCountChanged`, `streamingFinished`); `MainWindow` is the only owner of the status bar and the only signal subscriber (req. 4.3.27, req. 4.3.29).
11. ~~**`USE_SYSTEM_TBB` minimum version**~~ — **Resolved:** `find_package(TBB 2021.5 REQUIRED)` — first oneAPI-style release that has the `tbb::filter_mode` / `tbb::parallel_pipeline` API the implementation depends on (req. 4.6.37).
12. ~~**Performance target wording**~~ — **Resolved:** drop hard numerical multipliers from §2 / §8 and require the PR description to report before/after numbers + a thread-scaling table (req. 4.5.35). The reviewer judges whether the numbers are good enough.
13. ~~**Stage A bottleneck (per-line offset registration)**~~ — **Resolved (second-pass review; refined in third-pass review — see Decision 27):** Stage A finds **batch boundaries only** — total work O(file_size / batch_size) instead of O(lines). Stage B walks each batch's bytes to build a thread-local `local_line_offsets` vector. Absolute line-number assignment is delegated to Stage C (Decision 27 corrects the original design's broken Stage C → Stage A feedback). Stage C concatenates the per-batch offset vectors into `LogFile::mLineOffsets` under the implicit `serial_in_order` lock (req. 4.2.18). The previous design's per-line `LogFile::CreateReference` call from Stage A is removed.
14. ~~**Stage C bottleneck (per-line `KeyId` remap and back-pointer rewire)**~~ — **Resolved (second-pass review):** workers share the canonical `KeyIndex` directly. The `KeyIndex` is internally thread-safe (req. 4.1.2/2a, recommended `tbb::concurrent_hash_map<std::string, KeyId>` + pointer-stable reverse storage) and is fronted by a per-worker `string→KeyId` cache (req. 4.1.2/2b) so the steady-state hot path is a thread-local hash lookup with no atomics. The merge-and-remap pass in Stage C is gone; Stage C just appends `LogLine`s and concatenates line offsets. Care taken: reverse-lookup storage must be pointer-stable under concurrent inserts (`std::vector<std::string>` would be unsafe; `std::deque<std::string>` or `tbb::concurrent_vector` is correct).
15. ~~**`LogValue::std::string` allocation overhead**~~ — **Resolved (second-pass review, in scope for this PRD):** add `std::string_view` as the first alternative of the `LogValue` variant (req. 4.1.6); `LogFile` owns `mio::mmap_source` (req. 4.1.6a) so the views remain valid for the lifetime of `LogData`. JSON parsing uses `field.key()` / `raw_json_string` to capture mmap-pointing views for keys and unescaped string values; falls back to owned `std::string` for escape-decoded strings (req. 4.1.15a, req. 4.1.15b). Adds `loglib::AsStringView` / `loglib::HoldsString` helpers so consumer code never needs to know which alternative is held.
16. ~~**Mid-stream auto-discovered timestamp columns get silently misparsed**~~ — **Resolved (second-pass review, option B):** Stage B parses timestamps using a snapshot `LogConfiguration` taken at `BeginStreaming` time (req. 4.2.21) — no runtime configuration mutation reaches workers, so no race. When the GUI side discovers a new `Type::time` column mid-stream (via `batch.new_keys`), `LogTable::AppendBatch` runs `BackfillTimestampColumn` over already-appended rows for that single column, plus over the just-appended batch for any time column whose keys were not in Stage B's snapshot (req. 4.1.13b). Options A (forbid mid-stream discovery — bad UX for first-open-of-arbitrary-file) and C (single final pass at end-of-stream — defeats streaming) were rejected.
17. ~~**`OnStarted(initialKeys)` was dead code**~~ — **Resolved (second-pass review):** drop the `initialKeys` argument; the new signature is `OnStarted()` (req. 4.3.25). With the shared canonical `KeyIndex` and no JSON pre-pass, the parameter would always be empty and forwarding it would only invite confusion.
18. ~~**`std::stop_source` ownership**~~ — **Resolved (second-pass review):** `std::stop_source` is owned by `QtStreamingLogSink` (req. 4.3.28). `BeginParse()` replaces it with a fresh source per parse and returns the `std::stop_token`, which is plumbed to `JsonParserOptions::stopToken` (req. 4.2.22). `RequestStop()` bumps the generation id *before* requesting stop so any already-queued `OnBatch` is dropped on receipt. Stage A is the only filter that consumes the token (req. 4.2.22a); Stage B never preempts a half-parsed line.
19. ~~**`LogLine::mFileReference` was `const`-qualified**~~ — **Resolved (second-pass review):** drop the `const` qualifier (req. 4.1.9a). The `const` blocked move-assignment, which the streaming pipeline needs whenever `std::vector<LogLine>` grows or whenever a `ParsedBatch::lines` vector is moved between TBB filters.
20. ~~**Cancellation latency was unspecified**~~ — **Resolved (second-pass review):** cancellation is bounded by one Stage A round-trip plus drain of `ntokens` in-flight tokens (req. 4.2.22b). Wasted work is bounded by `ntokens × kBatchSizeBytes` (~8 MiB on 16 cores). The PR must include a measured `Clear()`-to-`OnFinished(true)` time on the 1 000 000-line benchmark.
21. ~~**Final-flush guarantee before `OnFinished`**~~ — **Resolved (second-pass review):** Stage C **must** flush its in-flight `StreamedBatch` immediately before invoking `OnFinished(cancelled)` (req. 4.3.26a). Guarantees zero data loss across natural EOF (truncated final batch) and across cancellation (rows already produced are still delivered).
22. ~~**`JsonParser::Options` struct shape was implicit**~~ — **Resolved (second-pass review):** the struct is fully written down in req. 4.2.22 with named fields `threads`, `batchSizeBytes`, `ntokens`, `configuration`, and `stopToken`, plus a `kDefaultMaxThreads` constant. The default-constructed struct is the recommended GUI streaming configuration.
23. ~~**`IndexedValues()` return type was `auto`**~~ — **Resolved (second-pass review):** explicit `std::span<const std::pair<KeyId, LogValue>>` (req. 4.1.8). This pins the storage layout to a sorted contiguous vector and lets bulk consumers iterate without virtual dispatch or template instantiation.
24. ~~**Parity test "byte-identical `LogData`" was undefined**~~ — **Resolved (second-pass review):** define `LogValueEquivalent(a, b)` in `loglib/log_line.hpp` that treats `std::string` and `std::string_view` of equal byte content as equivalent (req. 4.1.16); the parity test compares lines pairwise via this helper rather than relying on `operator==` of the variant (which would reject byte-equal `string`/`string_view` pairs).
25. ~~**Mid-stream column-discovery order was unspecified**~~ — **Resolved (second-pass review):** column discovery is **append-only** (req. 4.1.13). Existing column indices and `KeyId`s never change position; new columns are appended at the end. This is a contract on `LogConfigurationManager::Update` (or its `LogTable`-side wrapper) and is asserted by a Catch2 test in the implementation PR.
26. ~~**Performance ideas from the second-pass review (per-line scratch reuse, `madvise`/`PrefetchVirtualMemory`, `field.key()` fast path, pre-seeding the canonical `KeyIndex`, parallel Stage A scan)**~~ — **Resolved (second-pass review):**
    - Per-line scratch reuse: required (req. 4.2.20a).
    - `madvise SEQUENTIAL` / `PrefetchVirtualMemory`: required (req. 4.1.6b).
    - `field.key()` for unescaped keys: required (req. 4.1.15b); same string-view-when-possible logic as for value strings (req. 4.1.15a).
    - Pre-seeding the canonical `KeyIndex` from a first-batch peek: subsumed by Decision 14 — the shared canonical index naturally picks up keys as soon as the first worker sees them, and the per-worker `string→KeyId` cache absorbs the warm-up cost. Explicit pre-seeding offers no additional measurable gain over the current design and would require a synchronous pre-pass that hurts time-to-first-row.
    - Parallel Stage A scan: subsumed by Decision 13 — Stage A's work is now O(file_size / batch_size) (~8 000 `memchr` calls for a 2 GB file), well below the parallelization threshold. Re-evaluate only if profiling on NVMe SSDs shows Stage A on the critical path.
27. ~~**Stage A `first_line_number` feedback channel from Stage C was unimplementable**~~ — **Resolved (third-pass review):** `tbb::filter_mode::serial_in_order` only orders a filter against itself, so Stage A invocation N+1 routinely runs before Stage C invocation N completes; Stage A cannot legally read `prev_line_count` from Stage C without serializing the entire pipeline. The fix is to **stamp absolute line numbers in Stage C** (req. 4.2.18), which is `serial_in_order` and therefore owns a race-free local `mNextLineNumber` prefix-sum counter. `first_line_number` is removed from both the `Batch` token and the `ParsedBatch` token; Stage B constructs `LogFileReference` with a placeholder line number, and Stage C overwrites it via a new `LogFileReference::SetLineNumber(size_t)` method as part of `AppendBatch`. Stage A's token shrinks to `{batch_index, bytes_begin, bytes_end, file_end}` — pure byte-range information, no line-numbering bookkeeping.
28. ~~**Sort policy for `LogLine::mValues` was ambiguous and binary search was over-mandated**~~ — **Resolved (third-pass review):** Stage B pushes fields to its scratch vector in JSON document order and calls `std::sort` exactly once per line, keyed on `KeyId` (req. 4.1.5). `LogLine::GetValue(KeyId)` no longer mandates O(log n); for typical log lines (5–20 fields) a linear scan with early-exit-on-`id < entry.first` beats binary search on cache-friendly small vectors (req. 4.1.8). The PR description must include a `GetValue` micro-benchmark over 5/10/20/50-field lines; the implementer picks the strategy data-drive.
29. ~~**`LogFile::mFile` (`std::ifstream`) was kept "for compatibility"**~~ — **Resolved (third-pass review):** removed entirely (req. 4.1.6a). Every read path now slices the mmap (`std::string_view{mMmap.data() + mLineOffsets[i], len}`); `LogFile::GetLine(size_t)` is reimplemented in terms of the mmap. `LogFile` shrinks by ~256 bytes and loses an unsynchronized stream object on a `noexcept`-movable type. A new Catch2 test (req. 4.1.16) asserts that `LogFile` move-construct preserves the mmap address so outstanding `std::string_view`s remain valid.
30. ~~**`LogValue::std::string_view` lifetime footgun on `SetValue(KeyId, ...)`**~~ — **Resolved (third-pass review):** added `loglib::ToOwnedLogValue(LogValue) -> LogValue` (req. 4.1.6) which copies a borrowed `string_view` into an owned `std::string`. The lifetime contract is documented: callers that copy a `LogValue` out of a `LogLine` for any persistent state (clipboard, undo, filter state) **must** call `ToOwnedLogValue` first. Debug builds **should** assert when an "untrusted" `string_view` lands in `SetValue`; the trust signal is a `LogValue::TrustView{}` tag that a few well-known callers can pass.
31. ~~**`JsonParserOptions::configuration` was a raw `const LogConfiguration*`**~~ — **Resolved (third-pass review):** changed to `std::shared_ptr<const LogConfiguration>` (req. 4.2.21, req. 4.2.22). The shared pointer is captured by the parser thread for the lifetime of the parse, removing the shutdown-crash window where the GUI could destroy the configuration before the parser thread finished. Constructed via `std::make_shared<const LogConfiguration>(manager.Configuration())` — a value copy of a small object (typically < 1 KiB).
32. ~~**Mid-stream user edits of `LogConfiguration` required the back-fill machinery to handle two source-of-truth cases**~~ — **Resolved (third-pass review):** the GUI **must** disable the configuration-editing UI between `BeginStreaming` and `EndStreaming` (req. 4.3.29). With user edits forbidden, req. 4.1.13b shrinks to handle only **auto-promotion of newly-discovered keys to time columns** (a single, much simpler case). The user-edit branch (which previously needed both "back-fill all rows" and "back-fill this batch only" sub-cases) is gone.
33. ~~**`LogTable::AppendBatch` returned `AppendBatchResult` shaped for Qt's `beginInsertRows`/`beginInsertColumns`**~~ — **Resolved (third-pass review):** changed to `void` (req. 4.1.13a). `LogModel::AppendBatch` brackets the call with its own `RowCount()`/`ColumnCount()` observations to compute the deltas (req. 4.3.27). Rationale: returning a Qt-shaped struct from `loglib` is a leaky abstraction; Qt is an `app/`-only dependency. The new `LogTable::LastBackfillRange() -> std::optional<std::pair<size_t, size_t>>` accessor handles the back-fill notification handoff in the same Qt-agnostic way.
34. ~~**Per-worker `string→KeyId` cache and `ParseCache` were both hard-mandated**~~ — **Resolved (third-pass review):** both are now opt-in via `JsonParserOptions::useThreadLocalKeyCache` and `useParseCache` (default `true`), with mandatory benchmark variants (`[no_thread_local_cache]`, `[no_parse_cache]`) so the actual per-cache savings are measured rather than assumed (req. 4.1.2/2b, req. 4.1.15). Implementer is empowered to flip the defaults or remove a cache entirely if benchmarks show < 5 % / < 3 % win respectively.
35. ~~**Empty-rows `StreamedBatch` (errors only) crashed `LogModel::AppendBatch` via `beginInsertRows(first, first-1)`**~~ — **Resolved (third-pass review):** the empty-rows case is now spelled out in req. 4.3.25 and req. 4.3.27 step 5 — `LogModel::AppendBatch` **must** skip the `beginInsertRows`/`endInsertRows` pair when no rows were appended. The same applies to columns when `new_keys` is empty.
36. ~~**`KeyIndex` API took `const std::string&` for probes**~~ — **Resolved (third-pass review):** `GetOrInsert` and `Find` take `std::string_view` and the underlying container **must** support transparent (heterogeneous) hashing so probing does not allocate. For `tbb::concurrent_hash_map`, this is a `HashCompare` whose `hash`/`equal` overloads accept `std::string_view`; for `tsl::robin_map`, this is C++20 `is_transparent` (req. 4.1.2/2a).
37. ~~**`std::vector<std::streampos>` for line offsets was wasteful on Windows**~~ — **Resolved (third-pass review):** replaced with `std::vector<uint64_t>` everywhere (req. 4.1.6a, req. 4.2.18). MSVC's `std::streampos` is 24 bytes; `uint64_t` is 8. For a 2 GB file with ~20 M lines that's a ~160 MB memory saving plus faster comparisons. `LogFile::CreateReference(std::streampos)` overload is removed; the `size_t` overload survives.
38. ~~**`LogFile::mLineOffsets` was grown by per-batch `vector::insert` with no upfront reservation**~~ — **Resolved (third-pass review):** `LogFile::ReserveLineOffsets(size_t)` (new, req. 4.1.6a) is called from `LogModel::BeginStreaming` with the heuristic `file_size / 100`, amortizing Stage C's per-batch concatenation to amortized O(1) per line and avoiding the 5–6 reallocations a 20 M-element vector would otherwise see.
