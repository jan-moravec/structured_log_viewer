# Structured Log Viewer

[![Build](https://github.com/jan-moravec/structured_log_viewer/workflows/Build/badge.svg)](https://github.com/jan-moravec/structured_log_viewer/actions?query=workflow%3ABuild)
[![GitHub Releases](https://img.shields.io/github/release/jan-moravec/structured_log_viewer.svg)](https://github.com/jan-moravec/structured_log_viewer/releases)

## Overview

Structured Log Viewer is a C++ application consisting of a reusable library (`loglib`) for handling structured log data and a Qt-based GUI (`StructuredLogViewer`) for viewing, searching, and filtering the logs.

Two ingestion modes are available:

- **Static mode** (`File → Open…`, drag & drop) opens one or more finished log files and parses them in parallel with the TBB pipeline. Use this for post-mortem analysis of complete logs.
- **Stream Mode** (`File → Open Log Stream…`) tails a single file that is still being written, pre-fills the last *N* complete lines, then appends every new line as it arrives. It survives `logrotate` rotations, supports Pause / Follow newest / Stop, and bounds memory via a configurable retention cap. Use this when watching a live service.

Currently, only JSON Lines (one JSON object per line) logs are supported in both modes.

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

## Contributing

Contributions are welcome. See [CONTRIBUTING.md](CONTRIBUTING.md) for the developer reference: an [architecture overview](CONTRIBUTING.md#architecture) of the `loglib` core and the Qt GUI, [build instructions](CONTRIBUTING.md#building), how to [run the test suite](CONTRIBUTING.md#running-tests) and the [parser benchmarks](CONTRIBUTING.md#benchmarking), the [code style](CONTRIBUTING.md#code-style-and-pre-commit) and [pull-request workflow](CONTRIBUTING.md#pull-requests), and the [release process](CONTRIBUTING.md#release-process).

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
