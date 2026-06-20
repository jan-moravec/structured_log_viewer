# Structured Log Viewer — User Guide

## Overview

Structured Log Viewer is a Qt 6 desktop application for inspecting structured log files. It displays each record as a row in a sortable, filterable table, auto-detects timestamp columns, and lets you search records using plain text, wildcards, or regular expressions.

## Supported Input Formats

The application reads two structured-log formats out of the box. The format is auto-detected on `File → Open` and persisted with the session, so reopening a saved session reuses the same parser without re-prompting.

**JSON Lines** (also known as NDJSON / JSOND): one JSON object per line.

```json
{"timestamp":"2025-01-15T12:34:56.789Z","level":"info","component":"app","message":"started"}
{"timestamp":"2025-01-15T12:34:57.000Z","level":"error","component":"db","message":"connection refused"}
```

**logfmt** (the Heroku / `kr/logfmt` flavour): one record per line, whitespace-separated `key=value` pairs, optionally double-quoted values with C-style escapes (`\"`, `\\`, `\n`, `\r`, `\t`).

```logfmt
timestamp=2025-01-15T12:34:56.789Z level=info component=app msg="started"
timestamp=2025-01-15T12:34:57.000Z level=error component=db msg="connection refused" code=42
```

Bare values are typed: empty (`key=`) becomes null, `true` / `false` become booleans, decimal integers become int / uint, decimals with point or exponent become double, otherwise the value stays a string. Quoted values **stay strings even if the contents look numeric** (so `pid="42"` does not promote to a number). Repeated keys within one record are last-write-wins.

Empty lines are skipped. Lines that fail to parse are reported as errors but do not abort loading — valid records are still shown. Nested JSON objects and arrays are preserved as their compact JSON string; logfmt has no nesting.

## Ingestion modes

Structured Log Viewer ingests logs through three distinct paths. Pick the one that matches what you are looking at:

| Aspect                 | Static mode                                                            | Stream Mode (live tail)                                                                               | Network Stream Mode (TCP / UDP)                                                                  |
| ---------------------- | ---------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------ |
| **When to use**        | Post-mortem analysis of one or more *finished* log files.              | Watching a service's log file *as it is being written* — reproducing a bug, smoke-testing a release.  | Receiving structured logs pushed over the network — distributed services, dev loopback firehose. |
| **How to open**        | `File → Open…` (`Ctrl+O`) or drag & drop. The format is auto-detected. | `File → Open Log Stream…` (`Ctrl+Shift+O`). Drag & drop always uses static mode.                      | `File → Open Network Stream…` (`Ctrl+Shift+L`); pick the format in the dialog.                   |
| **Source per session** | One or many files; multi-file opens are **merged** into one table.     | Exactly one file.                                                                                     | One TCP listener (multiple concurrent clients allowed) or one UDP listener.                      |
| **Reads bytes via**    | Memory-mapped; parsed in parallel through the TBB pipeline.            | Buffered tail-reader; parsed line-by-line in a single worker.                                         | Asio TCP accept loop (with optional TLS) or Asio UDP receive loop; same line-by-line worker.     |
| **Memory**             | Whole file is parsed and held; row count grows with the file.          | Bounded by a configurable **retention cap** (default 10 000 lines, FIFO-evicted).                     | Same retention cap as Stream Mode; back-pressure also drops oldest *bytes* if the parser stalls. |
| **Reacts to new data** | No. The on-screen rows are a snapshot of the file at open time.        | Yes. New lines appear within ~250 ms of being written; survives `logrotate` and in-place truncations. | Yes. Each `\n`-terminated record lands as a new row as soon as it is received.                   |
| **Stream toolbar**     | Hidden.                                                                | Pause / Follow newest / Stop visible while a session is active.                                       | Same toolbar as Stream Mode.                                                                     |
| **Configuration menu** | Available between opens; disabled while a parse is in flight.          | Disabled for the lifetime of the session (the parser holds an immutable configuration snapshot).      | Same — disabled for the lifetime of the session.                                                 |

In Stream Mode and Network Stream Mode, **Stop** ends the session but keeps the visible rows around as a static snapshot you can keep filtering, sorting, and copying — handy when you only realised mid-tail that the bug already happened. Opening a new source (in any mode) clears the table first.

## Static Mode (Open files)

You can open a finished log file in two ways:

1. **File → Open…** (`Ctrl+O`) — opens a file picker that auto-detects whether the selected file is a log or a [configuration](#configurations) file, *and* whether a log file is JSON Lines or logfmt.
1. **Drag & drop** one or more files onto the main window.

The file dialog defaults to a filter that lists `*.json`, `*.jsonl`, `*.ndjson`, `*.logfmt`, `*.log`, and `*.txt`; switch it to **All Files (\*.\*)** to pick anything else (including unsuffixed files). The actual format is decided by content sniffing, not by extension, so the extension only affects what the picker shows — not how the file is parsed. The detected format is recorded on the active session so reopening a saved session keeps the same parser.

Opening multiple files at once **merges** their records into a single table; the files are queued and parsed sequentially while sharing one column layout. Mixing formats across the queue is supported — each file is sniffed individually. If parsing errors occur, the first 20 are shown in a dialog when the queue drains; the rest are summarized as "… and N more error(s)". The status bar shows `Parsing <file> — N lines, M errors` while the queue is in flight.

For a file that is **still being written**, use [Stream Mode](#stream-mode-live-tail) instead — static mode parses the bytes that exist when you opened the file and stops there.

## Stream Mode (live tail)

Stream Mode opens a single log file and continuously tails it: it pre-fills the table with the last `N` complete lines on disk and then appends every new line as it is written. The mode is targeted at developer workflows where you want to watch a service's log as you reproduce a bug or run a smoke test, without alt-tabbing to a terminal.

### Opening a stream

Use **File → Open Log Stream…** (`Ctrl+Shift+O`) to pick a single file. Drag-and-drop continues to use the static path (the assumption is that drag-and-drop is for "open this archive", not "tail this"). Errors during open (file not found, permission denied) are reported via the existing parse-error dialog and the previous session is preserved.

The pre-fill back-reads at most **retention** complete lines from the end of the file (see [Retention cap](#retention-cap)), so opening a multi-GB log in Stream Mode is fast even on a cold cache.

### Toolbar and **Stream** menu

A toolbar appears above the table while a stream is running, mirroring the **Stream** menu:

| Action         | Shortcut       | Notes                                                                                                              |
| -------------- | -------------- | ------------------------------------------------------------------------------------------------------------------ |
| Pause / Resume | `Ctrl+Shift+P` | Pause freezes the visible table; new lines accumulate in memory until you Resume or Stop.                          |
| Follow newest  | `Ctrl+Shift+T` | Auto-scroll to the newest line. Auto-disengages when you scroll away to read history; re-engages on scroll-to-end. |
| Stop           | `Ctrl+Shift+S` | Ends the session and leaves the rows visible as a static snapshot you can keep filtering, sorting, and copying.    |

**Pause** and **Follow newest** are independent toggles, and both reset to their defaults (un-paused, follow on) on every new session.

### Status bar

The permanent status-bar label reflects the current state of the live tail:

| Label                                                       | Meaning                                                                                                                                                                   |
| ----------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `Streaming <file> — N lines, M errors`                      | Normal flow; updated on every batch.                                                                                                                                      |
| `Paused — N lines, K buffered`                              | Stream is paused. `K` is the number of lines that arrived during the pause; **Resume** drains them in one go.                                                             |
| `Source unavailable — last seen <file> — N lines, M errors` | The watched file disappeared (e.g. mid-rotation). The viewer keeps the rows it has and waits for it to reappear.                                                          |
| `Streaming <file> — … — rotated`                            | Briefly appended (~3 s) right after a log rotation is detected.                                                                                                           |
| `Streaming <file> — … , X dropped while paused`             | At least `X` lines arrived while paused and overflowed the retention cap, so they were FIFO-evicted from the paused buffer. The counter is sticky until the session ends. |

### Retention cap

Stream Mode keeps at most `N` lines in memory; older lines are evicted in FIFO order so the resident set stays bounded across long-running tails. The default cap is **10 000 lines**. To change it, open **Settings → Preferences…** → **Streaming** → **Stream retention (lines)** (range 1 000 .. 1 000 000).

The setting is persisted via `QSettings` and applies to future sessions and to the *current* session: lowering the cap on a running stream FIFO-trims existing rows immediately; lowering it while paused trims the paused buffer first (visible rows are preserved, per the pause-suspends-eviction rule). Raising the cap takes effect from the next batch onward — already-evicted lines are not pulled back.

### Newest lines first

By default new lines are appended to the *bottom* of the table, which matches how most terminal log viewers behave. **Settings → Preferences… → Streaming → Show newest lines first** flips the orientation: the most-recently-arrived line is shown at row 0, older lines below it. **Follow newest** then keeps the top of the view pinned to the latest line. The setting is persisted via `QSettings`, applies to all subsequent sessions, and can be toggled while a stream is active.

> Alternating row colours are disabled in newest-first mode because Qt's CSS-based striping is keyed off the visual row index, which would flicker every time a new line shifts the rows down.

### Jump-to-tail pill

When **Follow newest** is off and you have scrolled away from the tail edge, a floating "↓ N new lines" pill (or "↑ N new lines" in newest-first mode) appears in the corner of the table whenever new lines arrive. Click it to jump to the newest row and re-engage **Follow newest**. The pill fades out automatically once you reach the tail edge or follow is re-engaged from any other surface.

### File rotation

Stream Mode tolerates the common log-rotation patterns:

- **logrotate `create`** (rename + new file at the original path)
- **logrotate `copytruncate`** (copy aside, then truncate in place)
- **In-place truncation** (`: > app.log`)
- **Delete-then-recreate** (path disappears for a moment, then reappears)

When a rotation is detected, the viewer keeps every line that is already in memory, switches to the new on-disk content, and briefly appends `— rotated` to the status bar so you can tell it happened. Rotation bursts within a 1 s window are coalesced into a single event. **Rotated content is not pulled back from disk**: only the lines you have already seen survive the rotation in memory.

### Stop semantics and configuration menus

While a stream is active the **Configuration** menus (Save / Load) and **Settings → Preferences…** are disabled — the running parser holds an immutable snapshot of the configuration, and editing it would race the worker. **Stop** ends the session, joins the background tailing thread within ~500 ms, and re-enables those menus. Closing the application while a stream is active stops cleanly via the same teardown path.

### Non-goals

The following are **out of scope** for the current Stream Mode implementation:

- **Compressed rotations** (`app.log.1.gz` etc.) — only uncompressed rotations are handled.
- **Pulling rotated history off disk** — the viewer does not read `app.log.1` to recover lines older than the in-memory cap.
- **stdin / named-pipe sources** — only file tailing is wired up; for network ingestion see [Network Stream Mode](#network-stream-mode-tcp--udp).
- **Auto-detect "this file is being actively written → open in Stream Mode"** — Stream Mode is always an explicit `File → Open Log Stream…` action.
- **Per-file or per-session retention overrides** — the retention cap is a single application-wide setting.
- **Streaming for arbitrary formats** — JSON Lines and logfmt are first-class; other formats (CSV, ad-hoc text) are not yet supported but the seam in `loglib` is format-agnostic so future parsers inherit live tail and network ingestion for free.

## Network Stream Mode (TCP / UDP)

Network Stream Mode listens on a local TCP or UDP port and ingests structured logs pushed to it by your application. It is intended for distributed services that cannot redirect their stdout/stderr to a file you can tail, and for "firehose into the GUI" loops during development. Each `\n`-terminated record becomes a row exactly the same way Stream Mode does, so the toolbar, retention cap, Pause / Follow newest / Stop, search, filters, and configurations all behave identically once the session is open.

### Opening a network stream

Use **File → Open Network Stream…** (`Ctrl+Shift+L`). The dialog asks for:

- **Protocol** — TCP or UDP.
- **Format** — JSON Lines or logfmt. Network ingestion has no file to sniff, so the parser is selected explicitly here. The choice is persisted with the session.
- **Bind address** — `0.0.0.0` (IPv4 any), `::` (IPv6 dual-stack), `127.0.0.1` / `::1` (loopback only), or a specific interface IP.
- **Port** — the listening port. `0` requests an OS-assigned ephemeral port (handy for ad-hoc local testing).
- **Max concurrent clients (TCP)** — hard cap on simultaneous accepted connections (default 16). New connections beyond this are accepted-and-immediately-closed.
- **TLS (TCP only)** — when the binary is built with `LOGLIB_NETWORK_TLS=ON`, an *Enable TLS* checkbox unlocks fields for the **certificate chain**, **private key**, and an optional **CA bundle** for verifying client certificates. *Require valid client certificate* turns the CA bundle from optional verification into mutual TLS. The whole TLS group is greyed out on builds compiled without TLS support.

The dialog values are persisted via `QSettings` so the next invocation defaults to your last choice. UDP cannot use TLS — DTLS is intentionally out of scope; encrypted log shipping is the TCP+TLS path.

### TCP vs UDP

| Aspect                 | TCP                                                                                  | UDP                                                                |
| ---------------------- | ------------------------------------------------------------------------------------ | ------------------------------------------------------------------ |
| **Concurrent senders** | Many; lines from different clients interleave at line granularity (no torn records). | Many; each datagram is independent.                                |
| **Reliability**        | Reliable, ordered.                                                                   | Best-effort; out-of-order or dropped datagrams are not retried.    |
| **Framing**            | One line per `\n`-terminated record over the byte stream.                            | One or more whole records per datagram (`\n` appended if missing). |
| **TLS**                | Optional, gated on the `LOGLIB_NETWORK_TLS=ON` build option.                         | Not supported (plaintext only).                                    |
| **Recommended for**    | Production log shipping over a LAN/WAN, especially when integrity matters.           | Loopback firehose, fire-and-forget local services.                 |

### Status bar and toolbar

Network sessions reuse the Stream Mode toolbar (Pause / Follow newest / Stop) and status-bar labels. The "filename" shown in the label is the listener URL — for example `Streaming tcp://0.0.0.0:5141 — N lines, M errors` or `Streaming udp://127.0.0.1:5142 — …`. Until the first byte arrives the label shows `Source unavailable — last seen tcp://… — 0 lines, 0 errors` (TCP) or the matching UDP variant, which makes a misconfigured client side obvious. Both protocols flip to `Streaming` on the first byte / datagram and never fall back, since there is no connection state to track on either side.

### Stop semantics

**Stop** closes every open client connection (TCP), shuts the listening socket, joins the I/O worker, and leaves the visible rows around as a static snapshot for further filtering / sorting / copying. Closing the application also cleanly shuts the session down. Once stopped, the same listening port is free again for the next session.

### Non-goals

- **Per-peer attribution** — connections are interleaved; the GUI does not split rows by source address.
- **DTLS** — UDP is plaintext-only by design.
- **Encrypted private keys / hardware tokens** — the TLS configuration accepts plain PEM cert + key files only. Decrypt offline before pointing the dialog at them.
- **Auto-discovery of services on the network** — bind address and port are always entered explicitly.

## Automatic Column Detection

The first time you open a file (in either mode), Structured Log Viewer builds the column list from the keys it sees in the JSON records:

- Any key named `timestamp`, `time`, `ts`, `@timestamp`, `datetime`, or `created_at` (case-insensitive) is treated as a **timestamp column**. Its values are parsed with the ISO 8601 formats `%FT%T%Ez`, `%F %T%Ez`, `%FT%T`, `%F %T` and displayed with the format `%F %H:%M:%S` in the local timezone. Timestamp columns are moved to the front of the table.

  > **Note:** the heuristic is **destructive** for streaming opens. If a key matching the heuristic appears mid-parse, the corresponding column is flipped from `any` to `time` in-place and every row already in the table is back-filled with the parsed `TimeStamp`. The original raw string variant is replaced by the parsed value, so disabling the column's `time` type later (via a saved configuration) does not bring the original textual value back without re-opening the file.

- Columns drawn from a small, stable set of strings (e.g. `service`, `host`, `module`) are auto-promoted to **enumeration columns** once at least 4096 rows have been seen and the column holds at most 64 distinct values. Smaller files are caught by an end-of-parse sweep, and streaming opens promote the column eagerly after 2 rows. Enum columns are stored compactly as dictionary references and unlock the value-picker filter UI (see [Filtering an enumeration column](#filtering-an-enumeration-column)). A 1 % tolerance for over-long or wrong-type values (evaluated after 50 observations) governs both promotion and demotion; a column that breaches the budget or outgrows 64 distinct values is demoted to text and stays demoted for the session.

- An enumeration column whose key matches a known log-level field name is further promoted to a **log-level column** when its dictionary holds at least one canonical level (`Trace`, `Debug`, `Info`, `Warn`, `Error`, `Fatal`) and at most one unrecognised entry per four canonical ones. A dictionary of size 4 or smaller must be 100 % canonical; size 5 tolerates one stray, size 10 tolerates two, and so on. The check is dict-weighted, so it re-evaluates only when the dictionary grows. Level columns sort by canonical severity rather than alphabetic order, and the filter dialog shows the six canonical levels. The raw bytes stay in the dictionary for display; only sort, filter, and styling use the canonical mapping. Per-column `levelMapping` overrides (see below) let you alias project-specific tags onto canonical levels.

  Recognised **key names** (case-insensitive):

  - Long-form: `level`, `severity`, `loglevel`, `log.level`, `log_level`, `lvl`, `levelname`, `priority`.
  - Short forms (used by compact / embedded loggers): `l`, `lv`, `lev`, `sev`, `s`, `loglvl`.
  - OpenTelemetry / ECS / GCP: `severity_text`, `severity.text`, `severitytext`, `log_severity`, `log.severity`, `logseverity`.
  - Separator variants of `levelname`: `level_name`, `level.name`.
  - Structured-JSON conventions (Serilog `@l`, Datadog @-fields, etc.): `@level`.

  > **Single-letter keys carry false-positive risk.** A `length` or `size` column could match `l` / `s` by name alone. The dictionary-content check is the safety net: a column named `l` whose dictionary holds `red`/`green`/`blue` will not promote.

  Recognised **value aliases** (case-insensitive):

  - `Trace`: `trace`, `trc`, `t`, `finer`, `finest`, `silly`
  - `Debug`: `debug`, `dbg`, `d`, `verbose`, `vrb`, `v`, `fine`
  - `Info`: `info`, `inf`, `i`, `information`, `informational`, `notice`
  - `Warn`: `warn`, `wrn`, `w`, `warning`
  - `Error`: `error`, `err`, `e`, `severe`
  - `Fatal`: `fatal`, `ftl`, `f`, `critical`, `crit`, `emerg`, `emergency`, `panic`, `alert`, `fault`

  > **Why `verbose` / `vrb` / `v` map to `Debug`, not `Trace`.** The original mapping landed Verbose on Debug; flipping it now would change the canonical level (and the sort key) for any saved configuration relying on the prior behaviour. Serilog and Android treat Verbose as below Debug, but we keep the old mapping for backwards compatibility.
  >
  > **Numeric levels (Bunyan/Pino `10/20/30/40/50/60`, syslog `0..7`, ...) are *not* built in.** The two conventions disagree, so picking one would silently mis-classify the other. Map them per-column via `levelMapping` (e.g. `[["10","Trace"], ["20","Debug"], ["30","Info"], ["40","Warn"], ["50","Error"], ["60","Fatal"]]`). This only works when the producer emits the value as a JSON string (`"level": "30"`); a numeric JSON value (`"level": 30`) arrives as an integer and never reaches the dictionary.
  >
  > **Mapping unrecognised aliases.** Add a `levelMapping` array to the column in a saved configuration to extend the alias table per-column. Entries are `(alias, canonicalName)` pairs and override the built-in table:
  >
  > ```json
  > {
  >   "header": "severity",
  >   "keys": ["severity"],
  >   "printFormat": "{}",
  >   "type": "level",
  >   "parseFormats": [],
  >   "levelMapping": [["NOTICE", "Info"], ["PANIC", "Fatal"]]
  > }
  > ```
  >
  > **Note:** auto-promotion only happens for columns that are **not** explicitly typed by a loaded [configuration](#configurations). Save a column as `any` to lock it as text.
  >
  > **Note:** the canonical side must be `Trace`, `Debug`, `Info`, `Warn`, `Error`, or `Fatal`. Anything else (including `Unknown` or typos like `NotARealLevel`) is silently ignored and matching aliases fall through to the built-in table. To suppress an alias entirely, drop it from the data or pin the column type to `enumeration` instead of `level`.

- All other keys become generic columns with the format `{}` (pass-through).

You can persist a customized column layout and filter set via [configurations](#configurations).

## Navigating the Table

- **Sorting**: click a column header to sort ascending, again to flip direction, a third time to clear. The initial load is unsorted (records appear in file order). The same sort is available from four other surfaces, all kept in lock-step:
  - The **Sort** menu lists every visible column twice (`▲ "<col>"` / `▼ "<col>"`) with the active sort checked. The first row is **Clear Sort**.
  - The main toolbar carries a **Sort** split button (mirroring the menu) and a plain **Clear Sort** button next to it.
  - **Right-clicking a column header** offers `Sort ascending by "<col>"`, `Sort descending by "<col>"`, and `Clear sort`.
  - The status bar shows a **Clear sort** indicator while a sort is active.
- **Column resizing**: drag the column header dividers. The last column stretches automatically to fill remaining space.
- **Smooth scrolling** is enabled by default (per-pixel) both vertically and horizontally.
- **Alternating row colors** improve readability on dense logs. The table respects the application's light/dark palette.

### Column Management

You can rearrange and hide columns to fit your view:

- **Reorder**: drag a column header sideways to a new position. Filters and the sort indicator track the column to its new index, and the new order is preserved by [configurations](#configurations).
- **Hide a column**: right-click the header and choose **Hide "\<col>"**, or uncheck the column in the **View** menu. Hidden columns stay in the model — data, sort, search, and filters keep working — only the header section is hidden.
- **Show a hidden column**: re-check the column in the **View** menu. The **View** menu is the only way back, and stays reachable even when *every* column is hidden (no header section is left to right-click).

Duplicate header names are disambiguated as `header [key]` in both menus so columns sharing a header are still individually addressable.

> Hiding the column the table is currently sorted by clears the sort, since the sort glyph would otherwise live on a hidden section with no way to undo it.

### Selecting and Copying Rows

- Click a row to select it. Hold `Ctrl`/`Shift` to extend the selection.
- **Edit → Copy** (`Ctrl+C`) copies the selected rows as the **original JSON text** (one line per row), so you can paste them back into another tool. Cell-level copy is not performed — rows are always copied whole.

See [Anchors](#anchors) for marking and navigating between specific rows.

### Inspecting a Record

For per-row drill-down, Structured Log Viewer ships a **Record Details** pane that shows every parsed field on its own row plus the pretty-printed original JSON. Open it any of three ways:

- **Double-click** any row in the table.
- **View → Record Details** (`Ctrl+I`).
- Click the dock back open if you had closed it via its title-bar `X`.

The pane is a Qt dock widget: drag the title bar to snap it to the left, right, or bottom of the window, or drop it outside the window to float it as an independent top-level window. Once visible, the pane follows the table's current row — arrow-key navigation updates it live.

Inside the pane:

- A bold header summarises the row (`Row N` plus the formatted timestamp when a `Time` column exists). `Row N` is the **source-model row** — the underlying record's stable position — and may not match the row number shown in the main table when a sort or filter is active.
- A two-column **Field / Value** table lists **every configured column** for the record — a complete view that isn't filtered by the main table's reorder, hide, or sort state. Fields are listed in the parser's original column order so the layout stays stable even as you customise the main view. Values are the same formatted output the table cells show, but without single-line compaction so nested objects stay readable. Present-but-empty fields render as a muted em-dash (`—`) so the row still has visual weight; the original empty value is what gets written to the clipboard.
- A collapsible **Raw JSON** section reveals the on-disk line, pretty-printed via `QJsonDocument` (with a fall-back to the original bytes for non-JSON lines). When the raw bytes aren't available (e.g. the line was evicted in a streaming session after you pinned it) the section is disabled and labelled *Raw JSON (unavailable)* so the empty state is visible rather than silent.
- The pane refreshes automatically whenever the pinned record itself changes underneath you: a column rename via the columns manager, an enum-column promotion, or any other `dataChanged` notification covering the pinned row. New rows being appended to a live stream don't trigger a refresh because the pinned record's data is unaffected; the persistent pin keeps tracking the same record across FIFO eviction shifts and falls back to a placeholder if the pinned line is itself evicted.
- **Copy raw JSON** copies the **original on-disk bytes** of the line (compact, exactly as the parser ingested) so the clipboard text round-trips back into another tool unchanged. To copy the pretty-printed text instead, select inside the Raw JSON edit and press `Ctrl+C`. The button is disabled when the raw bytes aren't available.
- **Copy as key/value** copies the field table as `header: value` lines. Embedded newlines, tabs, and backslashes inside values are escaped C-style (`\n`, `\r`, `\t`, `\\`) so each field always lands on a single line and the format round-trips unambiguously.
- Inside the Field/Value table, `Ctrl+C` copies the selected cells as tab-separated values. Selection is extended (Ctrl-click to toggle individual cells, Shift-click for a range), matching standard spreadsheet behaviour.

To pin a record for side-by-side comparison, click **Open in new window** inside the pane. That spawns a top-level snapshot window with a frozen copy of the displayed content — you can open as many as you like, and each one survives streaming-mode FIFO eviction, sort, filter, or even a full `File → Open…` reset because its strings are deep-copied at creation. Close each snapshot with the window's normal close button when you're done.

## Anchors

Anchors let you mark notable rows with one of eight colours and jump between them with the keyboard. Useful when triaging a long log: pin the request that started the incident, the first error, and the final recovery, then cycle through them with `F2`.

### Marking a row

- Right-click a row → **Anchor** → "Colour N" to colour it.
- Or press `Ctrl+1` … `Ctrl+8` with one or more rows selected to apply that slot to every selected row.
- The same chord on an already-anchored row re-colours it.

### Clearing

- Right-click a row → **Anchor** → **Remove anchor**, or `Ctrl+0` on the selection.
- **View → Anchors** panel → **Clear all**, or `Ctrl+Shift+A` from anywhere.

### Navigating between anchors

- `F2` jumps to the next anchored row in the current visible order (sort + filter + [newest-first](#newest-lines-first) orientation are honoured).
- `Shift+F2` jumps to the previous one. Both wrap at the visible bounds.
- Anchors filtered out of the visible table are skipped; the status bar shows an explanation if every anchor is currently filtered.

### Anchors panel

- **View → Anchors** (`Ctrl+K`) toggles a dock listing every anchored row with its colour swatch, line id, and source filename.
- Double-click an entry (or press `Enter` on it) to jump to it.
- Right-click for **Jump to anchor** / **Remove anchor**.
- The header **Clear all** button drops every anchor at once. It is disabled while no anchors exist.
- The panel is a regular Qt dock — drag the title bar to redock it to another edge, or drop it outside the window to float it.

### Anchor colours and themes

The eight slots index into the active theme's `anchorPalette`. A theme that omits the field (or leaves a slot empty) falls back to a built-in palette with eight saturated, hue-distinct entries. The foreground text colour for each swatch is picked from the slot's luma so the label stays legible on custom user themes.

### Persistence

The anchor list is persisted as part of the [configuration](#configurations), so re-opening the same session restores its colours. Anchors are keyed by `(canonical file path, lineId)`; switching to a different log source loses their resolution even when the configuration is loaded.

> Anchored rows that are FIFO-evicted from a [streaming session](#retention-cap) are dropped from the anchor list automatically — they would otherwise linger in the panel and in the saved configuration with no resolvable row to point at.

## Searching

Open the Find bar with **Edit → Find** (`Ctrl+F`). It appears at the bottom of the window with:

- A text field for the search term
- **Wildcards** checkbox — interprets `*` and `?` in the term
- **Regular Expressions** checkbox — interprets the term as a Qt `QRegularExpression`
- **Next** / **Previous** buttons — walk forward or backward from the current selection
- **X** to close the bar

Search matches any cell in any visible column. Results wrap at the end/beginning of the table. The found row is selected and scrolled into view.

> Only one of *Wildcards* or *Regular Expressions* should be enabled at a time. Leaving both off performs a case-sensitive substring search.

## Filtering

Structured Log Viewer supports any number of simultaneous filters. A row is shown only if it matches **all** active filters.

### Adding a Filter

1. Choose **Filters → Add** to open the **Filter Editor** dialog.
1. Pick the column to filter in the **Row to filter** dropdown.
1. Depending on the column type:
   - **Text columns** — enter a filter string and pick a match mode: *Exactly*, *Contains*, *Regular Expression*, or *Wildcards*.
   - **Timestamp columns** — pick a **Begin** and **End** date/time. The dialog constrains the two so Begin ≤ End, and pre-fills them with the range of values present in the table.
   - **Enumeration columns** — see [Filtering an enumeration column](#filtering-an-enumeration-column).
1. Click **Ok**. The active filter is added as an entry in the **Filters** menu, titled with its value (or timestamp range, or comma-separated list of enum values).

### Filtering an enumeration column

When the **Row to filter** dropdown points at an [enumeration column](#automatic-column-detection), the dialog shows a **Selected values** list instead of a text field:

- The list shows every distinct value seen so far, sorted alphabetically.
- Tick the values to keep — rows whose value is ticked pass the filter.
- A `Selected: X / Y` header plus **Select all** / **Clear all** buttons make bulk selection easy.
- Saving the filter with zero values selected is rejected with a warning.

If a column is demoted back to text mid-session, saved enum filters fall back to comparing the row's text value against the saved selection. A saved text filter on a column that later auto-promotes to enum continues to match by text; re-edit it to switch to the value picker.

Saved filters on a **log-level column** that demotes back to text are translated automatically: each canonical name you ticked (`Info`, `Warn`, …) is expanded to every raw dictionary entry that resolved to it pre-demote (e.g. `Info` becomes `info` plus any `levelMapping` aliases like `NOTICE` or `30`). The rewritten filter then matches as a regular enum filter would. Values that arrive *after* the demote are not added retroactively — re-edit the filter once the column stabilises if a new alias should join the selection.

### Editing or Removing a Filter

Each filter entry in the Filters menu has **Edit** and **Remove** sub-actions:

- **Edit** re-opens the Filter Editor pre-populated with the current values.
- **Remove** drops just that filter.
- **Filters → Clear All** removes every filter at once.

You can also right-click a column header for the same actions scoped to that column: **Add filter on "\<col>"…** opens the Filter Editor preselected to the clicked column, and every existing filter targeting the column appears with its own **Edit** / **Remove** sub-menu.

Columns with one or more active filters carry a small funnel icon in their header; hovering shows a tooltip listing the filter values. The status bar also displays a **Clear filters** button while any filter is active.

Filters are live: the table updates immediately when a filter is added, edited, or removed.

## Configurations

A *configuration* captures the current column layout — headers, keys, print format, timestamp parse formats, column type (`time` / `enumeration` / `level` / …), per-column log-level alias overrides (`levelMapping`), **column order**, **per-column visibility** — the **active filter set**, and the current [anchors](#anchors). Configurations are saved as JSON files and can be loaded into future sessions to skip auto-detection and to enforce a consistent layout across teammates.

- **File → Save Configuration…** (`Ctrl+S`) — writes the current layout and filters to a `.json` file.
- **File → Load Configuration or Session…** — loads a configuration (or [session](#sessions-and-recent-sessions)) file and clears any open logs. Open logs again afterwards to apply the layout.

Because `Open…` auto-detects configurations, double-clicking a saved configuration file also works (it loads the layout without opening any logs).

> Saved filters are validated against the loaded column set on every Load. Filters that no longer match a column (e.g. the key was renamed, or the column type changed in an incompatible way) are dropped and reported in a summary dialog so you can re-create them; the rest are restored as live entries in the **Filters** menu.

## Sessions and Recent Sessions

A *session* is a saved configuration **plus the source** (file path(s) for static mode, tailed file for stream mode, listener URL for network stream mode) and the active sort. Loading a session re-opens the source automatically and re-applies the full view state — column layout, filters, sort, anchors — in one step.

- **File → Save Session…** (`Ctrl+Shift+S`) writes the current view state, source, and sort to a `.json` file.
- **File → Load Configuration or Session…** auto-detects whether the picked file is a configuration or a session and dispatches accordingly. Sessions re-open their source; configurations leave the table empty so you can open logs after loading.
- **File → Recent Sessions** lists the most recently auto-saved sessions. Click an entry to re-open it. The list is bounded by **Settings → Preferences… → Session History → Maximum Recent Sessions entries** (default 20); **Clear Recent Sessions** at the bottom of the submenu drops every entry.

Auto-saved sessions are written silently in the background as you work, so closing and re-launching the app puts you back where you were when **Settings → Preferences… → Session History → Restore last session on launch** is enabled (default). Live-tail and network-stream sessions are auto-saved as well, but only the source identity is restored — the tail position is not.

### New Window vs New Session

- **File → New Window** (`Ctrl+Shift+N`) opens a second top-level window with an empty session. The two windows share the same Recent Sessions list and global preferences but each holds its own logs, filters, anchors, and panels. Useful for side-by-side comparison of two sources.
- **File → New Session** (`Ctrl+N`) discards the current window's session (rows, filters, sort, source) and returns it to an empty view. Holding `Shift` while dragging or opening a file is the in-place equivalent for static sessions.

## Themes

Structured Log Viewer ships a built-in theme catalogue covering both Light and Dark variants, plus high-contrast triage skins. The active theme drives the palette, optional Qt style override, fonts, level row tints (subtle by default; loud when **High contrast levels** is on), the level column icon-pill (when **Show level icons** is on), and the eight [anchor](#anchors) swatches.

Switch themes via **Settings → Preferences…** → **Theme → Active theme**:

- **Auto (follow system)** — picks a Light or Dark theme to match the OS palette and re-resolves on the fly when the OS flips. This is the default.
- Any built-in theme by name — pins the theme regardless of OS scheme.
- Any user theme — `*.json` files dropped into `<AppData>/themes/` (button **Open themes folder** reveals the directory in your OS file manager). User files **shadow built-ins of the same name**, so you can override a built-in by saving a file with a matching `name` field.

Three buttons next to the combo manage user themes:

- **Open themes folder** opens `<AppData>/themes/` so you can edit JSON files directly.
- **Duplicate active theme…** copies the currently resolved theme into `<AppData>/themes/<name>-copy.json` and opens the folder so you can edit it.
- **Reload themes from disk** re-scans the built-in catalogue and the user folder, then re-applies the active selection. Use after editing a JSON file outside the app.

Two adjacent toggles tune how the active theme is rendered:

- **Show level icons** — when on (default) the [level column](#automatic-column-detection) renders a themed glyph inside a coloured pill instead of the raw level text. The raw text is preserved for **Copy** and **Find** searches. Disabled if the active theme omits the optional `levelColumnOverride` block; every built-in supplies it.
- **High contrast levels** — swaps the subtle per-level row colours for the theme's loud `levelsHighContrast` palette. Useful for triage screens where Warn / Error / Fatal rows need to jump out. Disabled if the active theme omits the `levelsHighContrast` block; every built-in supplies it.

## Preferences

Open **Settings → Preferences…** to change application-wide settings. The dialog is split into four groups:

**Theme** — previewed live; reverted on Cancel.

- **Active theme** — see [Themes](#themes) above.
- **Show level icons** — render themed glyphs in the level column instead of the raw level text.
- **High contrast levels** — use the theme's loud row tints for Warn / Error / Fatal.

**Streaming** — applied transactionally on **Ok**.

- **Stream retention (lines)** — the cap on how many lines [Stream Mode](#retention-cap) keeps in memory (range 1 000 .. 1 000 000, default 10 000).
- **Show newest lines first** — orient new stream lines at the top of the table instead of the bottom (see [Newest lines first](#newest-lines-first)).

**Static (file mode)** — applied transactionally on **Ok**.

- **Show newest lines first** — display files opened in static mode (`File → Open…`, drag & drop) with the last line at the top and the first line at the bottom. The flag is independent of the Streaming group's setting, so you can keep static files oldest-first while streaming sessions are newest-first (or the other way around). Alternating row colours are disabled while this is enabled, for the same reason as in Stream Mode.

**Session History** — applied transactionally on **Ok**.

- **Restore last session on launch** — when on, the most recent auto-saved [session](#sessions-and-recent-sessions) is reopened automatically on startup. Only applies to the primary instance on a launch with no command-line files.
- **Maximum Recent Sessions entries** — cap on how many entries appear in **File → Recent Sessions**. Older entries are evicted as new sessions are saved.

Click **Ok** to persist (stored via `QSettings` under the organization `jan-moravec` / application `StructuredLogViewer`), or **Cancel** to revert to the last saved values. The previous configuration is automatically restored the next time you launch the application.

> Preferences are disabled while a stream is active because the parser holds an immutable snapshot of the configuration. Stop the stream to edit them.

## Keyboard Shortcuts

| Action                         | Shortcut            |
| ------------------------------ | ------------------- |
| New session                    | `Ctrl+N`            |
| New window                     | `Ctrl+Shift+N`      |
| Open file(s)                   | `Ctrl+O`            |
| Open log stream                | `Ctrl+Shift+O`      |
| Open network stream            | `Ctrl+Shift+L`      |
| Save configuration             | `Ctrl+S`            |
| Save session                   | `Ctrl+Shift+S`      |
| Find                           | `Ctrl+F`            |
| Copy selected rows as JSON     | `Ctrl+C`            |
| Toggle Record Details pane     | `Ctrl+I`            |
| Toggle Anchors panel           | `Ctrl+K`            |
| Anchor selection (colour 1..8) | `Ctrl+1` … `Ctrl+8` |
| Remove anchor from selection   | `Ctrl+0`            |
| Clear every anchor             | `Ctrl+Shift+A`      |
| Jump to next anchor            | `F2`                |
| Jump to previous anchor        | `Shift+F2`          |
| Pause / Resume stream          | `Ctrl+Shift+P`      |
| Toggle Follow newest           | `Ctrl+Shift+T`      |
| Stop stream                    | `Ctrl+Shift+X`      |
| Show keyboard shortcuts        | `Ctrl+/`            |

## Troubleshooting

### "Failed to parse …" or "No valid log data found"

The selected file is not valid JSON Lines or logfmt, or every line failed to parse. Check the error dialog for the first few offending line numbers. If a file looks like logfmt but is being opened as JSON Lines (or vice versa), make sure the first non-empty line is unambiguously one format — auto-detection requires JSON Lines to start with `{` and logfmt to contain at least one bare `key=` token.

### Timestamps show as raw strings

Your timestamp key is not named `timestamp`, `time`, or `t`, or its format is not ISO 8601. Save a configuration, open it in a text editor, and add your column's key to the `keys` array of the timestamp column. You can also add additional patterns to `parseFormats` (e.g. `"%Y-%m-%d %H:%M:%S.%f"`). Reload the configuration and re-open the log.

### Timezone-related errors on first launch

On startup the application loads the IANA timezone database from a `tzdata/` folder shipped alongside the executable (or inside `Contents/Resources/tzdata` on macOS). If this folder is missing, a dialog reports the error and the application exits. Re-installing from the official release archive should resolve it.
