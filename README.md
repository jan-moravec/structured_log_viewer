# Structured Log Viewer

[![Build](https://github.com/jan-moravec/structured_log_viewer/workflows/Build/badge.svg)](https://github.com/jan-moravec/structured_log_viewer/actions?query=workflow%3ABuild)
[![GitHub Releases](https://img.shields.io/github/release/jan-moravec/structured_log_viewer.svg)](https://github.com/jan-moravec/structured_log_viewer/releases)

## Overview

Structured Log Viewer is a C++ application consisting of a reusable library (`loglib`) for handling structured log data and a Qt-based GUI (`StructuredLogViewer`) for viewing, searching, and filtering the logs.

Currently, only JSON Lines (one JSON object per line) logs are supported.

## Application

### Supported Platforms

- **Linux** (Ubuntu 22.04+ / compatible; AppImage)
- **Windows** (Windows 10/11; standalone ZIP)
- **macOS** (macOS 12+; DMG)

### Installation

#### Linux

1. Download the latest `StructuredLogViewer-x86_64.AppImage` from the [Releases page](https://github.com/jan-moravec/structured_log_viewer/releases).

1. Make the AppImage executable:

   ```sh
   chmod +x StructuredLogViewer-x86_64.AppImage
   ```

1. Run the AppImage:

   ```sh
   ./StructuredLogViewer-x86_64.AppImage
   ```

   The AppImage supports delta updates via [`appimageupdate`](https://github.com/AppImageCommunity/AppImageUpdate); see [CONTRIBUTING.md](CONTRIBUTING.md#appimage-delta-updates-zsync) for details.

#### Windows

1. Download the latest `StructuredLogViewer.zip`.
1. Unzip the archive.
1. Run `StructuredLogViewer.exe`.

#### macOS

1. Download the latest `StructuredLogViewer.dmg`.
1. Open the DMG and drag `StructuredLogViewer.app` into `/Applications`.
1. Launch it from Launchpad or Spotlight.

> The macOS build is not notarized. On first launch you may need to right-click the app and choose **Open** to bypass Gatekeeper.

Each release is accompanied by SHA-256 checksums; see [Verifying a release](CONTRIBUTING.md#verifying-a-release) for instructions.

### Usage

For information regarding using the application, see the [user guide](doc/README.md).

## Development

### Project Structure

The project is organized into two main components: the `library` and the GUI `app`.

```plaintext
structured_log_viewer/
├── library/               # loglib: core log handling (no Qt dependency)
│   ├── include/loglib/    # Public library headers
│   ├── src/               # Library implementation
│   └── CMakeLists.txt
├── app/                   # Qt6 GUI application (StructuredLogViewer)
│   ├── include/           # GUI headers
│   ├── src/               # GUI implementation (including main_window.ui)
│   └── CMakeLists.txt
├── test/
│   ├── lib/               # Catch2 unit tests and benchmarks for loglib
│   └── app/               # Qt Test smoke tests for MainWindow
├── cmake/                 # Shared CMake modules (warnings, FetchContent)
├── resources/             # Icons, .desktop entry, Qt resource file
├── doc/                   # End-user documentation
├── .github/workflows/     # CI: build + test on Linux / Windows / macOS
├── CMakeLists.txt
└── README.md
```

### Library

The `library` component (`loglib`) provides the core functionality for handling structured log data. It defines types such as `LogLine`, `LogTable`, `LogData`, and `LogConfiguration` for representing log entries and their presentation, plus pluggable parsers (currently `JsonParser`). It has no Qt dependency and can be reused in other applications.

### GUI Application

The `app` component is a Qt 6 Widgets application. It uses `loglib` for parsing and data management, and exposes the data through `QAbstractTableModel`/`QSortFilterProxyModel` subclasses with support for sorting, filtering, searching, and configurable columns.

### Build Instructions

#### Prerequisites

- **CMake** 3.28 or newer
- **Qt** 6.1 or newer (CI uses Qt 6.8)
- A **C++23** toolchain:
  - Linux: GCC 13+ or Clang 17+
  - Windows: MSVC 2022 (Visual Studio 2022)
  - macOS: Xcode 15+ / Apple Clang

Most third-party C++ dependencies are fetched automatically via `FetchContent`. To use system copies, pass the corresponding option (e.g. `-DUSE_SYSTEM_FMT=ON`) when configuring. See [`cmake/FetchDependencies.cmake`](cmake/FetchDependencies.cmake) for the full list.

`loglib` links against [oneTBB](https://github.com/uxlfoundation/oneTBB) (Intel oneAPI Threading Building Blocks) for the parallel JSON parsing pipeline. It is fetched at version `v2022.3.0` by default; pass `-DUSE_SYSTEM_TBB=ON` to use a system installation, which must be **>= 2021.5** (the first oneAPI-style release that ships `tbb::filter_mode` / `tbb::parallel_pipeline`). On Windows, the build copies `tbb12.dll` next to `StructuredLogViewer.exe` automatically (both into the build tree and the install tree) — keep it next to the executable when redistributing.

#### Building

The project ships a [`CMakePresets.json`](CMakePresets.json) that defines shared configure/build/test/workflow presets, so a single command can configure, build, and run the test suite:

```sh
cmake --workflow --preset release
```

Available workflows are `release`, `debug`, and `relwithdebinfo`; each writes to its own `build/<preset>/` directory. All presets use the Ninja generator, so `ninja` must be on your `PATH` (installed by default with modern Qt / Visual Studio, or available via your system package manager).

On Windows, run the command from the **Developer PowerShell** (or **Developer Command Prompt**) for Visual Studio 2022 so MSVC is on `PATH`.

For machine-specific overrides (e.g. pinning `CMAKE_PREFIX_PATH` to your Qt install), create a personal `CMakeUserPresets.json` at the repo root — it is gitignored. Qt Creator, CLion, and VS Code all discover these presets automatically. See [CONTRIBUTING.md](CONTRIBUTING.md#building) for a worked example and per-step commands.

### Contributing

Contributions are welcome! See [CONTRIBUTING.md](CONTRIBUTING.md) for detailed build instructions, test setup, coding style, and the release process.

## License

This project is licensed under the MIT License. See the [LICENSE](LICENSE) file for details.

## Acknowledgments

- [HowardHinnant/date](https://github.com/HowardHinnant/date) — timezone-aware timestamp parsing/formatting
- [fmtlib/fmt](https://github.com/fmtlib/fmt) — formatting library
- [simdjson/simdjson](https://github.com/simdjson/simdjson) — fast JSON parsing
- [stephenberry/glaze](https://github.com/stephenberry/glaze) — JSON serialization for configuration
- [mandreyel/mio](https://github.com/mandreyel/mio) — header-only memory-mapped file I/O
- [Tessil/robin-map](https://github.com/Tessil/robin-map) — fast hash map
- [uxlfoundation/oneTBB](https://github.com/uxlfoundation/oneTBB) — Threading Building Blocks (parallel parsing pipeline)
- [catchorg/Catch2](https://github.com/catchorg/Catch2) — unit testing and benchmarking
- [Icon by Ilham Fitrotul Hayat](https://www.freepik.com/icon/file_5392654)
