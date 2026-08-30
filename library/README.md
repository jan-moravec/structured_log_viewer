# loglib

Qt-free structured-log core used by Structured Log Viewer. C++23, static library. Detailed header notes live in [`CONTRIBUTING.md`](../CONTRIBUTING.md#library).

## Install

From a tree already configured and built with a CMake preset:

```sh
cmake --install build/release --prefix <prefix> --component loglib
```

`--component loglib` stages the static archive, supported headers, bundled `tsl/robin_map.h`, private link archives, and the CMake package. It does not install the GUI. Headers under `loglib/internal/` are not installed.

## Consume

```cmake
cmake_minimum_required(VERSION 3.28)
project(example LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
find_package(loglib CONFIG REQUIRED)
add_executable(example main.cpp)
target_link_libraries(example PRIVATE loglib::loglib)
```

Configure with `-DCMAKE_PREFIX_PATH=<prefix>`. Do not add `library/include` or name loglib's third-party packages (`fmt`, TBB, simdjson, PCRE2, …) in the consumer `CMakeLists.txt`.

`loglib` is a static library. Private compiled archives are linked automatically as `$<LINK_ONLY:>` imported targets so vendor types stay out of the public compile interface. Header-only private dependencies (mio, glaze, asio, Howard Hinnant `date`) are compiled into the archive and are not installed. When TLS was enabled at build time, the package calls `find_dependency(OpenSSL)`; the consumer still does not name OpenSSL.

On Windows, keep `tbb12.dll` from `<prefix>/bin` on `PATH` or next to the executable. A default `TimeZoneContext` is UTC and does not need tzdata; `TimeZoneContext::Load` requires an IANA tzdata directory.

A Release archive built with interprocedural optimization (MSVC `/GL`) needs LTCG at the final link. The `loglib_consumer` CTest passes `CMAKE_INTERPROCEDURAL_OPTIMIZATION` from the parent preset.

## Minimal usage

[`test/consumer/src/main.cpp`](../test/consumer/src/main.cpp) is the smoke path. Typical composition:

1. Construct a `TimeZoneContext` (default UTC, or `Load(tzdataPath[, zone])`).
2. Ingest with `LogFactory::Create` / `ParseFile`.
3. Hold rows in `LogTable`.

`SetProcessDefaultTimeZone` is a documented process-wide convenience for formatting helpers such as `TimeStampToDateTimeString`. Core conversion APIs take an explicit context; they do not require a prior `Initialize` call.

## API stability

Supported headers are `LOGLIB_PUBLIC_HEADERS` in [`library/CMakeLists.txt`](CMakeLists.txt). Pre-v1 C++ source may still change. Persisted configuration JSON keys stay compatible. There is no shared-library ABI and no vcpkg/Conan recipe.

## v1 boundary decisions

- Timezone type: `TimeZoneContext`. `Load` throws `std::runtime_error` on invalid tzdata or zone.
- `LogFile` hides the mapping backend behind an incomplete type and an out-of-line destructor.
- Configuration groups: `Column` / `ColumnType`, `Source` / `SourceLocator`, `Sort` / `SessionView`, `AnchorEntry`, `HighlightRule`. JSON remains flat on `LogConfiguration`.
- Table collaborators: `EnumInference`, `ColumnTypeHealth`, and `internal::RefreshColumnKeyIds`. `LogTable` remains the row facade.
- Package check: CTest `loglib_consumer` installs component `loglib` and builds `test/consumer` against that prefix.
