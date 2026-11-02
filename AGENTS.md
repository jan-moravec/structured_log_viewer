# AGENTS.md — Build and test policy for AI agents

This file is the binding, agent-facing summary. Read it before running a build or test. Humans should use [`CONTRIBUTING.md`](CONTRIBUTING.md) for the full development guide.

## Comments and Doxygen

Public and non-obvious C++ APIs use Doxygen comments. These rules apply to `lib/` headers now and to the rest of the
code when it is touched. Match the example below.

1. Every header starts with a file comment, above `#pragma once`. It has a one-sentence `@brief` describing the header,
   then a body saying what the unit is, why it exists, what it connects to, and where it sits in the library.
1. No backticks in comments. Write LogTable, not the name in backticks. If a sentence would need code in it, rewrite the
   sentence.
1. Say why and how to use it. Do not restate the name: "Returns the row count" on RowCount adds nothing. Delete comments
   that add nothing.
1. Describe current behavior only. No history, no "previously", "no longer", "legacy", task numbers or PR numbers.
1. Keep it short. Use short sentences. Do not name GUI classes in library comments, and do not explain clang-tidy
   suppressions in API docs.
1. Use `/** */` everywhere, including internal headers. Use `@param` and `@return` only when they add information the
   name and type do not.
1. Let clang-format wrap the text at the 120-column limit. Write each paragraph as one long line, then run clang-format.
   Do not rejoin existing comment lines with a script, because that breaks lists, examples and tables. Rewrite each
   comment by hand.
1. Every comment must be true. Check what the code does before writing about it.

`scripts/check-loglib-docs.py` checks the first two rules for `lib/include`. It reports only for now.

```cpp
/**
 * @brief Common interface to the bytes behind parsed log rows.
 *
 * A LogLine stores offsets and lengths rather than text, so it needs something that can turn them back into bytes.
 * LineSource is that link. FileLineSource serves bytes from a memory-mapped file and StreamLineSource serves them from
 * lines held in memory, which lets the rest of the library treat a static file and a live stream the same way.
 *
 * This is part of the core module. It knows nothing about parsers or tables.
 */
#pragma once

/**
 * @brief Returns a cell as text, formatting it if it is not already a string.
 *
 * The result may point into buffer, so keep buffer alive while using it. A missing cell gives an empty result.
 */
```

## Non-negotiable rules

- Run commands from the repository root.
- Use the CMake presets. Do not improvise with `cmake --build <dir>`, `ctest --test-dir ...`, or direct test executables.
- On Windows, enter the MSVC developer shell in the same persistent PowerShell session that will run CMake.
- Do not run benchmarks unless the task requires them.

The presets provide the expected build tree, parallelism, Qt environment, test data, and sanitizer settings. Bypassing them causes misleading failures.

## Default validation workflow

### Windows (MSVC; primary development platform)

```powershell
.\scripts\Enter-DevShell.ps1 -Arch amd64
cmake --workflow --preset release
```

`Enter-DevShell.ps1` is idempotent: it returns immediately when `VSCMD_VER` is already set. Skipping it can leave MSVC unable to find standard headers such as `<cstddef>`.

### Linux and macOS

```sh
cmake --workflow --preset release
```

The workflow configures, builds, and runs unit and Qt smoke tests. Benchmarks are excluded by their CTest label.

## Iterative workflow

Run only the stages needed:

```sh
cmake --preset release          # configure after CMake/preset changes or for a new build tree
cmake --build --preset release  # incremental build
ctest --preset release          # unit and Qt smoke tests
```

Keep the preset name consistent across stages. Build before running a filtered test:

```sh
cmake --build --preset release
ctest --preset release -R log_table            # CTest test-name regex
ctest --preset release -L regex_templates      # CTest label
ctest --preset release -R log_table -V         # verbose failure reproduction
```

## CLI tests

The `slp` tests (`test/cli/`) carry the CTest label `cli`. They run the real `slp` binary, so rebuild first and select them with the preset:

```sh
cmake --build --preset release
ctest --preset release -L cli
ctest --preset release -R tail          # e.g. only the slp tail cases
```

`cli/` must stay Qt-free. Platform-specific `slp` code needs a run on that platform.

## Specialized presets

Sanitizer and coverage presets pin `clang-23` / `clang++-23` and are intended for Linux (locally, under WSL, or in CI):

```sh
cmake --workflow --preset clang-asan-ubsan  # ASan + UBSan
cmake --workflow --preset clang-tsan        # TSan; excludes selected Qt suites
cmake --workflow --preset clang-coverage    # source-based coverage
```

Benchmarks are opt-in and require the corresponding build first:

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release-benchmark

cmake --preset relwithdebinfo
cmake --build --preset relwithdebinfo
ctest --preset relwithdebinfo-benchmark
```

`CMakePresets.json` is the source of truth for preset names and behavior. Machine-specific overrides, usually a local Qt path, belong in the gitignored `CMakeUserPresets.json`; see `CONTRIBUTING.md` §Machine-specific overrides.

## Scripted PowerShell runs and interrupted builds

- Do not pipe long CMake or CTest runs through `Tee-Object`. Redirect with `*> <logfile>`, then inspect the file after the process exits. An interrupted pipeline can retain a file handle and make subsequent runs fail with a sharing violation.
- If an agent-owned workflow is interrupted, verify that its process tree has stopped before retrying. Orphaned `cmake`, `ninja`, compiler, test, or application processes can lock the build tree and CTest logs.
- Never terminate a broad list of processes blindly; they may belong to the user or another build. Inspect them first and stop only processes confirmed to belong to the interrupted run. Ask the user if ownership is unclear.

## Common traps

- `ctest -E "benchmark"` excludes tests by **name**, not label. It does not exclude this project's benchmarks. The `release` preset excludes the `benchmark` label; `release-benchmark` includes only that label.
- Do not run `tests.exe`, `apptest.exe`, or another test binary directly. Test presets set `QT_QPA_PLATFORM`, arrange runtime data, and configure sanitizer or coverage environment variables.
- Do not replace `cmake --build --preset <name>` with `cmake --build <directory>`. The latter bypasses preset parallelism and can target a build tree different from the matching test preset.
- Do not run the Clang sanitizer presets natively on Windows; their pinned compiler names and flags target Linux.

## Diagnosing a test failure

1. Rerun the same failing test once with the same preset. This distinguishes a repeatable failure from known filesystem-timing flakes.
1. Reproduce with `-V` and the narrowest useful `-R` or `-L` filter. Test presets already enable output on failure.
1. Inspect the failing assertion and relevant changes. `git log -S <TestName>` and `git blame` can provide context, but history alone does **not** prove a runtime failure predates the branch.
1. Do not stash changes and rebuild a large target merely to make an unsupported “pre-existing” claim. If a baseline run is necessary, weigh its cost and use an isolated worktree or CI result.
1. Do not silence flakiness by disabling a test. Record it with an issue or a referenced TODO; new preset exclusions require explicit justification.

## Validation by change type

- Documentation-only changes normally need no C++ build. Verify any documented commands or preset names against their source files.
- Ordinary C++ or build-system changes require the `release` workflow.
- Platform-specific behavior must be tested on the affected platform. A Linux sanitizer run does not validate Windows-only APIs such as `_wfopen_s`.
- Memory-safety or undefined-behavior-sensitive changes should also run `clang-asan-ubsan`.
- `slp tail` stdin and network cases, and anything that touches its stop watcher, also run `clang-tsan`.
- Threading changes involving the TBB pipeline, `StreamLineSource`, `TailingBytesProducer`, `LogModel::Reset`, or `BoundedBatchQueue` should also run `clang-tsan`.
