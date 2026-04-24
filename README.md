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

#### Building

It is recommended to use Qt Creator for development (open the top-level `CMakeLists.txt`).

To build manually:

```sh
mkdir build
cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build . -j
```

On Windows, use the Developer PowerShell or Developer Command Prompt for VS 2022. See [CONTRIBUTING.md](CONTRIBUTING.md#building) for platform-specific notes and test instructions.

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
- [catchorg/Catch2](https://github.com/catchorg/Catch2) — unit testing and benchmarking
- [Icon by Ilham Fitrotul Hayat](https://www.freepik.com/icon/file_5392654)
