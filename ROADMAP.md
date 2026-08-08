# Roadmap

This document tracks the feature work planned between today's `main` branch and the first stable (`v1.0`) release of Structured Log Viewer, plus the post-`v1` themes we want to grow into. It is **not** a release plan with dates — items move based on contributor bandwidth — but the ordering inside each tier reflects current priority, and every item names the existing seams (`loglib` types, Qt classes, configuration fields) where the work would land. New contributors looking for a starting point should pick an item from Tier 1 (release-blocking ergonomics) or Tier 2 (post-`v1` differentiators).

For the architecture each item plugs into, see [CONTRIBUTING.md → Architecture](CONTRIBUTING.md#architecture). For the user-facing feature inventory those items extend, see [`doc/README.md`](doc/README.md).

## Table of contents

- [Scope](#scope)
- [Competitive context](#competitive-context)
- [Themes](#themes)
- [Tier 1 — Pre-`v1` release-blocking ergonomics](#tier-1--pre-v1-release-blocking-ergonomics)
  - [1. ~~Transparent decompression of `.gz` / `.bz2` / `.xz` / `.zst`~~ (shipped)](#1-transparent-decompression-of-gz--bz2--xz--zst)
  - [2. ~~Histogram / activity-rate strip~~ (shipped)](#2-histogram--activity-rate-strip)
  - [3. ~~User-defined highlight rules~~ (shipped)](#3-user-defined-highlight-rules)
  - [4. ~~Bookmark notes on anchors~~ (shipped)](#4-bookmark-notes-on-anchors)
  - [5. ~~Boolean filter expressions (AND / OR / NOT)~~ (shipped)](#5-boolean-filter-expressions-and--or--not)
  - [6. ~~Multi-line records (stack traces and continuation lines)~~ (shipped)](#6-multi-line-records-stack-traces-and-continuation-lines)
  - [7. ~~Export filtered rows~~ (shipped)](#7-export-filtered-rows)
  - [8. ~~Goto line / Goto timestamp~~ (shipped)](#8-goto-line--goto-timestamp)
  - [9. ~~Stdin / pipe input~~ (shipped)](#9-stdin--pipe-input)
  - [10. Headless / scriptable CLI mode](#10-headless--scriptable-cli-mode)
- [Tier 2 — `v1.x` strong differentiators](#tier-2--v1x-strong-differentiators)
  - [11. Pulling rotated history off disk](#11-pulling-rotated-history-off-disk)
  - [12. Saved searches and named views](#12-saved-searches-and-named-views)
  - [13. ~~Match overview rail / minimap~~ (shipped)](#13-match-overview-rail--minimap)
  - [14. Per-cell quick filter](#14-per-cell-quick-filter)
  - [15. Inline pretty-print for embedded JSON / XML](#15-inline-pretty-print-for-embedded-json--xml)
  - [16. Tabs for multiple sources in one window](#16-tabs-for-multiple-sources-in-one-window)
  - [17. Time-gap detection](#17-time-gap-detection)
  - [18. Pattern clustering / similar-line grouping](#18-pattern-clustering--similar-line-grouping)
  - [19. Encoding auto-detection](#19-encoding-auto-detection)
  - [20. Diff view between two sources](#20-diff-view-between-two-sources)
- [Tier 3 — longer horizon](#tier-3--longer-horizon)
  - [21. SQL query interface](#21-sql-query-interface)
  - [22. Remote sources (SSH / SFTP)](#22-remote-sources-ssh--sftp)
  - [23. Plugin / extension system](#23-plugin--extension-system)
  - [24. Search & filter history dropdown](#24-search--filter-history-dropdown)
  - [25. Timeshift (per-source clock offset)](#25-timeshift-per-source-clock-offset)
  - [26. Triggers / actions on pattern match](#26-triggers--actions-on-pattern-match)
  - [27. Scratchpad / notes panel](#27-scratchpad--notes-panel)
  - [28. Pipe selection to external command](#28-pipe-selection-to-external-command)
  - [29. AI / LLM assistance panel](#29-ai--llm-assistance-panel)
  - [30. ~~Full-session export bundle~~ (shipped)](#30-full-session-export-bundle)
- [Explicit non-goals](#explicit-non-goals)
- [Process](#process)
- [Comparative feature matrix](#comparative-feature-matrix)

## Scope

Structured Log Viewer is a **desktop application for inspecting structured logs** on developer / SRE workstations. The roadmap is bounded by that scope:

- **In scope.** Parsing common structured log formats, displaying them as a sortable / filterable table, interactive triage workflows (search, filter, highlight, bookmark, drill-down), live tailing of local and network sources, persistence of view state across sessions, and the ergonomics that make the table comfortable on multi-million-row files.
- **Out of scope.** Centralised log storage, alerting, dashboards, multi-user collaboration, server-side ingestion pipelines, or anything else that turns the tool into an observability platform. Those are well covered by Splunk / Elastic / Loki / Datadog; we deliberately stop at the desk.

See [Explicit non-goals](#explicit-non-goals) for the long-tail list.

## Competitive context

The roadmap is informed by a feature sweep of the most-cited desktop and TUI log viewers (see [Comparative feature matrix](#comparative-feature-matrix)). The short version:

- **[lnav](https://github.com/tstack/lnav)** sets the bar for *structured-log* power features: SQL queries over rows, histogram + timeline views, automatic format detection across 70+ formats, gzip / bzip2 auto-decompression, and a headless scriptable mode.
- **[Klogg](https://github.com/variar/klogg)** (and its glogg ancestry) sets the bar for *unstructured-text* ergonomics: tabs, user-defined highlighter sets, boolean regex composition, encoding auto-detection, match overview rail, scratchpad.
- **[LogExpert](https://github.com/zarunbal/LogExpert)** is the historical Windows tail with bookmarks-with-comments, columnizer plugins, timeshift, and triggers.
- **[OtrosLogViewer](https://github.com/otros-systems/otroslogviewer)** is the Java/Log4j reference with remote sources (SSH/SFTP/SMB), socket listener, notes-on-rows, and IDE jump-to-code.
- **[QLogExplorer](https://github.com/rafaelfassi/qlogexplorer)**, **[LogSleuth](https://github.com/swatto86/LogSleuth)**, **[Logan](https://github.com/SolidKeyAB/logan)**, **[Nerdlog](https://github.com/dimonomid/nerdlog)** are the newer entrants pushing JSON-native browsing, modern UX (minimap, virtual scroll), pattern clustering, and AI integration.

The roadmap aims to close the **mainstream desktop log-viewer expectations** before `v1` (Tier 1), then differentiate on the structured-log axis we already lead on (Tier 2 / 3).

## Themes

Beyond the per-item list, three themes run through the roadmap:

1. **Pre-release ergonomics.** Close the "table stakes" gaps any reviewer will flag in a head-to-head against Klogg or lnav: compressed files, histogram strip, highlight rules, bookmark notes, AND/OR filters, ~~multi-line records~~ (shipped), filtered/session export, goto, ~~stdin~~ (shipped), headless mode.
1. **Structured-log power user.** Lean into what `loglib` already does well — typed columns, level promotion, the regex-template registry — with features that only make sense on structured data: SQL queries over typed rows, per-cell quick filters, pattern clustering by template key, time-gap detection across the first `Type::Time` column.
1. **Scale and performance.** Preserve the existing performance envelope (see [CONTRIBUTING.md → Benchmarking](CONTRIBUTING.md#benchmarking)) as features land. Each Tier 1 / 2 item below documents whether it needs a new benchmark or a regression check against the [Acceptance bar](CONTRIBUTING.md#acceptance-bar).

## Tier 1 — Pre-`v1` release-blocking ergonomics

Each of the ten items below closes a gap that reviewers and first-time users routinely flag against direct competitors. They should all land before `v1.0`. Items are listed in suggested implementation order; the order also roughly tracks how much code surface each touches.

### 1. ~~Transparent decompression of `.gz` / `.bz2` / `.xz` / `.zst`~~

> **Shipped.** Static `File → Open…`, drag & drop, CLI arguments, and session restore all transparently decompress `.gz`, `.bz2`, `.xz`, and `.zst` (magic-byte detection, extension-agnostic). Backing bits: `loglib::internal::DecompressingByteSource` (streams to a RAII temp file under `std::filesystem::temp_directory_path()`), a `QtConcurrent::run` worker orchestrated from `MainWindow::StreamNextPendingFile`, and a modal-per-window `QProgressDialog` with **Cancel** wired to a `loglib::StopSource`. Session locators always store the *original* compressed path — the temp path is a per-open implementation detail. See [`doc/README.md § Compressed inputs`](doc/README.md#compressed-inputs) for the user-facing surface.

**Why.** Logrotate-style deployments compress every retained segment (`app.log.1.gz`, `app.log.1.zst`, ...). Today the app cannot open them at all. lnav, Klogg, LogViewPlus, OtrosLogViewer all do this transparently. This is the single biggest "first impression" gap.

**Scope.** Static (`File → Open…`, drag & drop) for `.gz`, `.bz2`, `.xz`, `.zst`. Detection by **content magic bytes** (not extension), so `app.log` that happens to be gzipped still opens correctly. Read-only — the app never writes compressed bytes.

**Non-goals (v1).** `.zip` / `.tar.gz` multi-member archives (extract first), encrypted archives, live-tail of a compressed file (the producer would need to re-decompress on every truncate; defer to a v1.x ticket if there's demand).

**Approach.** `DecompressingByteSource` sniffs codec magic before `loglib::DetectFormatForPath` runs. It streams decoded bytes to a temp file so the static mmap + TBB pipeline sees a normal file. Dependencies prefer system `zlib` / `libbz2` / `liblzma` / `libzstd`, with FetchContent fallbacks.

**Acceptance bar.** Open one ~500 MiB JSONL file compressed with each of the four codecs in `< 2 ×` the time of the uncompressed equivalent. Existing parser benchmarks unchanged (the decompression path is upstream of them). Unit tests for each codec with truncated / corrupt input must surface a parse error rather than crash.

**Touches.** `loglib`: `internal/decompressing_byte_source.hpp` + `.cpp`. `app`: `MainWindow::StreamNextPendingFile` and async decompression helpers. Docs: [`doc/README.md`](doc/README.md) supported-extensions section.

### 2. ~~Histogram / activity-rate strip~~

> **Shipped.** Bottom-docked `HistogramDock` renders stacked per-level bars over time for the first `Type::Time` column. Bucket rungs cycle through `1 s` .. `1 d` via `z` / `Shift+Z` (and `Ctrl+wheel`); `AutoBucketSize` picks a rung that fits ~500 columns on open. Click jumps the table to the first row in the bucket; drag brushes a time range and installs a `Type::Time` filter through `LogFilterModel`. Backing bits: `loglib::HistogramBucketIndex` (rebuilds 1 M rows in `< 5 ms`), `HistogramModel` (subscribes to `LogModel::rowsInserted` / `rowsRemoved` / `modelReset` with 50 ms coalesce), `HistogramWidget` (custom `paintEvent`, per-level palette from `ThemeControl::BackgroundFor`), and `HistogramDock` wired through `MainWindow::WireDockToggle`. See [`doc/README.md § Histogram / activity-rate strip`](doc/README.md#histogram--activity-rate-strip) for the user-facing surface.

**Why.** The single most-cited "structured-log power user" feature, present in lnav, LogViewPlus, Logan, Acacia, Nerdlog. Bucketed message counts over time, coloured by level, give triage an immediate visual anchor: spot the spike, click to jump.

**Scope.** A dockable strip (`HistogramDock`) along the bottom that shows the row count per time bucket for the first `Type::Time` column, segmented and coloured by the canonical `LogLevel`. Click-or-drag selection on the strip drives a time-range filter on the table. Bucket size auto-picks between `1 s` / `10 s` / `1 min` / `10 min` / `1 h` / `1 d` based on the visible range, with `z` / `Shift+Z` to zoom in / out.

**Non-goals (v1).** Custom bucket sizes, multi-column / multi-source overlay charts, persisting strip state across sessions (v1.x).

**Approach.** Build atop the existing `Type::Time` column promotion and the canonical level mapping (`level_canonical.hpp`). A new `HistogramModel` subscribes to `LogModel::rowsInserted` / `rowsRemoved` / `modelReset` and maintains a `std::vector<LevelBucket>` keyed by truncated timestamp. Rendering uses `QtCharts` (already pulled in by Qt 6.8) or a hand-rolled `QGraphicsScene` to keep dependencies minimal. The brush palette comes from `ThemeControl::levelBrush(...)` so the strip respects Light / Dark / High-contrast level palettes.

**Acceptance bar.** On a 1 M-row JSONL file the strip rebuilds in `< 50 ms` after the parse finishes. Live-tail updates the strip at the existing 100 ms batch cadence without dropping frames. Click-to-jump lands the table on a row whose timestamp falls inside the clicked bucket.

**Touches.** `app`: `app/include/histogram_dock.hpp` + `.cpp`, hook into `MainWindow::WireDockToggle`. `loglib`: nothing — the level mapping and time column already exist.

### 3. ~~User-defined highlight rules~~

> **Shipped.** `Settings → Highlight rules…` opens a modeless `HighlightRulesEditor` (list + inline form, palette-swatch pickers, move up / down, dirty-guard on close). Rules are stored on `LogConfiguration::highlightRules` (Configuration-scope; ride with both `SaveScope::Full` and `SaveScope::ColumnsOnly` saves) and evaluated at runtime by `HighlightRuleSet`, which resolves columns by stable `Column::keys`, compiles predicates via the shared `MakeStringMatcher` / `RowPredicate` infrastructure, and caches per-row last-match indices. `LogModel::data` layers the highlight brush / font on top of the level palette (anchors still win). Rules re-bind automatically after `AppendKeys` / enum-type flips. Colours come from a new 16-slot `Theme::highlightPalette`. See [`doc/README.md § Highlight rules`](doc/README.md#highlight-rules) for the user-facing surface.
>
> **v1 scope not yet lifted into the editor.** Time and Enumeration match specs render correctly through the parse / render path but are read-only in the editor form; a follow-up will unlock them. `LogFilter::row` still uses index-based identity (Session-scope); migrating that to keys-based identity is a separate ticket now that the pattern is proven by `HighlightRuleSet::ResolveColumnByKeys`.

**Why.** Standard since glogg / Klogg / LogExpert. The current app only colours rows by `LogLevel`. Power users want to tag "their" subsystem with a colour (`service=auth → blue`), highlight specific request IDs while triaging an incident, or visually mark slow requests (`duration > 500`).

**Scope.** A list of rules, each with: a name, a match expression (regex on a chosen column or a typed comparator on numeric / timestamp columns), a foreground / background brush picked from the active theme, optional bold / italic. Rules are applied in order, last-match-wins per row. Rules persist with the configuration (alongside columns and filters) so a session restores its highlighters.

**Non-goals (v1).** Multi-condition AND/OR per rule (covered by item 5's expression engine when that lands), cell-only highlighting (rows only in v1), trigger actions (item 26).

**Approach.** Reuse `LogFilterModel`'s per-row predicate plumbing but in a separate "render rule" path so filtered-out rows don't pay any cost. A new `HighlightRule` struct lives in `loglib::LogConfiguration` (round-tripped through the existing Glaze meta). A new `HighlightRulesEditor` (modelled on `RegexTemplatesEditor`) hosts the list. The `QStyledItemDelegate` on the table reads the per-row highlight result via `LogFilterModel::data(role=HighlightBrushRole)` and falls back to the level brush when no rule matches.

**Acceptance bar.** 10 active highlight rules on a 1 M-row file do not regress the scroll-frame budget by more than 5 %. Saved configurations from before the feature lands (no `highlightRules` field) round-trip without warnings.

**Touches.** `loglib`: extend `LogConfiguration` with a `highlightRules` vector + Glaze meta. `app`: `app/include/highlight_rules_editor.hpp` + `.cpp`, `LogFilterModel::data` hook, `MainWindow::ApplyTableStyleSheet` to skip the alt-row tint when a row carries a custom brush.

### 4. Bookmark notes on anchors

**Status: shipped.** Each anchor now carries an optional one-line `note` alongside its colour. Surfaces: the `AnchorsDock` second column (inline-editable, `F2` / double-click), the `RecordDetailDock` italic subline (with the note included in **Copy as key/value** as a synthetic `anchor.note:` prefix line), and the row tooltip. The row right-click **Anchor** sub-menu gains an **Edit note…** entry; `F4` on a focused anchored row opens the same editor (guarded against firing while a text editor holds focus so an in-flight edit is never dropped). Notes round-trip through `LogConfiguration::AnchorEntry` (a new `note` field), and `error_on_unknown_keys=false` keeps sessions saved by pre-note builds loading cleanly with empty notes. Notes are one-line by contract — CR / LF / tab collapse to a single space and edge whitespace is trimmed on write, enforced by `AnchorManager::SanitiseNote`. See [doc/README.md → Anchor notes](doc/README.md#anchor-notes) for the user-facing shape.

### 5. ~~Boolean filter expressions (AND / OR / NOT)~~

**Status: shipped.** Filters now compose as a full boolean tree (`loglib::FilterExpression` = `Leaf` / `And` / `Or` / `Not`). The default simple-mode UX stays "click + to add another AND rule"; a new **Filters -> Advanced Filter…** action opens the `AdvancedFilterEditor`, a text-first dialog backed by a hand-rolled recursive-descent parser (`library/src/query_parser.cpp`, grammar in `library/include/loglib/query_parser.hpp`). Queries look like `level in [Warn, Error] AND (service:auth OR service:db) AND NOT msg~/heartbeat/`; every keystroke reparses and the OK button gates on a clean parse. Evaluation goes through `CompiledFilterExpression`, which sorts children cheap-first (`EstimatedLeafCost`) and picks between a per-row visit path and a **bitset-materialisation fast path** (packed `RowBitset` per unique leaf, folded with word-parallel AND/OR/NOT). The bitset path engages for trees containing `OR` or `NOT` with at least two leaves, subject to a 512 MiB memory budget; flat `And` trees stay on the short-circuiting visit path. `LogConfiguration::filters` is gone; on-disk the field is `expression` (internally tagged variants under a `kind` discriminator). See [doc/README.md -> Advanced filter query syntax](doc/README.md#advanced-filter-query-syntax) for the user-facing shape.

### 6. ~~Multi-line records (stack traces and continuation lines)~~

**Status: shipped.** Static files, live tail, and network streams now share a continuation-aware decoder contract. `LineDecodeResult::Continue` extends the pending row instead of emitting another row. The streaming loop defers a row for at most 500 ms; static Stage C holds batch tails and stitches cross-batch continuations. Static raw-byte lookup uses `StreamedBatch::multiLineSpans` and `LogFile::RegisterMultiLineRecord`.

**Parser coverage.**

- `RegexTemplate::continuationMode` accepts `"none"` (default), `"indented"`, and `"untilNextHeader"`. The last mode uses `PCRE2_ANCHORED | PCRE2_PARTIAL_HARD` against the main pattern or optional `headerAnchor`.
- Continuations append to the last named capture in source order. If that capture is missing or classifies as a non-string value for a header, its continuations are dropped with one error summary.
- Seven built-ins use `"indented"`: Apache error, Generic bracketed level, Java / log4j / Logback, MongoDB 3.x, MySQL / MariaDB error, PostgreSQL, and spdlog.
- Four use `"untilNextHeader"`: Google glog, Rust `env_logger`, Ruby on Rails, and Uber Zap.
- Logfmt folds indented lines into the previous record's last source-order key by default. `ParserOptions::multilineLogfmt = false` restores the old row-per-line behavior.
- JSON Lines and CSV remain line-framed and never emit `Continue`.

Blank lines join a record only when followed by another continuation; trailing and between-record blanks remain separators. A run of continuations before any header produces one summarized orphan error.

**Copy and details.** A single-row **Edit → Copy** preserves the joined text. Multi-row copy escapes backslashes plus embedded CR/LF so every selected record occupies one clipboard line. Record Details receives the same joined raw text.

**Benchmarks.** `[java_multiline]` and `[untilNextHeader]` each parse 1,000,000 records, with every tenth record carrying continuations, and follow the manual ±3% `[large]` before/after convention. `[header_anchor]` compares full-pattern and dedicated-anchor probes and enforces a 1.03× parse-loop throughput floor.

**Non-goals.** Regex matching still happens per physical line; continuation handling appends bytes after the header match. JSON/CSV record framing and expanded in-cell rendering are unchanged.

### 7. ~~Export filtered rows~~

> **Shipped.** **File → Export Filtered Rows…** exports the current
> filtered + sorted view (or just the selection) as **JSON Lines**,
> **CSV**, **Source snapshot** (raw on-disk bytes per row, same shape
> as `Ctrl+C`), or **Markdown table**. The dialog previews the row
> count, remembers the last format via `QSettings`, suggests an
> extension per format, and warns on large Markdown exports. Progress
> and cancel run through the same `QFutureWatcher` +
> `loglib::StopSource` + deferred `QProgressDialog` pattern as async
> decompression; cancellation leaves no partial file on disk because
> `FileSink` writes to `<dest>.tmp` and atomically renames on
> `Finish`. See
> [`doc/README.md § Exporting Filtered Rows`](doc/README.md#exporting-filtered-rows).

**Non-goals (v1).** Streaming export (continuous flush to disk as
new lines arrive — defer to v1.x), exports with attachments
(anchors / notes), and per-column transformations. Full-session
export shipped separately as [item 30](#30-full-session-export-bundle).

### 8. ~~Goto line / Goto timestamp~~

> **Shipped.** **Edit → Go to Line…** (`Ctrl+G`) jumps to a 1-based source-model row (line 1 is the earliest retained row; streaming FIFO eviction may have dropped older rows so numbers need not match the source file). **Edit → Go to Timestamp…** (`Ctrl+Shift+G`) accepts every format the timestamp column already parses, two ISO 8601 fallbacks (`%FT%T` and `%F %T`), and the relative shortcuts `-Nh` / `-Nm` (case-insensitive; `+N` and bare `N` also mean "N ago", matching lnav / less). Naive inputs (no `%z` / `%Z`) are interpreted in the display TZ (`loglib::CurrentZone()`) and shifted to UTC via `loglib::LocalMicrosecondsSinceEpochToUtc`. Search takes one of three branches: an O(log N) source-row binary search when the timestamps are monotonic and no user sort is active (guarded by `LogModel::TimestampsAreMonotonic()`, which flips false on multi-file `Append` / rotation / clock skew); an O(N_visible) outer-proxy walk when monotonicity has broken; an O(N_visible) display-order scan under a user sort. See [`doc/README.md § Jumping to a Line or Timestamp`](doc/README.md#jumping-to-a-line-or-timestamp).

### 9. ~~Stdin / pipe input~~

> **Shipped.** `StructuredLogViewer -` and `StructuredLogViewer --stdin` open a live-tail session over standard input with 16 KiB format detection, JSON Lines fallback, implicit `--new-instance`, no session persistence, and the same controls as file tailing. The Network Stream dialog also defaults to Auto-detect.

See [`doc/README.md § Reading from standard input`](doc/README.md#reading-from-standard-input) for shell examples.

### 10. Headless / scriptable CLI mode

**Why.** lnav's `-n` / `-c` flags are the most cited reason engineers script lnav into CI / triage runbooks. A minimal headless mode unlocks one-shot triage (`structured_log_viewer --filter 'level=error' --since '-1h' --export errors.jsonl`) without spinning up Qt.

**Scope.** A `--no-gui` (or `-n`) flag that runs the existing `loglib` pipeline against the requested source, applies filters / sort, then writes the result through the [item 7](#7-export-filtered-rows) export path and exits. Flags:

- `--source <path | ->` (file, stdin, or `tcp://...` / `udp://...`).
- `--format <json|logfmt|csv|regex>` (optional override for auto-detect).
- `--filter <expression>` (uses the [item 5](#5-boolean-filter-expressions-and--or--not) text syntax).
- `--since <timestamp|relative>` / `--until <timestamp|relative>`.
- `--columns <a,b,c>` (whitelist).
- `--sort <column>[:asc|desc]`.
- `--export <format>` and `--out <path>` (or stdout if `-`).
- Exit code is non-zero on parse failure or no matching rows (configurable via `--allow-empty`).

**Non-goals (v1).** SQL queries (item 21 — when SQL lands, expose it as `--sql`), live tail in headless (`--follow` defers to v1.x), interactive prompts.

**Approach.** Build a separate `slv` binary that links against `loglib` only (no Qt dependency). The existing `loglib::ParseFile` / streaming harnesses already do all the work; the binary is a thin argument parser + filter evaluator + exporter. The two binaries (`StructuredLogViewer` GUI, `slv` CLI) ship in the same package.

**Acceptance bar.** Round-trip parity with the GUI: a filter expression that selects 1234 rows in the GUI exports exactly 1234 rows on the CLI. Cold-start `< 100 ms` overhead vs the raw `loglib` benchmarks.

**Touches.** New `cli/` directory mirroring `app/`'s structure. `cmake/`: a new `slv` target. CI: extend the build matrix to build the CLI on every platform.

## Tier 2 — `v1.x` strong differentiators

These items aren't required to ship `v1` but are the most-requested follow-ups in alternative tools' issue trackers. The order is suggested but not strict; pick whichever one a contributor is most motivated to land.

### 11. Pulling rotated history off disk

When the user opens `app.log`, also surface `app.log.1`, `app.log.2`, `app.log.1.gz`, `app.log-2025-04-28`, etc. as the older prefix of the merged view. Detection is by sibling-filename glob (`<stem>.[0-9]+(.gz|.bz2|.xz|.zst)?` and the dated variants). The configuration grows an optional `rotationFollowMode` so users can opt out per session. Works with [item 1](#1-transparent-decompression-of-gz--bz2--xz--zst) for the compressed companions. Lnav is the reference; the existing static multi-file merge already provides the join logic.

### 12. Saved searches and named views

Sessions already persist a single filter set. Real triage needs a small library of named presets: `@errors`, `@my-service`, `@slow-requests`, swappable from a dropdown next to the filter bar. The chosen "view" applies its filters + sort + visible-column set in one click. Views live in a new section of the user configuration; the global library lives under `<AppDataLocation>/views/*.json` with the same shadowing rules as themes and regex templates.

### 13. ~~Match overview rail / minimap~~

> **Shipped.** Vertical strip to the right of the table showing matches, anchors, and level-coloured bands over the whole proxy stream, with a click / drag / wheel viewport indicator. On by default; toggle from **View → Overview Rail** (`Ctrl+Shift+R`). See [`doc/README.md § Match overview rail`](doc/README.md#match-overview-rail).

### 14. Per-cell quick filter

Right-click on a cell → **Filter on this value** / **Hide rows with this value** / **Show only this column's value here**. Today the header right-click adds an empty filter dialog; a per-cell variant pre-populates the dialog with the cell's value and either commits immediately (`Filter on this value`) or opens the editor (`Refine filter…`). Two-click "drill into noise" is the most common triage motion.

### 15. Inline pretty-print for embedded JSON / XML

`RecordDetailDock` already pretty-prints JSON. Surface the same renderer per-cell: hovering a long JSON / XML payload shows a popup with the pretty-printed view; an **Expand** affordance pins the popup so multiple cells can be inspected at once. Lnav's `Shift+P` and LogViewPlus's pretty-print are the references. Works particularly well on logfmt / regex rows whose `message` column carries an embedded JSON object.

### 16. Tabs for multiple sources in one window

Today multiple sources require multiple windows (`File → New Window`). Tabs are more conventional and reduce window clutter, especially on tight monitor setups. The Recent Sessions submenu is the natural feed for "reopen as tab". Each tab keeps its own `LogModel` + filter set + sort, so the existing model layer needs no change; the tab bar is a `QTabWidget` wrapping the current single-`MainWindow` body.

### 17. Time-gap detection

A small dialog ("show me the 10 longest silences") that scans the first `Type::Time` column for the largest inter-row deltas and surfaces them as jump targets. Logan, Acacia, and `logtimeline` all ship a variant. Pairs naturally with the [histogram strip](#2-histogram--activity-rate-strip) — gaps in the activity rate are visually obvious there, but the dialog enumerates them with timestamps and durations.

### 18. Pattern clustering / similar-line grouping

"Collapse 5 000 near-identical messages into one row with count" is a huge triage win on noisy logs. logq, Logan, Acacia. Heuristic: tokenise the row's `message` column, replace numbers / timestamps / UUIDs / hex with placeholders, group by the resulting template. The grouped view is a separate proxy model so the underlying rows are not destroyed; clicking a group expands to its constituent rows.

### 19. Encoding auto-detection

Today the app assumes UTF-8. Klogg / LogExpert / OtrosLogViewer / LogViewPlus auto-detect UTF-16 LE/BE, cp125x, and friends — Windows event-log exports and many vendor tools emit UTF-16 by default. Use [`uchardet`](https://www.freedesktop.org/wiki/Software/uchardet/) (BSD-style licence, same library Klogg uses) and convert into a UTF-8 staging buffer in the `LineSource`, upstream of the parsers. The parsers themselves stay UTF-8.

### 20. Diff view between two sources

Open two log files (or two sessions) side by side with aligned matched / added / removed regions. Logan and several VS Code extensions ship variants. Useful for "what changed between this run and the last green one". A diff-by-message-template variant (using [item 18](#18-pattern-clustering--similar-line-grouping)'s clustering) is more useful than line-by-line diff on noisy logs.

## Tier 3 — longer horizon

These items aren't on the critical path for `v1` or the immediate follow-ups, but are worth tracking so they don't get rediscovered later.

### 21. SQL query interface

Lnav's signature feature: expose every record as a row in a SQLite (or DuckDB) virtual table, let the user write `SELECT level, count(*) FROM log GROUP BY level` from a query bar. Powerful but takes real work — the table view needs a "DB results" alternate mode, the columns become dynamic. A first cut could expose rows to DuckDB through Arrow IPC; the existing typed columns (`LogValue` discriminated union) map cleanly onto Arrow types.

### 22. Remote sources (SSH / SFTP)

OtrosLogViewer's hook into Apache Commons VFS. Less urgent now that we ship a TCP / UDP listener, but a "read from `ssh://host/path`" path is convenient: the user supplies SSH creds in a dialog and the app tails the remote file through `libssh2` (BSD-licensed) into the existing `StreamLineSource` machinery.

### 23. Plugin / extension system

LogExpert and OtrosLogViewer differentiate on this. Probably overkill for v1 — the regex-template registry already covers most parser-extensibility needs. If we ship one, the most useful surface is a "post-parse hook" that can decorate / transform a record before it hits the model (e.g. resolve a request ID against an external service, mask PII fields).

### 24. Search & filter history dropdown

Klogg, LogLens, Logan. A small dropdown next to the find bar listing the last *N* needles; same for the filter editor's value field. Cheap and very ergonomic; pairs well with [item 12](#12-saved-searches-and-named-views) (history items can be promoted to named views).

### 25. Timeshift (per-source clock offset)

LogExpert. When merging logs from two machines with skewed clocks, apply an offset to one source before the merge sort. The source descriptor in `LogConfiguration` already exists; add an optional `clockOffsetMillis` field per source.

### 26. Triggers / actions on pattern match

LogExpert. Run an action when a pattern fires: desktop notification on first `Fatal`, system bell on a watchword, auto-bookmark with a note. Builds naturally on the [highlight rules](#3-user-defined-highlight-rules) machinery — every rule grows an optional `action` field.

### 27. Scratchpad / notes panel

Klogg, Logan. A floating note editor that lives alongside the table, persisted with the session. Useful during long triage runs ("at 14:32 customer reported X, at 14:35 we deployed Y"). Strictly a free-form text editor; no integration with anchors / bookmarks beyond a side-by-side view.

### 28. Pipe selection to external command

lnav's `!` shell hand-off, Logan's tabbed terminal. Right-click selection → **Pipe to…** with a small palette of recent commands. Useful for `jq` filtering, `grep` on copied rows, `kubectl logs --since=...` on a request ID. Configurable per-user; persists with the global settings.

### 29. AI / LLM assistance panel

Logan, LogLens. A side panel where the user can ask "summarise these 200 rows", "what changed between window A and window B", "which requests look suspicious". Optional, off-by-default, requires a user-supplied API key or local model endpoint. Increasingly expected by 2026 buyers but **not** something we want to be required for the core triage workflow.

### 30. ~~Full-session export bundle~~

Landed as **File → Export Session Bundle…** (`Ctrl+Shift+E`). A v1 bundle is one standard checksummed zstd stream. Decompressed content is JSON Lines: a compact `{"__slv_bundle__":{...}}` metadata object containing `formatVersion`, `rowCount`, and the full configuration, followed by one normalized typed JSON object per retained source-model row.

**Why.** [Item 7](#7-export-filtered-rows) exports the visible view. This item exports every retained row plus the investigation state: columns, filters, sort, anchors + notes, and highlight rules. Recipient double-clicks the bundle and lands on the same configured view without needing the original files.

**Scope and deliberate flattening.** All original sources become one normalized JSONL source. Original paths, source boundaries, raw formatting, parser settings, and line IDs are discarded. Anchors are remapped to dense flattened row IDs and rebased to the bundle locator. This makes the file portable and inspectable with standard `zstd -d` / `unzstd` plus ordinary text or `jq` tools, while repeated JSON keys compress efficiently.

**Open path.** `DecompressingByteSource` streams the frame to a TEMP JSONL file, removes and retains the metadata line, and hands the remaining records to the normal `LogFile` / `FileLineSource` / `JsonParser` path. The bundle then behaves as a regular compressed static file (`Source::Kind::File`): append/replace, autosave, restore, and Recent Sessions all use the normal machinery. Embedded configuration is a default only for a fresh lone open; explicit configuration, append state, restored autosave, and later edits take precedence.

**Non-goals (v1 of this item).** Streaming continuation ("keep the tail growing after the bundle is opened"), diffing two bundles, GUI-side integrity signatures. Bundles are static snapshots.

**Approach.** `WriteSessionBundle` and `ParseSessionBundleMetadata` live in `loglib`; typed row serialization is shared with JSON Lines export through `SerializeNormalizedJsonRow`. The writer preserves cancellation, progress, metadata/row/size limits, zstd checksum, unique staging, durable flush, and atomic replacement. Temporary-file ownership is attached to `LogFile` so Windows unmaps before deletion.

**Acceptance bar.** Export 1 M retained rows + 20 anchors + 5 highlight rules + a non-trivial filter, reopen through the normal static path, and recover the flattened rows and configured view with anchors attached to their remapped rows.

**Touches.** `loglib`: `session_bundle.hpp`, writer/metadata reader, shared normalized-row serializer, and decompression first-line support. `app`: export action plus bundle detection/config-default handling inside the existing static compressed-file queue.

## Explicit non-goals

These come up often enough to warrant calling out:

- **Centralised log storage / ingestion pipeline.** We are a desk-side viewer, not an observability backend. Splunk / Elastic / Loki / Datadog cover that space.
- **Alerting.** No pager hooks, no email / Slack integrations. Triggers ([item 26](#26-triggers--actions-on-pattern-match)) max out at local notifications.
- **Dashboards.** The histogram strip ([item 2](#2-histogram--activity-rate-strip)) is the one visualisation we ship. Multi-panel dashboards belong in Grafana / Kibana.
- **Multi-user collaboration.** Sessions / anchors / notes are per-user; we do not sync them to a server, and there is no concept of "another user's cursor".
- **Auto-parsing of vendor binary formats** (Windows `.evtx`, Apple `.tracev3`, MSBuild `.binlog`, ...). Out of scope for the generic structured-log parser; each would need its own dedicated converter project. We document conversion recipes (e.g. `wevtutil epl > json` for Windows event logs) instead.
- **Streaming for arbitrary text formats with no record framing.** We require a per-line record boundary (or [item 6](#6-multi-line-records-stack-traces-and-continuation-lines)'s continuation rule). Truly free-form text logs should be opened in a tool like Klogg.
- **Mobile / web UI.** Desktop only. The Qt 6.8 codebase makes a web port impractical; a separate read-only web viewer over the same `loglib` could exist as a sibling project, but is not on this roadmap.
- **A built-in editor for the log file.** Read-only by design.

## Process

The roadmap is owned by the maintainer but **not closed**. To propose changes:

- **For a new feature idea**, open a GitHub Issue with the label `roadmap-proposal`. Include the user-facing problem, one or two competing tools that solve it, and a sketch of how it would land on the existing seams. Issues that fit the [Scope](#scope) and don't conflict with [Explicit non-goals](#explicit-non-goals) graduate to a roadmap entry in the next pass.
- **To pick up an existing item**, open a "tracking issue" referencing the roadmap section so the work is visible. The acceptance bar in each Tier 1 item is the merge gate; deviations should be discussed up-front rather than relitigated in code review.
- **To re-order items**, open a discussion thread (not an issue). Re-ordering happens at most quarterly; chasing the latest hot feature is how roadmaps die.

Every merged Tier 1 / Tier 2 item updates this file in the same PR — strike the item through, add a one-line note pointing at the shipping commit / release tag.

## Comparative feature matrix

Reference snapshot from the survey that informed the roadmap (June 2026). `✓` = present, `~` = partial / limited, blank = absent.

| Feature                                     | **Yours today**  |   lnav    | Klogg | LogExpert | LogViewPlus | OtrosLV | QLogExplorer | LogSleuth | Logan |
| ------------------------------------------- | :--------------: | :-------: | :---: | :-------: | :---------: | :-----: | :----------: | :-------: | :---: |
| JSON / NDJSON parsing                       |        ✓         |     ✓     |       |     ~     |      ✓      |    ✓    |      ✓       |     ✓     |   ✓   |
| logfmt / CSV / regex templates              |        ✓         |     ✓     |       |     ~     |      ~      |    ~    |      ~       |     ~     |   ~   |
| Multi-file merge into one table             |        ✓         |     ✓     |       |     ~     |      ✓      |    ✓    |              |     ✓     |       |
| Tabs for separate files                     |                  |           |   ✓   |     ✓     |      ✓      |    ✓    |      ✓       |           |   ✓   |
| Live tail                                   |        ✓         |     ✓     |   ✓   |     ✓     |      ✓      |    ✓    |      ✓       |     ✓     |   ✓   |
| Log rotation handling                       |        ✓         |     ✓     |   ~   |     ✓     |      ✓      |    ~    |      ✓       |     ~     |   ~   |
| Network ingestion (TCP / UDP)               |        ✓         |     ~     |       |           |             |    ✓    |              |           |       |
| TLS for network ingestion                   |        ✓         |           |       |           |             |         |              |           |       |
| Compressed file (gz / bz2 / zst / zip)      |        ✓         |     ✓     |   ~   |           |      ✓      |    ✓    |              |     ~     |       |
| Pulling rotated history off disk            |                  |     ✓     |       |           |             |    ~    |              |           |       |
| stdin / pipe input                          |        ✓         |     ✓     |   ✓   |           |             |         |              |           |   ~   |
| Per-row colouring by level                  |        ✓         |     ✓     |       |     ~     |      ✓      |    ✓    |      ✓       |     ✓     |   ✓   |
| User-defined highlight rules                |        ✓         |     ✓     |   ✓   |     ✓     |      ✓      |    ✓    |      ✓       |           |   ✓   |
| Bookmarks with notes / comments             |        ✓         |     ✓     |   ✓   |     ✓     |      ✓      |    ✓    |      ✓       |     ~     |   ✓   |
| Boolean filter expressions (AND / OR / NOT) |        ✓         |     ✓     |   ✓   |     ✓     |      ✓      |         |      ✓       |     ~     |   ✓   |
| Per-column / per-cell scoped filters        |        ✓         |     ✓     |       |     ✓     |      ✓      |    ✓    |      ✓       |     ✓     |   ✓   |
| Saved / named filter or query presets       |  ~ session only  |     ✓     |   ✓   |     ✓     |      ✓      |    ✓    |      ✓       |     ✓     |   ✓   |
| Histogram / activity timeline               |        ✓         |     ✓     |       |     ~     |      ✓      |         |              |           |   ✓   |
| Match overview rail / minimap               |        ✓         |           |   ✓   |           |             |         |              |           |   ✓   |
| Pretty-print JSON / XML inline              | ~ Record Details |     ✓     |       |           |      ✓      |    ✓    |      ✓       |           |   ✓   |
| Multi-line records (stack traces)           |        ✓         |     ✓     |   ✓   |     ✓     |      ✓      |    ✓    |      ✓       |     ✓     |   ✓   |
| Goto line / Goto timestamp                  |        ✓         |     ✓     |   ✓   |     ✓     |      ✓      |    ✓    |      ✓       |     ✓     |   ✓   |
| Time-range zoom / jump-by-N-min             |  ~ time filter   |     ✓     |       |     ✓     |      ✓      |    ✓    |      ✓       |     ✓     |   ✓   |
| Timeshift (per-file clock offset)           |                  |           |       |     ✓     |             |         |              |           |       |
| Encoding auto-detect (UTF-16, cp125x)       |                  |           |   ✓   |     ✓     |      ✓      |    ✓    |              |           |       |
| Export filtered rows (CSV / JSON / MD)      |        ✓         |     ✓     |   ~   |     ✓     |      ✓      |    ✓    |      ✓       |     ✓     |   ✓   |
| Full-session export bundle                  |        ✓         |           |       |           |             |         |              |           |       |
| Diff view of two files                      |                  |           |       |           |      ~      |         |              |           |   ✓   |
| Pattern clustering / similar-line grouping  |                  |           |       |           |             |         |              |           |   ✓   |
| Time-gap detection                          |                  |           |       |           |             |         |              |           |   ✓   |
| SQL queries over rows                       |                  |     ✓     |       |           |      ✓      |         |              |           |       |
| Headless / CLI / scriptable mode            |                  |     ✓     |   ~   |           |      ~      |         |              |     ~     |       |
| Remote sources (SSH / SFTP / SMB)           | ~ via TCP / UDP  |           |       |           |             |    ✓    |              |           |   ~   |
| Plugin / extension system                   |                  | ~ scripts |       |     ✓     |      ✓      |    ✓    |              |           |       |
| Scratchpad / notes panel                    |                  |           |   ✓   |           |             |         |              |           |   ✓   |
| Search / activity history                   |                  |           |       |           |      ✓      |         |              |           |   ✓   |
