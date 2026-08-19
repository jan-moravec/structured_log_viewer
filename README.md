# Structured Log Viewer

[![Build](https://github.com/jan-moravec/structured_log_viewer/actions/workflows/build.yml/badge.svg?branch=main)](https://github.com/jan-moravec/structured_log_viewer/actions/workflows/build.yml)
[![Coverage](https://codecov.io/gh/jan-moravec/structured_log_viewer/branch/main/graph/badge.svg)](https://codecov.io/gh/jan-moravec/structured_log_viewer)
[![OpenSSF Scorecard](https://api.securityscorecards.dev/projects/github.com/jan-moravec/structured_log_viewer/badge)](https://securityscorecards.dev/viewer/?uri=github.com/jan-moravec/structured_log_viewer)
[![OpenSSF Best Practices](https://img.shields.io/badge/OpenSSF%20Best%20Practices-track%20on%20bestpractices.dev-blue)](https://www.bestpractices.dev/en/projects/new)
[![GitHub Releases](https://img.shields.io/github/release/jan-moravec/structured_log_viewer.svg)](https://github.com/jan-moravec/structured_log_viewer/releases)
![Downloads](https://img.shields.io/github/downloads/jan-moravec/structured_log_viewer/total)
![Platforms](https://img.shields.io/badge/platforms-Linux%20%7C%20Windows%20%7C%20macOS-blue)
![C++23](https://img.shields.io/badge/C%2B%2B-23-blue.svg)
![Qt 6.8](https://img.shields.io/badge/Qt-6.8-41cd52?logo=qt&logoColor=white)
[![License: MIT](https://img.shields.io/github/license/jan-moravec/structured_log_viewer.svg)](LICENSE)

## Overview

Structured Log Viewer is a C++ application consisting of a reusable library (`loglib`) for handling structured log data and a Qt-based GUI (`StructuredLogViewer`) for viewing, searching, and filtering the logs.

Four ingestion modes are available:

- **Static mode** (`File → Open…`, drag & drop) opens one or more finished log files and parses them in parallel with the TBB pipeline. Any file whose bytes start with a supported codec's magic number (`.gz` / `.bz2` / `.xz` / `.zst`, detected by content rather than extension) is transparently decompressed through `loglib::internal::DecompressingByteSource` before parsing, with a cancellable per-window progress dialog and a 32 GiB zip-bomb cap. Use this for post-mortem analysis of complete logs.
- **Stream Mode** (`File → Open Log Stream…`) tails a single file that is still being written, pre-fills the last *N* complete lines, then appends every new line as it arrives. It survives `logrotate` rotations, supports Pause / Follow newest / Stop, and bounds memory via a configurable retention cap. Use this when watching a live service.
- **Standard-input mode** (`StructuredLogViewer -` / `--stdin`) reads a pipe or redirected file with the same streaming controls and retention cap as Stream Mode. The format is detected from the first 16 KiB.
- **Network Stream Mode** (`File → Open Network Stream…`, `Ctrl+Shift+L`) listens on a TCP or UDP port and ingests structured logs pushed by your application. TCP supports many concurrent clients with line-granular interleaving and (when the binary is built with `LOGLIB_NETWORK_TLS=ON`) optional TLS via `asio::ssl`; UDP is one record per datagram and plaintext-only. Use this for distributed systems where redirecting stdout/stderr to a log file is not practical, or when you want a quick local-loopback firehose during development.

File opens automatically load numbered and dated rotations as older history. This also works in Stream Mode, where the active file is tailed after its history loads. See [Auto-loading rotated history](doc/README.md#auto-loading-rotated-history), including how to opt out.

Two dockable visualisations complement the main table: the **Histogram** dock (`View → Histogram`, `Ctrl+H`) plots row count per time bucket, stacked and coloured by canonical log level, with click-to-jump and drag-to-time-filter over the first `Type::Time` column; and the **Overview Rail** (`View → Overview Rail`, `Ctrl+Shift+R`, on by default) is a thin vertical strip along the right edge of the table that condenses the whole proxy-filtered stream into level-tinted buckets with anchor bands, current-search tick marks, and a live viewport indicator. Both surfaces stay in lock-step with the sort, filter, and anchor state.

**Tabs** put several independent investigations in one window. `Ctrl+T` opens a new empty tab, `Ctrl+W` closes the current one, and `Ctrl+Tab` / `Ctrl+Shift+Tab` (also `Ctrl+PgDown` / `Ctrl+PgUp`) cycle between tabs. Every source mode can run concurrently in separate tabs. Dropped files target the active tab: the default appends and holding `Shift` replaces. Tab labels use the same automatic names as Recent Sessions (file basename, `name + N more`, or a stdin / network locator). Rename a tab from **File → Rename Tab**, the tab context menu, or `F2` while the tab strip is focused; custom titles persist with the workspace. They also include a localized operation word (Ingesting, Paused, Failed, and so on); glyphs may prefix the label but are not the only status. Unsaved changes show `●` and parse errors show `!`. Closing a dirty tab that cannot be autosaved offers **Save** / **Discard** / **Cancel**. Clean tabs and restorable file-backed tabs close silently. Closing the last tab closes the window after that same decision. Ordered windows and tabs restore on the next launch: file-backed sessions reopen, a saved live-tail tab reopens as a static file from that path, and network / stdin entries restore as empty tabs that keep their last automatic name. See [`doc/README.md § Tabs`](doc/README.md#tabs).

**Session bundles** (`File → Export Session Bundle…`, `Ctrl+Shift+E`) package every retained row with columns, filters, sort, anchors, and highlight rules in one portable `.slvbundle` file. Reopening the bundle restores the configured view without the original logs.

Four structured-log formats ship out of the box:

- **JSON Lines / NDJSON** — one JSON object per line.
- **logfmt** — Heroku / `kr/logfmt`-style `key=value` fields. By default, indented physical lines continue the previous record's last source-order field.
- **CSV** — strict comma-separated RFC 4180 with a required header row. Quoted cells cannot span physical lines.
- **Regex templates** — PCRE2 patterns with named captures. Templates can keep one record per line, fold indented lines, or fold everything until the next matching header.

The 28 built-in regex templates cover syslog, Apache/nginx access and error logs, Google glog, PostgreSQL, MySQL/MariaDB, HAProxy, Uber Zap, Rails, Redis, Java / log4j / Logback, spdlog, Rust `env_logger`, Cisco ASA, BIND9, iptables, Squid, AWS access logs, NetScreen, Exim, MongoDB, and Docker / containerd CRI. Eleven opt into multi-line continuations. Sources include [lnav](https://github.com/tstack/lnav), [logstash-patterns-core](https://github.com/logstash-plugins/logstash-patterns-core), and vendor documentation.

Static files, live-tail files, stdin, and network streams (Auto-detect by default) detect the format from content. Probes run in this order: JSON Lines, logfmt, regex templates, then CSV. `Settings → Regex templates...` manages the merged built-in and user catalog. User JSON files live under `<AppDataLocation>/regex_templates/`, shadow same-named built-ins, and can join auto-detection in `priority` order.

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

The planned feature work between today and `v1.0` (plus the post-`v1` themes) lives in [ROADMAP.md](ROADMAP.md), including a [comparative feature matrix](ROADMAP.md#comparative-feature-matrix) against the most-cited desktop and TUI log viewers (lnav, Klogg, LogExpert, LogViewPlus, OtrosLogViewer, QLogExplorer, LogSleuth, Logan). New contributors looking for a starting point should pick a [Tier 1](ROADMAP.md#tier-1--pre-v1-release-blocking-ergonomics) or [Tier 2](ROADMAP.md#tier-2--v1x-strong-differentiators) item.

## License

This project is licensed under the MIT License. See the [LICENSE](LICENSE) file for details.

Distributed binaries also include a `THIRD_PARTY_LICENSES.txt` aggregating the license texts of every bundled third-party library (Qt under LGPL-3.0, simdjson / oneTBB / OpenSSL under Apache-2.0, fmt / glaze / mio / robin-map / Howard Hinnant's date / argparse / efsw under MIT, Asio / Catch2 under BSL-1.0, PCRE2 under BSD-3-Clause). The file is generated by [`cmake/BundleLicenses.cmake`](cmake/BundleLicenses.cmake) and ships next to the executable in every packaged artifact.

## Acknowledgments

- [HowardHinnant/date](https://github.com/HowardHinnant/date) — timezone-aware timestamp parsing/formatting
- [fmtlib/fmt](https://github.com/fmtlib/fmt) — formatting library
- [simdjson/simdjson](https://github.com/simdjson/simdjson) — fast JSON parsing
- [stephenberry/glaze](https://github.com/stephenberry/glaze) — JSON serialization for configuration
- [mandreyel/mio](https://github.com/mandreyel/mio) — header-only memory-mapped file I/O
- [Tessil/robin-map](https://github.com/Tessil/robin-map) — fast hash map
- [uxlfoundation/oneTBB](https://github.com/uxlfoundation/oneTBB) — Threading Building Blocks (parallel parsing pipeline)
- [chriskohlhoff/asio](https://github.com/chriskohlhoff/asio) — standalone Asio (cross-platform networking for the TCP/UDP producers)
- [openssl/openssl](https://github.com/openssl/openssl) — TLS support for the TCP server producer (optional, gated by `LOGLIB_NETWORK_TLS=ON`)
- [catchorg/Catch2](https://github.com/catchorg/Catch2) — unit testing and benchmarking
- [PCRE2](https://github.com/PCRE2Project/pcre2) — regex engine (PCRE2-8 with JIT) backing the regex-template log format
- [lnav](https://github.com/tstack/lnav) (BSD-2-Clause) — reference patterns for the built-in syslog / Apache / glog / PostgreSQL / MySQL / HAProxy / Zap / Rails / Redis / Java / spdlog / env_logger regex templates
- [logstash-plugins/logstash-patterns-core](https://github.com/logstash-plugins/logstash-patterns-core) (Apache-2.0) — reference grok patterns informing the built-in regex catalog (Cisco ASA, BIND9, iptables, Squid, AWS S3 / Classic ELB / CloudFront, NetScreen, Exim, MongoDB)
- [nginx](https://nginx.org/) (BSD-2-Clause) — reference for the built-in nginx error log regex template (`src/core/ngx_log.c`)
- [Kubernetes cri-api](https://github.com/kubernetes/cri-api) (Apache-2.0) — reference for the built-in Docker / containerd CRI runtime regex template
- [Icon by Ilham Fitrotul Hayat](https://www.freepik.com/icon/file_5392654)
