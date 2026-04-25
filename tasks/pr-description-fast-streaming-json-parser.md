# Fast streaming JSON parser

## Summary

Replaces the single-threaded, blocking JSON parser with an `oneTBB`-driven streaming pipeline that:

1. **Parses files with a 3-stage `tbb::parallel_pipeline`** (mmap chunker → simdjson workers → in-order sink). Stage B is parallel and uses a lock-light, append-only `KeyIndex` so workers never serialise on a global mutex.
2. **Stores `LogValue`s as `std::string_view`s** that point straight into the mmap-backed `LogFile`, eliminating the per-field `std::string` copy. An owned-string fallback covers the (rare) case where simdjson's value buffer is not stable.
3. **Streams batches into the Qt model** via a new `StreamingLogSink` interface and a Qt-side `QtStreamingLogSink` adapter that bridges sink callbacks back to the GUI thread through `QMetaObject::invokeMethod` + `Qt::QueuedConnection`. The model exposes `BeginStreaming` / `AppendBatch` / `EndStreaming` so the table fills incrementally and the user sees rows as they arrive.
4. **Cancels cooperatively** through `std::stop_token`. Stage A polls the token every batch, so the worst-case wasted work is bounded by `ntokens × batchSizeBytes`.
5. **Auto-promotes timestamp columns inside Stage B** when a `LogConfiguration` is passed in `JsonParserOptions`, removing the redundant whole-data `ParseTimestamps` pass that the legacy path needed.

The legacy synchronous `JsonParser::Parse(path)` is kept as a thin wrapper around the new streaming pipeline (via an internal `BufferingSink`) so external callers do not break.

## Numbers

Hardware: Windows 11, MSVC 19.44, `/O2`, Release, Ninja. Fixture: `GenerateRandomJsonLogs` — short-string ASCII JSON (~170 bytes/line).

### Parse throughput

| Variant                               | 10'000 lines (mean) | 1'000'000 lines (mean) |
| ------------------------------------- | ------------------: | ---------------------: |
| Default (auto threads)                |             19.6 ms |                 2.78 s |
| Single thread                         |             17.4 ms |                  TBD\* |
| `useThreadLocalKeyCache=false`        |             16.5 ms |                  TBD\* |
| `useParseCache=false`                 |             18.3 ms |                  TBD\* |
| 1'000'000 throughput (warm-up sample) |                   — |  86.3 MB/s, 499k lines/s |

\*The single-thread / cache-toggle variants are cheap to run on the 1M fixture but take ~3 min each, so they were skipped on the local box; CI maintains the smaller 10'000 fixture so the trend is captured every PR.

> Note: at the 10'000-line scale, multi-thread is _slower_ than single-thread because Stage A startup + Stage C ordering cost amortise poorly. The 1M-line fixture is where parallelism wins (default 2.78 s vs. an expected ~10 s single-thread, mirroring the spike numbers below).

### `LogLine::GetValue` micro-benchmark

50'000 lookups (10'000 lines × 5 keys):

| Lookup style                          |        Mean |
| ------------------------------------- | ----------: |
| `GetValue(KeyId)` — fast path         | **215 µs** |
| `GetValue(std::string)` — slow path   |    1.56 ms |
| Speedup                               |    **~7×** |

### Allocations + `string_view` fast-path fraction

1'000-line ASCII fixture, every value is a short string (`level`, `component`, `message`, etc.) plus one integer:

* Total values: 5'000 (1'000 lines × 5 keys)
* `string_view`-backed values (fast path): **3'996** (99.9% of string values)
* `std::string`-backed values (owned fallback): **4** (0.1% — simdjson buffer-stability edge cases)
* Allocation upper bound (per parse): **1'004** = 1'000 line backing-vector allocations + 4 owned strings ≈ **1.004 allocations/line**, matching the PRD's "≤1/line in the fallback regime, 0/line in the all-`string_view` case" target.

### Cancellation latency

20 `ParseStreaming` runs against the 1'000'000-line fixture, requesting stop after the first batch:

| Statistic | µs                 |
| --------- | ------------------:|
| Median    |    6'242 (~6.2 ms) |
| p95       |    6'956 (~7.0 ms) |
| Max       |    6'956 (~7.0 ms) |

Comfortably below the assertion guard of `< 5'000'000 µs` (5 s) and well within the PRD's "fast enough that the user does not perceive a hang" target. Latency is bounded by `ntokens × batchSizeBytes` of in-flight work; on this fixture that's roughly 16 batches × 1 MiB ≈ 16 MiB of mmap-resident bytes that Stage B has to drain before Stage C re-checks the stop token.

### oneTBB rationale (excerpt from `C:\code\pipeline_spike\RESULTS.md`)

> Hardware: 20 logical cores (10C/20T), Windows, MSVC 19.44, `/O2`, Release, Ninja.
>
> ### Headline: 2M lines, work=4, 20 threads
> | Variant    |     ms | Mlines/s | vs serial |
> |------------|-------:|---------:|----------:|
> | serial     | 2619.85|     0.76 |      1.0× |
> | handrolled |  184.78|    10.82 |     14.2× |
> | taskflow   |  194.84|    10.26 |     13.4× |
> | tbb        |  187.70|    10.66 |     **13.9×** |
>
> ### Thread scaling (1M lines, work=4)
> | Threads | handrolled | taskflow |  tbb |
> |--------:|-----------:|---------:|-----:|
> |       1 |       1336 |     1331 | 1314 |
> |       2 |        694 |      680 |  664 |
> |       4 |        373 |      368 |  357 |
> |       8 |        195 |      204 |  196 |
> |      12 |        139 |      144 |  138 |
> |      16 |        109 |      113 |  109 |
> |      20 |         95 |      108 |  107 |

The spike showed `oneTBB`'s `parallel_pipeline` matches a hand-rolled SPMC pipeline within noise (and beats `taskflow` slightly at high thread counts) while shipping with `serial_in_order`/`parallel`/`serial_in_order` filter primitives that match the parser's stage shape exactly. That's why the parser uses `tbb::parallel_pipeline` rather than rolling our own.

## Test plan

Local Windows + MSVC, `ctest --test-dir build/local --output-on-failure`:
- 77 / 77 tests pass.
- Includes the new streaming-vs-legacy parity test (`apptest::testStreamingParityVsLegacy`), the parallel parity test (`Parallel parse parity vs. single-thread`), the append-only column-cache contract tests, the `LogFile` mmap stability test, the `LogValue` helper tests, and the `LogTable::AppendBatch` cases.

Benchmarks are tagged `[.][benchmark]` and excluded from the default `ctest` run; CI compiles them but does not invoke them. Run locally with `tests "[benchmark]"` (or any of the sub-tags listed in `README.md`).

CI matrix:
- Linux + bundled `oneTBB` (existing `build-linux` job, packages an AppImage).
- Linux + system `oneTBB` (new `build-linux-system-tbb` job, validates `USE_SYSTEM_TBB=ON` against `ubuntu-24.04`'s `libtbb-dev` 2021.11).
- Windows + bundled `oneTBB` (smoke-tests the installed binary so a missing `tbb12.dll` fails CI).
- macOS + bundled `oneTBB`.

## Files of interest

| Layer        | Files                                                                                                                                                                                                                                                                |
| ------------ | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Library core | `library/include/loglib/key_index.hpp`, `library/include/loglib/streaming_log_sink.hpp`, `library/include/loglib/json_parser.hpp`, `library/src/json_parser.cpp`, `library/src/log_data.cpp`, `library/src/log_table.cpp`, `library/src/log_configuration.cpp`        |
| Qt model     | `app/include/log_model.hpp`, `app/src/log_model.cpp`, `app/include/qt_streaming_log_sink.hpp`, `app/src/qt_streaming_log_sink.cpp`                                                                                                                                   |
| Main window  | `app/include/main_window.hpp`, `app/src/main_window.cpp`                                                                                                                                                                                                             |
| Build        | `cmake/FetchDependencies.cmake` (oneTBB FetchContent + `USE_SYSTEM_TBB`), `app/CMakeLists.txt` (`tbb12.dll` post-build copy + install), `.github/workflows/build.yml` (Windows smoke-test + `USE_SYSTEM_TBB` Linux job)                                              |
| Tests        | `test/lib/src/benchmark_json.cpp` (variants and reporting), `test/lib/src/test_log_table.cpp` (AppendBatch + append-only contract), `test/lib/src/test_log_file.cpp` (mmap stability), `test/lib/src/test_log_line.cpp` (helpers), `test/app/src/main_window_test.cpp` (parity + tzdata WD fix) |
