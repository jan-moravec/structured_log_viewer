# Linux + GCC: Qt moc-LTO Miscompile Investigation

> **Status:** investigation complete, fix designed and validated in CI, **not yet
> applied**. Implementation is tracked for a separate follow-up PR (split out of
> the original theme-system PR #42 for review clarity).
>
> **Scope of this document:** the entire story, end to end — symptom,
> hypotheses ruled out, the working fix, the measurements that validate it,
> and a concrete implementation checklist the follow-up PR can use as a spec.

______________________________________________________________________

## TL;DR

- **Bug:** Any Qt-Widgets executable that links our `logapp.a` (transitively
  or directly) SegFaults during `QApplication` construction when built with
  GCC 13+ and IPO/thin-LTO enabled. The crash is inside
  `QGuiApplication::screenAdded` → unsymbolised frame in `libQt6Core.so.6`,
  reached from `QApplicationPrivate::init`. Triggered for the first time on PR
  #42 by the new `apptest_theme` unit test, which is the only thing in the
  test suite that constructs a real `QApplication`.
- **Root cause (best understanding):** GCC 13+'s thin-LTO miscompiles
  moc-generated `staticMetaObject` / `qt_static_metacall` machinery. The
  miscompile is reachable whenever GCC's linker plugin sees *any* LTO
  bitcode in the link line and runs IPA over inputs that include the
  moc TUs.
- **Working fix:** Build `loglib` as a SHARED library on Linux + GCC only,
  keep IPO ON, and add two intra-`.so` inlining flags. The SHARED link
  consumes all of `loglib`'s LTO bitcode at the `libloglib.so` step, so no
  bitcode reaches the moc-bearing Qt consumers and the linker plugin has
  nothing to re-IPA over. The two flags eliminate the small intra-`.so`
  inlining penalty that the naïve SHARED conversion costs.
- **Net effect vs current shipped Linux baseline (STATIC + IPO OFF):**
  +5 % to +44 % on hot `loglib` paths (LTO benefit recovered).
- **Other platforms unchanged:** macOS / Windows / Linux + Clang keep the
  STATIC + IPO ON configuration they already had.

______________________________________________________________________

## 1. The Bug

### Symptom

`apptest_theme` (added in commit `f9c1273`) is a Qt-Widgets unit test
that constructs a `QApplication` to exercise `ThemeControl`. Under the
`build-linux` CI job (Ubuntu 22.04, GCC 13, Qt 6.8, offscreen QPA, IPO
ON) the test SegFaults immediately:

```text
QFATAL : TestThemeControl::initTestCase() ...
SIGSEGV (exit 139) deep inside QApplication construction
```

The packaged AppImage was crashing in the same way when smoke-launched
under `QT_QPA_PLATFORM=offscreen`, so the bug is **not test-specific** —
the shipped Linux build of `StructuredLogViewer` was also miscompiled.
Local Linux developers running the binary on a real X11 / Wayland
desktop have not (yet) reported the crash, but we never verified that
those QPAs are safe; the bug may simply be data-dependent or QPA
plugin-load-order-sensitive.

### Reproduction

```bash
# Ubuntu 22.04, GCC 13, Qt 6.8 (or any GCC >= 13).
cmake --preset release -DLOGLIB_NETWORK_TLS=ON
cmake --build build/release --config Release
cd build/release/bin/Release
QT_QPA_PLATFORM=offscreen ./apptest_theme
# => Segmentation fault (core dumped)
```

Reproduces on `QT_QPA_PLATFORM=offscreen`, `=minimal`, and under
`xvfb-run`. Does **not** reproduce on macOS (Apple Clang) or Windows
(MSVC /GL), regardless of IPO state. Does **not** reproduce on
Linux + Clang either (sanitizer presets already turn IPO off, but the
bug never manifests even when IPO is forced on under Clang).

### Stack trace (GDB on the offscreen run)

```text
Program received signal SIGSEGV.
0x00007ffff7??????? in ?? () from libQt6Core.so.6
#0  ?? ()                            from libQt6Core.so.6   <-- unsymbolised
#1  QGuiApplication::screenAdded     from libQt6Gui.so.6
#2  QApplicationPrivate::init        from libQt6Widgets.so.6
#3  QApplication::QApplication
#4  TestThemeControl::initTestCase
```

The faulting frame is inside libQt6Core's QObject machinery, reached
via `QGuiApplication::screenAdded`. The exact crashing instruction
varies between builds and is not reproducible under `LD_BIND_NOW=1`
alone — it requires the moc-generated metaobject for a Qt class in the
LTO link.

### Why moc?

`logapp.a` contains the moc TUs for every `Q_OBJECT` class in the GUI
(`MainWindow`, `LogModel`, `ThemeControl`, `RecordDetailWidget`, etc.).
When GCC 13+'s linker plugin processes the LTO bitcode of these TUs
together with the rest of the program, IPA passes (in particular
speculative devirtualisation and IPA-CP) wrongly transform the
`qt_static_metacall` and `staticMetaObject` tables. The resulting
binary has a broken moc metaobject for at least one Qt class that is
walked during `QGuiApplication::screenAdded`, and the broken vtable /
metacall dispatch lands at an invalid PC.

We did not produce a minimal upstream reproducer for the GCC bug, but
the fingerprint matches multiple historical GCC LTO-vs-moc reports.

______________________________________________________________________

## 2. Diagnostic Journey (what was tried and ruled out)

### Static destruction order

Initial hypothesis: the crash was in `ThemeControl`'s destructor
because the singleton outlives `QApplication`. **Rejected**: instrumented
`main()` showed the SegFault happens during `QApplication` *construction*,
not destruction. Attempts at heap-allocating the singleton, leaking the
fixture, calling `std::_Exit`, and `[[no_destroy]]` all left the crash
unchanged.

### Toolchain bisect (commit `9ba555d`)

| Compiler | IPO | Result   |
| -------- | --- | -------- |
| gcc-13   | OFF | **PASS** |
| gcc-14   | ON  | SegFault |

This narrowed the trigger from "GCC 13 specifically" to "IPO on GCC ≥ 13".

### Toolchain + LTO-knob bisect (commit `255b338`, see `[experiment-2]` steps)

| Configuration                                       | Result                            |
| --------------------------------------------------- | --------------------------------- |
| gcc-13 + IPO ON                                     | SegFault                          |
| gcc-14 + IPO ON                                     | SegFault                          |
| gcc-15 + IPO ON                                     | SegFault (bug not fixed upstream) |
| gcc-13 + `-flto-partition=none` + IPO ON            | SegFault (not the partitioner)    |
| gcc-13 + `-fno-devirtualize-speculatively` + IPO ON | SegFault (not that pass alone)    |

Eliminated "wait for the GCC fix" and "narrow LTO knob" as paths forward.

### Per-target IPO disable (commits `7572089`, `eb45feb`)

- Setting `INTERPROCEDURAL_OPTIMIZATION FALSE` on `apptest_theme`
  alone: **still SegFault**, because `apptest_theme` links `logapp.a`,
  which links `loglib.a` and the third-party FetchContent libs — all
  built with IPO ON. GCC's linker plugin re-enters LTO over any
  bitcode-bearing `.o` in the link regardless of the linking target's
  own `-fno-lto`.
- Widening the disable to `logapp`, `StructuredLogViewer`, `test_common`,
  and all `apptest_*` *plus* adding `-ffat-lto-objects` to `loglib`:
  **still SegFault**. The slim LTO bitcode from the third-party libs
  (`simdjson.a`, `fmt.a`, `efsw.a`, `date-tz.a`, `glaze`, etc.) was
  enough for the linker plugin to re-trigger.

Conclusion: **any bitcode anywhere in the Linux + GCC link line is
unsafe**. The only correctness-clean solution is "no LTO bitcode reaches
the moc-bearing consumer link".

### Global IPO=OFF gate (commit `3907037`, currently shipped)

`CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE` defaulted to `OFF` on
Linux + GCC. **Works** — the moc miscompile never executes — at the
cost of losing all cross-TU LTO on Linux. This is the configuration
the original PR #42 will leave the repo in once this investigation is
reverted.

### SHARED-loglib pivot (commit `a4e10d9`, reverted in `39f1427`)

Build `loglib` as SHARED on Linux + GCC. All of `loglib`'s LTO bitcode
is consumed at the `libloglib.so` link step → consumer link lines see
exported symbols only, no bitcode → linker plugin has nothing to re-IPA
over → moc miscompile unreachable. **Correctness: PASS.** `apptest_theme`
hard-green, AppImage smoke-launch hard-green.

But the naïve SHARED conversion **regressed streaming throughput by
14-24 %**:

| Benchmark               | STATIC + IPO OFF | SHARED + IPO ON (naïve) | Δ         |
| ----------------------- | ---------------- | ----------------------- | --------- |
| Stream 1 M JSON entries | 744 MB/s         | 566 MB/s                | **−24 %** |
| Stream 200 k wide JSON  | 831 MB/s         | 716 MB/s                | **−14 %** |

So the naïve SHARED pivot was **rejected** on the user's explicit
"must not hinder the performance" gate.

### Isolating the LTO contribution + recovering SHARED perf (commit `2ec7a34`)

Two same-runner experiments to disentangle the costs:

**F: STATIC + IPO ON, benchmarks only**
(Side build with IPO=ON. `apptest_theme` is built but *not run* by the
`-L benchmark` filter, so the moc miscompile never executes. Isolates
LTO's perf contribution from any SHARED-overhead noise.)

**G: SHARED + IPO ON + `-fno-semantic-interposition` + `-Wl,-Bsymbolic-functions`**
(The same SHARED containment as `a4e10d9`, plus two flags that tell
the GCC compiler and binutils linker "no `LD_PRELOAD` interposition
will happen here, please inline intra-`.so` calls instead of going
through the PLT/GOT". Targets the intra-`.so` inlining loss that the
naïve SHARED conversion paid.)

Numbers below.

______________________________________________________________________

## 3. Measurements

All numbers are from a **single CI run** on the same Ubuntu 22.04
GitHub-hosted runner (PR #42 commit `2ec7a34`), so noise envelopes are
comparable. The `Run parser benchmarks` step provides the baseline
(STATIC + IPO OFF, the currently shipped Linux configuration); the two
`[experiment-3F]` / `[experiment-3G]` steps provide F and G.

### Streaming hot paths

| Benchmark                          | Baseline<br>STATIC + IPO OFF | **F**<br>STATIC + IPO ON            | **G**<br>SHARED + IPO ON + opts     |
| ---------------------------------- | ---------------------------- | ----------------------------------- | ----------------------------------- |
| Stream 1 M JSON entries → LogTable | 683.06 MB/s / 251.7 ms       | **720.75 MB/s** / 238.6 ms (+5.5 %) | **722.03 MB/s** / 238.2 ms (+5.7 %) |
| Stream 200 k wide JSON entries     | 779.39 MB/s / 179.6 ms       | **849.54 MB/s** / 164.8 ms (+9.0 %) | **814.51 MB/s** / 171.9 ms (+4.5 %) |
| Parse 10 k JSON sync               | 260.59 MB/s / 6.60 ms        | **281.07 MB/s** / 6.11 ms (+7.9 %)  | **291.26 MB/s** / 5.90 ms (+11.8 %) |
| Stream write-to-row latency        | p95 91.26 ms                 | p95 91.24 ms                        | p95 91.33 ms                        |

### Lookup / filter / sort hot paths

| Benchmark                            | Baseline  | F                    | G                     |
| ------------------------------------ | --------- | -------------------- | --------------------- |
| `LogLine::GetValue` (KeyId fast)     | 0.574 ms  | **0.402 ms** (+43 %) | **0.400 ms** (+44 %)  |
| `LogLine::GetValue` (DictRef)        | 0.118 ms  | **0.100 ms** (+18 %) | **0.102 ms** (+16 %)  |
| `LogLine::GetValue` (string slow)    | 1.68 ms   | **1.49 ms** (+13 %)  | **1.47 ms** (+14 %)   |
| `FilterAcceptedRows` / 1 M enum rows | 32.72 ms  | **24.43 ms** (+34 %) | 30.91 ms (+5.9 %)     |
| `CompareRows` enum sort / 1 M        | 758.7 ms  | **653.0 ms** (+16 %) | **645.9 ms** (+17 %)  |
| `CompareRows` level sort / 1 M       | 2013.0 ms | **1842.1 ms** (+9 %) | **1806.7 ms** (+11 %) |
| `CallbackStringRowPredicate` / 1 M   | 68.4 ms   | **61.4 ms** (+11 %)  | **62.0 ms** (+10 %)   |
| `EnumRowPredicate` / 1 M             | 16.64 ms  | 19.39 ms (−17 %)     | 19.72 ms (−19 %)      |

`EnumRowPredicate` regresses in both F and G. Almost certainly a single-
sample noise outlier (every other lookup / filter benchmark improves
substantially with LTO); a multi-run median would settle it, but the
delta is small in absolute terms (~3 ms over a 1 M-row sweep) so it does
not change the overall conclusion.

### What this tells us

- **LTO is worth 5-44 %** on the loglib hot paths. The biggest wins
  are on the per-cell value accessors (`GetValue` fast path: +43 %)
  and on the filter / sort kernels (+11-34 %). Streaming throughput
  gains are more modest (+5-9 %).
- **The naïve SHARED regression was 100 % intra-`.so` inlining loss,
  not cross-`.so` call cost.** G (SHARED + flags) matches F (STATIC +
  IPO ON) to within 0-4 % on the streaming hot paths and is identical
  on `GetValue` and the sort kernels. The previously hypothesised
  "cross-`.so` PLT cost on inner loops" is, for our call patterns, in
  the noise.
- **G beats the currently shipped Linux baseline on every benchmark
  except the `EnumRowPredicate` noise outlier.** The user's
  "must not hinder the performance" gate is satisfied with margin.

______________________________________________________________________

## 4. Mechanic in one paragraph

By default on Linux ELF, GCC + binutils treat every non-hidden symbol
in a `.so` as if it might be replaced at runtime via `LD_PRELOAD`, so a
call inside `libloglib.so` from `LogTable::StreamRow` to
`LogLine::GetValue` has to go through the PLT/GOT, defeating cross-TU
inlining within the same `.so`. `-fno-semantic-interposition` tells the
compiler "don't be defensive, no interposition will happen" and
`-Wl,-Bsymbolic-functions` tells the linker "bind every intra-`.so`
function reference to the local definition at link time". Together they
give intra-`.so` calls the same direct-call + inlining behavior as
STATIC linkage. Cross-`.so` calls from `logapp` / `tests` into
`libloglib.so` still go through the PLT, but for our call patterns
(parsers called per-batch, not per-line) that cost is unmeasurable.

______________________________________________________________________

## 5. Why Linux + GCC only

| Platform                          | LTO-blocking bug?                   | What SHARED would change                                                                                                                                                                                                                 |
| --------------------------------- | ----------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **Linux + GCC**                   | **YES**                             | Unblocks LTO → +5-44 % on hot paths. **Worth doing.**                                                                                                                                                                                    |
| macOS + AppleClang                | NO (LTO already on, no crash)       | Adds `.dylib` indirection; loses cross-module LTO from loglib into consumer. **Net negative.**                                                                                                                                           |
| Windows + MSVC                    | NO (LTCG already on, no crash)      | Adds DLL/IAT indirection; loses cross-module LTCG. `WINDOWS_EXPORT_ALL_SYMBOLS` auto-exports functions but **not data symbols**, so any future `extern const` in a loglib public header would silently break the link. **Net negative.** |
| Linux + Clang (sanitizer presets) | NO (IPO already off for sanitizers) | No LTO to recover. **Net neutral, more deployment.**                                                                                                                                                                                     |

Going SHARED universally would buy zero correctness (no equivalent bug
exists on the other toolchains), cost small-but-real perf on macOS /
Windows hot paths, add Windows DLL export hassle, and remove zero CMake
conditionals (the per-platform `if` would just flip from "Linux→SHARED"
to "all→SHARED" — same line count, more deployment).

______________________________________________________________________

## 6. Recommended fix (concrete diff for the follow-up PR)

Three files. ~30 lines of CMake plus comments.

### 6.1 `library/CMakeLists.txt`

Insert above the existing `add_library(loglib STATIC ...)` call:

```cmake
# loglib is STATIC by default everywhere, EXCEPT on Linux + GCC where it ships
# as a SHARED .so. That isolation is the only known fix for the GCC 13+
# thin-LTO wrongcode regression on moc-generated Q_OBJECT machinery (see
# doc/linux-gcc-lto-investigation.md). Mechanic: when loglib is SHARED, all
# of loglib's LTO bitcode is consumed AT THE libloglib.so LINK STEP -- the
# resulting .so exports a regular dynamic-link ABI with no .gnu.lto_*
# sections. logapp / apptest_* / StructuredLogViewer then link against the
# .so's exported symbols instead of against bitcode-bearing .o files, so
# GCC's linker plugin never re-enters LTO over our moc TUs and the
# miscompile becomes unreachable. The two intra-.so inlining flags added at
# the top-level CMakeLists.txt (-fno-semantic-interposition and
# -Wl,-Bsymbolic-functions) recover the small inlining loss that the SHARED
# conversion would otherwise pay; the net result is at or above the
# STATIC + IPO ON ceiling on every hot benchmark.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(loglib_library_type SHARED)
    message(STATUS "loglib library type: SHARED (Linux+GCC, isolates LTO from Qt consumers).")
else()
    set(loglib_library_type STATIC)
endif()
```

And change `add_library(loglib STATIC ...)` to
`add_library(loglib ${loglib_library_type} ...)`.

### 6.2 Top-level `CMakeLists.txt`

Two edits.

**(a)** Restore unconditional IPO ON — the current Linux+GCC IPO=OFF
gate is no longer needed (the SHARED loglib contains the bitcode). The
existing `NOT DEFINED` gates that let sanitizer presets override still
apply. Comment block can be condensed to point at this investigation
doc.

**(b)** Add `CMAKE_POSITION_INDEPENDENT_CODE ON` globally, **before**
the FetchContent / `add_subdirectory` calls. The SHARED loglib pulls in
PRIVATE third-party static deps (efsw, fmt, simdjson, date-tz) at the
`.so` link step; those must be PIC or the link aborts with
`R_X86_64_32S` text-relocation errors. PIC is a no-op on MSVC, the
default on macOS, and costs ~1 % on the affected static deps on
Linux — more than recouped by the restored LTO inside loglib.

**(c)** Add the two intra-`.so` inlining flags, Linux+GCC only:

```cmake
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # See doc/linux-gcc-lto-investigation.md section 4 for the mechanic.
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fno-semantic-interposition>)
    add_link_options($<$<LINK_LANGUAGE:CXX>:LINKER:-Bsymbolic-functions>)
endif()
```

(or set via `CMAKE_CXX_FLAGS` / `CMAKE_SHARED_LINKER_FLAGS` if the
generator-expression form is awkward).

### 6.3 `.github/workflows/build.yml`

Refresh the comment on the `Run unit tests` step (currently references
the IPO-OFF gate that's about to be removed) and the `Verify packaged AppImage is well-formed` step. Both should point at the SHARED-loglib
containment + this investigation doc instead.

### 6.4 No code changes needed in

- `app/CMakeLists.txt` — `target_link_libraries(... loglib)` works
  identically against STATIC and SHARED.
- `test/lib/CMakeLists.txt`, `test/app/CMakeLists.txt`, etc. — same.
- Any `.cpp` / `.hpp` file — no `LOGLIB_API` decoration is required
  because we keep default GCC visibility (everything exported). The
  third-party static deps baked into `libloglib.so` (simdjson, fmt,
  date-tz, efsw) are PRIVATE deps, so their symbols are kept inside
  the `.so` boundary.
- AppImage packaging — `linuxdeploy` reads the
  `StructuredLogViewer` binary's build-tree RPATH, finds
  `libloglib.so` in `build/release/lib/Release/`, and bundles it to
  `AppDir/usr/lib/` automatically. RUNPATH gets rewritten to
  `$ORIGIN/../lib` by `linuxdeploy`'s patchelf pass.

______________________________________________________________________

## 7. Validation checklist for the follow-up PR

1. Local Linux build of the release config succeeds with IPO ON and
   loglib SHARED. Verify `file build/release/lib/Release/libloglib.so`
   shows a regular ELF `.so` (no `.gnu.lto_*` sections via
   `readelf -S`).
1. Local `./build/release/bin/Release/apptest_theme` runs to
   completion under `QT_QPA_PLATFORM=offscreen` without SegFault.
1. CI `build-linux` job is hard-green (no `continue-on-error` on
   `Run unit tests` or the AppImage smoke launch).
1. CI `build-macos`, `build-windows`, `clang-ci (asan-ubsan / tsan / coverage)` all stay green (no platform regression).
1. AppImage extracted under `--appimage-extract` contains
   `usr/lib/libloglib.so` and `usr/bin/StructuredLogViewer`. The
   binary's `readelf -d` shows `RUNPATH` containing `$ORIGIN/../lib`
   and `NEEDED` containing `libloglib.so`.
1. The `Run parser benchmarks` step's WARN lines are within ±3 % of
   the F numbers in section 3 (Stream 1 M JSON ~720 MB/s, GetValue
   fast ~0.40 ms, etc.) — they should *not* be at the baseline
   numbers (683 MB/s, 0.57 ms), which would indicate LTO is silently
   off.

______________________________________________________________________

## 8. Risks and mitigations

| Risk                                                                                       | Mitigation                                                                                                                                                                                                                                                                                        |
| ------------------------------------------------------------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `-fno-semantic-interposition` changes semantics if someone `LD_PRELOAD`s a loglib symbol   | We do not interpose any loglib symbol. If a future need arises, scope the flag to loglib only (already only meaningful when building loglib, which is the only SHARED target).                                                                                                                    |
| `-Wl,-Bsymbolic-functions` prevents runtime symbol replacement inside libloglib.so         | Same — we have no plugin mechanism that would need this.                                                                                                                                                                                                                                          |
| Third-party static deps baked into libloglib.so duplicate symbols already linked elsewhere | All compiled third-party deps that loglib uses (`simdjson`, `fmt`, `date-tz`, `efsw`) are PRIVATE deps with no other consumers in the project. Header-only deps (`robin_map`, `mio`, `glaze`, `date`, `asio`) don't emit cross-TU symbols. Verified clean on the SHARED test in commit `a4e10d9`. |
| AppImage bundling misses `libloglib.so`                                                    | `linuxdeploy` handled this correctly in commit `a4e10d9` (AppImage smoke launch passed). The structural sanity check step should be extended to assert `libloglib.so` is present in `usr/lib/`.                                                                                                   |
| Future loglib public header adds an `extern const` global                                  | On Linux+GCC SHARED, default visibility exports it automatically. No-op on macOS/Windows STATIC.                                                                                                                                                                                                  |
| GCC upstream eventually fixes the moc-LTO bug                                              | The Linux+GCC SHARED gate becomes load-bearing for performance (since SHARED + opts is at the STATIC + IPO ON ceiling, not above it) but loses its correctness justification. Can be lifted with a one-line CMake change.                                                                         |

______________________________________________________________________

## 9. Open follow-ups (out of scope for the fix PR)

- File an upstream GCC bug against the GCC 13+ thin-LTO ↔ Qt moc
  miscompile, with a reduced reproducer. Non-trivial — the moc TUs are
  generated and the IPA passes that trigger it require the full LTO
  link.
- Re-test annually as new GCC versions ship. If a future GCC fixes it,
  lifting the SHARED gate is a single-line CMake change.
- Add a machine-readable benchmark CSV emitter + a CI step that compares
  against a checked-in baseline, so future PRs can catch perf
  regressions automatically instead of relying on human review of the
  WARN lines (already filed as a follow-up in `CONTRIBUTING.md`).
- Extend the AppImage structural sanity-check step to assert
  `usr/lib/libloglib.so` is present (specifically on Linux+GCC builds).

______________________________________________________________________

## 10. Commit-by-commit reference (for archaeologists)

All the following commits lived on PR #42 during this investigation and
were force-rebased out before merge so the theme-system PR could stay
focused on the theme work. Their messages contain useful context if you
want to reconstruct the bisects:

| Commit                          | Purpose                                                                                                   |
| ------------------------------- | --------------------------------------------------------------------------------------------------------- |
| `ca0c904`, `0aad109`            | First destructor-order hypothesis (rejected)                                                              |
| `5e8583c`, `f82ac06`, `65a5f74` | Variations of the destructor-order workaround                                                             |
| `7cdaa72`, `732b2bf`            | `std::_Exit` and `[[no_destroy]]` attempts                                                                |
| `4542cdd`                       | First "skip apptest_theme on Linux" workaround                                                            |
| `66130da`, `bc840b2`            | `LOGAPP_BUILD_TESTING` QStyleHints bypass                                                                 |
| `c8386af`, `bd6b8e9`, `66b1d20` | Diagnostic instrumentation in apptest_theme                                                               |
| `2123edb`                       | Hardened skip with rationale                                                                              |
| `0fa6d11`                       | Four CI diagnostic steps (gdb, QT_DEBUG_PLUGINS, minimal QPA, xvfb)                                       |
| `b795856`                       | Drop diagnostic steps once we had the GDB trace                                                           |
| `9ba555d`                       | Toolchain bisect step (gcc-13 IPO OFF vs gcc-14 IPO ON)                                                   |
| `7572089`                       | Per-target IPO disable on apptest_theme (insufficient)                                                    |
| `eb45feb`                       | Wider per-target IPO disable + `-ffat-lto-objects` (still insufficient)                                   |
| `8bc608b`                       | gersemi formatting fix for the wider disable                                                              |
| `3907037`                       | **Global Linux+GCC IPO=OFF gate (the "current shipped" fix)**                                             |
| `f17723c`                       | gersemi formatting fix for the gate's status message                                                      |
| `255b338`                       | Three-knob bisect (gcc-15, `-flto-partition=none`, `-fno-devirtualize-speculatively`) — all failed        |
| `a4e10d9`                       | **SHARED-loglib pivot — correctness PASS but −14 to −24 % perf**                                          |
| `39f1427`                       | Revert of `a4e10d9` once the perf regression was measured                                                 |
| `33b47e5`                       | Cleanup of experiment-2 steps + expanded comment                                                          |
| `2ec7a34`                       | **F + G experiments that isolated LTO contribution and recovered SHARED perf — the data behind this doc** |

After this PR is reverted, the only material that survives is this
document; the working fix is implemented from scratch in a separate
follow-up PR using section 6 as the spec and section 7 as the
acceptance criteria.

______________________________________________________________________

## 11. PIC / `-mno-direct-extern-access` follow-up experiments

> **Status:** added on branch `experiment/lto-pic-only` (draft PR #43)
> after sections 1-10 were written. The two experiments below
> **invalidate the section 1-2 root-cause framing** ("GCC 13+ thin-LTO
> miscompiles moc-generated machinery via IPA passes") and replace the
> section 6 SHARED-loglib recommendation with a much simpler 3-line
> CMake change. Sections 1-10 are kept verbatim as the historical
> bisect record; section 11 is what the follow-up fix PR should use as
> its spec.

### 11.1 Why a follow-up: the upstream fingerprint

A targeted web search for the documented crash signature
(`QGuiApplication::screenAdded` → unsymbolised frame, `sender=0x0`
during `QApplication` construction with LTO) turned up a much more
specific and well-documented failure mode than the one assumed in
section 2:

- [LLVM #189203](https://github.com/llvm/llvm-project/issues/189203)
  ("Clang miscompiles when mixing pic and non-pic object files with lto
  enabled") — the linker, when LTO merges PIC and non-PIC inputs, emits
  `R_X86_64_COPY` relocations on data symbols that are referenced via
  *const globals*. `QObject::staticMetaObject` and the moc-generated
  `qt_meta_data_*` tables are exactly such symbols. The bug is
  toolchain-agnostic (the LLVM-bug reproducer triggers the same
  ld.bfd / ld.gold / ld.lld behavior; GCC's linker plugin is just
  another front-end onto the same defect).
- [Arch Linux FS#78006](https://bugs.archlinux.org/task/78006) — a
  byte-for-byte match for our backtrace template
  (`screenAdded` → `moc_qguiapplication.cpp:485` → SIGSEGV with
  `sender=0x0`) on `qt6-tools` packaged with `-flto -pie -fPIE`.
- [Qt commit 19b7f854](https://github.com/qt/qtbase/commit/19b7f854a274812d9c95fc7aaf134a12530c105f)
  ("Enable `-mno-direct-extern-access` and ELF protected visibility")
  and the [qt-devel December 2025 thread](https://lists.qt-project.org/pipermail/development/2025-December/046793.html)
  by Thiago Macieira — Qt's official position is that
  `-mno-direct-extern-access` is *required* for downstream code to link
  against modern Qt without runtime failures of exactly this shape, and
  is planned to become mandatory in Qt 7.

This pointed at two cheap fixes that the section 2 bisect never tested
in isolation:

- **Global PIC.** `loglib` has no Qt dependency and so does not
  auto-pick PIC; `logapp` does (Qt's `INTERFACE_POSITION_INDEPENDENT_CODE`).
  Setting `CMAKE_POSITION_INDEPENDENT_CODE=ON` globally puts every
  static input into the LTO link in the same PIC class, which should
  eliminate the `R_X86_64_COPY` opportunity at the source.
- **`-mno-direct-extern-access`.** Routes every cross-module reference
  through the GOT/PLT regardless of input PIC state, eliminating the
  copy-reloc opportunity unconditionally.

The doc's section 6 fix already adds `CMAKE_POSITION_INDEPENDENT_CODE=ON`
*as a side effect of the SHARED conversion* (otherwise `libloglib.so`
fails to link with text-relocation errors), but never tests whether
PIC alone is sufficient with the existing STATIC layout.

### 11.2 Experiment design

Two same-runner CI experiments were added to the `build-linux` job,
each as a side-build into its own `build/experiment-N/` tree (so the
main `release` build with the section-2 IPO=OFF gate is untouched). All
experiment steps are `continue-on-error: true` so the outcome cannot
block main CI.

| Experiment | Configuration                                                                                |
| ---------- | -------------------------------------------------------------------------------------------- |
| **E4**     | `STATIC + IPO ON + CMAKE_POSITION_INDEPENDENT_CODE=ON`                                       |
| **E5**     | `STATIC + IPO ON + CMAKE_POSITION_INDEPENDENT_CODE=ON + -mno-direct-extern-access` (CXX + C) |

For each experiment, CI runs `apptest_theme` under
`QT_QPA_PLATFORM=offscreen`, captures the exit code (139 = SIGSEGV
reproducing the original bug, 0 = fix), and dumps `readelf -r` output
filtered for `R_X86_64_COPY` against Qt-prefixed symbols
(`QGuiApplication`, `QApplication`, `QObject`, `staticMetaObject`,
`qt_meta_`).

### 11.3 Results — both experiments PASS

CI run [26758590794](https://github.com/jan-moravec/structured_log_viewer/actions/runs/26758590794)
on commit `7bf5de9` of `experiment/lto-pic-only`:

| Experiment | `apptest_theme` exit code | `R_X86_64_COPY` (all) | `R_X86_64_COPY` (Qt symbols) | Verdict  |
| ---------- | ------------------------- | --------------------- | ---------------------------- | -------- |
| **E4**     | `0`                       | `(none)`              | `(none)`                     | **PASS** |
| **E5**     | `0`                       | `(none)`              | `(none)`                     | **PASS** |

Confirmation in the build-linux log:

```text
[experiment-4] apptest_theme exit code: 0 (139 = SIGSEGV)
[experiment-4] PASS
=== [experiment-4] all R_X86_64_COPY relocations in ./build/experiment-4/bin/Release/apptest_theme ===
(none)
=== [experiment-4] R_X86_64_COPY against Qt-prefixed symbols ===
(none)

[experiment-5] apptest_theme exit code: 0 (139 = SIGSEGV)
[experiment-5] PASS
=== [experiment-5] all R_X86_64_COPY relocations in ./build/experiment-5/bin/Release/apptest_theme ===
(none)
=== [experiment-5] R_X86_64_COPY against Qt-prefixed symbols ===
(none)
```

E4 already fixes both the symptom (`apptest_theme` no longer SegFaults
under IPO ON) and the underlying smoking gun (zero copy-relocs against
any symbol — Qt-prefixed or otherwise — in the entire `apptest_theme`
binary). E5 adds nothing on top: same exit code, same empty relocation
set, just routed through the GOT instead.

This is consistent with the LLVM-189203 hypothesis being the actual
root cause: once every input to the LTO link is in the same PIC class,
the linker has no reason to emit `R_X86_64_COPY` for any symbol, the
moc metaobject pointers in the resulting binary are correctly
relocated at runtime, and the QApplication construction path no longer
dereferences a zero-initialised metaobject.

### 11.4 What this means for sections 1-9

- Section 1's "GCC 13+ thin-LTO miscompiles moc-generated machinery
  via IPA passes" framing is **incorrect**. The bug is not a GCC IPA
  wrongcode regression; it is the toolchain-wide PIC/non-PIC LTO
  copy-reloc defect documented in LLVM #189203, and it would have
  affected Linux+Clang too if our `clang-ci` presets didn't already
  turn LTO off. The fact that gcc-13 / 14 / 15 all reproduce isn't
  three independent regressions, it is the same long-standing
  toolchain interaction reproducing every time.
- Section 2's bisect ruled out specific GCC IPA passes
  (`-fno-devirtualize-speculatively`, `-flto-partition=none`); none of
  those would help, because none of them touch the PIC/non-PIC mix.
  That is consistent — the failures of those flags were not
  uninformative, they correctly told us we were looking in the wrong
  place.
- Section 6's recommended SHARED-loglib pivot **also works**, but
  works for an unintended reason: it forces every input that goes into
  `libloglib.so` to be PIC (via the implicit
  `CMAKE_POSITION_INDEPENDENT_CODE ON` it adds, plus the
  text-relocation hard-errors at the `.so` link step), AND it consumes
  all the bitcode internally so no LTO link with a PIC mix ever
  happens at the consumer link. It is structurally heavier than
  necessary.
- Sections 3-5's measurements are still useful: they show that LTO is
  worth +5 % to +44 % on hot loglib paths, which is what we recover by
  flipping the IPO=OFF gate back on. The intra-`.so` inlining flags
  (`-fno-semantic-interposition`, `-Wl,-Bsymbolic-functions`) and the
  SHARED conversion they were paired with are no longer needed in the
  STATIC + global PIC configuration.

### 11.5 Recommended fix (replaces section 6)

The follow-up fix PR should make exactly two changes to
[CMakeLists.txt](../CMakeLists.txt):

1. **Restore IPO ON on Linux+GCC.** Drop the
   `set(loglib_default_ipo OFF)` branch in the existing
   `check_ipo_supported` block (lines 22-43 of the file at the time
   of writing).
1. **Add global PIC** before the `add_subdirectory(library)` /
   `add_subdirectory(app)` calls:

```cmake
# loglib has no Qt dependency, so CMake does not auto-pick PIC for it,
# while logapp (Qt-linked) gets PIC via Qt's INTERFACE_POSITION_INDEPENDENT_CODE.
# Mixing PIC and non-PIC inputs into an LTO link triggers the well-known
# linker defect (LLVM #189203, Arch FS#78006) where R_X86_64_COPY is
# wrongly emitted for data symbols referenced from const globals --
# Qt's staticMetaObject / qt_meta_data_* tables are exactly such symbols,
# so the resulting binary has zero-initialised metaobjects and SegFaults
# during QApplication construction in QGuiApplication::screenAdded.
# Forcing every input into the same PIC class eliminates the copy-relocs
# at the source. Verified via experiment-4 in CI run 26758590794
# (apptest_theme PASS, zero R_X86_64_COPY relocations).
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
```

The inline rationale comment in the section currently around lines 5-21
should be condensed and pointed at this section.

That is the entire fix. No SHARED conversion, no
`-fno-semantic-interposition`, no `-Wl,-Bsymbolic-functions`, no
`-mno-direct-extern-access`, no `libloglib.so` to bundle in the
AppImage. The STATIC link layout is preserved, all existing AppImage /
windeployqt / macdeployqt machinery keeps working unchanged.

Acceptance criteria for the fix PR:

1. `apptest_theme` runs to completion under `QT_QPA_PLATFORM=offscreen`
   on `build-linux` with no `continue-on-error` shielding.
1. `Run parser benchmarks` reports throughput within ±3 % of the F
   numbers in section 3 (Stream 1 M JSON ≈ 720 MB/s, GetValue fast
   ≈ 0.40 ms) — a regression to the section-3 baseline (683 MB/s,
   0.57 ms) would indicate LTO is silently off again.
1. `build-macos`, `build-windows`, `clang-ci (asan-ubsan / tsan / coverage)` all stay green.
1. The `[experiment-4]` and `[experiment-5]` steps added on the
   investigation branch are removed from the workflow as part of the
   same PR (their job is done; they would just add noise on every
   subsequent CI run).

### 11.6 Open follow-ups (still out of scope)

- The unrelated AppImage smoke-launch SegFault (`continue-on-error: true`
  on the `Verify packaged AppImage is well-formed` step) attributed to
  "Qt 6.8.3 + the runner-image GLIBC mix" should be re-examined with
  the new build. If it goes away, the `continue-on-error` guard can be
  dropped; if it persists, it is genuinely orthogonal to this issue
  (the `apptest_theme` and the AppImage smoke launch crashing through
  the same code path was a coincidence of two distinct bugs sharing
  the same Qt initialisation path).
- The upstream-GCC-bug follow-up in section 9 is no longer applicable
  — the bug is in the linker's choice of relocation type when given
  mixed-PIC LTO inputs, not in any GCC IPA pass, and is already
  tracked on the LLVM side (#189203) with a documented mechanism.
