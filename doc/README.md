# Structured Log Viewer — User Guide

## Overview

Structured Log Viewer is a Qt 6 desktop application for inspecting JSON Lines log files. It displays each record as a row in a sortable, filterable table, auto-detects timestamp columns, and lets you search records using plain text, wildcards, or regular expressions.

## Supported Input Format

The application currently reads **JSON Lines** (also known as NDJSON): one JSON object per line, for example:

```json
{"timestamp":"2025-01-15T12:34:56.789Z","level":"info","component":"app","message":"started"}
{"timestamp":"2025-01-15T12:34:57.000Z","level":"error","component":"db","message":"connection refused"}
```

Empty lines are skipped. Lines that fail to parse are reported as errors but do not abort loading — valid records are still shown. Nested objects and arrays are preserved as their compact JSON string.

## Ingestion modes

Structured Log Viewer ingests logs through three distinct paths. Pick the one that matches what you are looking at:

| Aspect                 | Static mode                                                          | Stream Mode (live tail)                                                                               | Network Stream Mode (TCP / UDP)                                                                  |
| ---------------------- | -------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------ |
| **When to use**        | Post-mortem analysis of one or more *finished* log files.            | Watching a service's log file *as it is being written* — reproducing a bug, smoke-testing a release.  | Receiving JSON Lines pushed over the network — distributed services, dev loopback firehose.      |
| **How to open**        | `File → Open…` (`Ctrl+O`), `File → Open JSON Logs…`, or drag & drop. | `File → Open Log Stream…` (`Ctrl+Shift+O`). Drag & drop always uses static mode.                      | `File → Open Network Stream…` (`Ctrl+Shift+N`).                                                  |
| **Source per session** | One or many files; multi-file opens are **merged** into one table.   | Exactly one file.                                                                                     | One TCP listener (multiple concurrent clients allowed) or one UDP listener.                      |
| **Reads bytes via**    | Memory-mapped; parsed in parallel through the TBB pipeline.          | Buffered tail-reader; parsed line-by-line in a single worker.                                         | Asio TCP accept loop (with optional TLS) or Asio UDP receive loop; same line-by-line worker.     |
| **Memory**             | Whole file is parsed and held; row count grows with the file.        | Bounded by a configurable **retention cap** (default 10 000 lines, FIFO-evicted).                     | Same retention cap as Stream Mode; back-pressure also drops oldest *bytes* if the parser stalls. |
| **Reacts to new data** | No. The on-screen rows are a snapshot of the file at open time.      | Yes. New lines appear within ~250 ms of being written; survives `logrotate` and in-place truncations. | Yes. Each `\n`-terminated record lands as a new row as soon as it is received.                   |
| **Stream toolbar**     | Hidden.                                                              | Pause / Follow newest / Stop visible while a session is active.                                       | Same toolbar as Stream Mode.                                                                     |
| **Configuration menu** | Available between opens; disabled while a parse is in flight.        | Disabled for the lifetime of the session (the parser holds an immutable configuration snapshot).      | Same — disabled for the lifetime of the session.                                                 |

In Stream Mode and Network Stream Mode, **Stop** ends the session but keeps the visible rows around as a static snapshot you can keep filtering, sorting, and copying — handy when you only realised mid-tail that the bug already happened. Opening a new source (in any mode) clears the table first.

## Static Mode (Open files)

You can open a finished log file in three ways:

1. **File → Open…** (`Ctrl+O`) — opens a file picker that auto-detects whether the selected file is a log or a [configuration](#configurations) file.
1. **File → Open JSON Logs…** — forces the JSON parser regardless of content. Use this if auto-detection mistakenly treats a log as a configuration.
1. **Drag & drop** one or more files onto the main window.

Opening multiple files at once **merges** their records into a single table; the files are queued and parsed sequentially while sharing one column layout. If parsing errors occur, the first 20 are shown in a dialog when the queue drains; the rest are summarized as "… and N more error(s)". The status bar shows `Parsing <file> — N lines, M errors` while the queue is in flight.

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
- **Streaming for non-JSON formats** — currently only JSON Lines streams. The seam in `loglib` is format-agnostic so a future CSV / logfmt parser inherits the feature for free.

## Network Stream Mode (TCP / UDP)

Network Stream Mode listens on a local TCP or UDP port and ingests JSON Lines pushed to it by your application. It is intended for distributed services that cannot redirect their stdout/stderr to a file you can tail, and for "firehose into the GUI" loops during development. Each `\n`-terminated record becomes a row exactly the same way Stream Mode does, so the toolbar, retention cap, Pause / Follow newest / Stop, search, filters, and configurations all behave identically once the session is open.

### Opening a network stream

Use **File → Open Network Stream…** (`Ctrl+Shift+N`). The dialog asks for:

- **Protocol** — TCP or UDP.
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

- Any key named `timestamp`, `time`, or `t` (case-insensitive) is treated as a **timestamp column**. Its values are parsed with the ISO 8601 formats `%FT%T%Ez`, `%F %T%Ez`, `%FT%T`, `%F %T` and displayed with the format `%F %H:%M:%S` in the local timezone. Timestamp columns are moved to the front of the table.

  > **Note:** the heuristic is **destructive** for streaming opens. If a key matching the heuristic appears mid-parse, the corresponding column is flipped from `any` to `time` in-place and every row already in the table is back-filled with the parsed `TimeStamp`. The original raw string variant is replaced by the parsed value, so disabling the column's `time` type later (via a saved configuration) does not bring the original textual value back without re-opening the file.

- All other keys become generic columns with the format `{}` (pass-through).

You can persist a customized column layout and filter set via [configurations](#configurations).

## Navigating the Table

- **Sorting**: click a column header to sort ascending/descending. Click again to toggle direction. The initial load is unsorted (records appear in file order).
- **Column resizing**: drag the column header dividers. The last column stretches automatically to fill remaining space.
- **Smooth scrolling** is enabled by default (per-pixel) both vertically and horizontally.
- **Alternating row colors** improve readability on dense logs. The table respects the application's light/dark palette.

### Selecting and Copying Rows

- Click a row to select it. Hold `Ctrl`/`Shift` to extend the selection.
- **Edit → Copy** (`Ctrl+C`) copies the selected rows as the **original JSON text** (one line per row), so you can paste them back into another tool. Cell-level copy is not performed — rows are always copied whole.

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
1. Click **Ok**. The active filter is added as an entry in the **Filters** menu, titled with its value (or timestamp range).

### Editing or Clearing a Filter

Each filter entry in the Filters menu has **Edit** and **Clear** sub-actions:

- **Edit** re-opens the Filter Editor pre-populated with the current values.
- **Clear** removes just that filter.
- **Filters → Clear All** removes every filter at once.

Filters are live: the table updates immediately when a filter is added, edited, or cleared.

## Configurations

A *configuration* captures the current column layout (headers, keys, print format, timestamp parse formats). Configurations are saved as JSON files, and can be loaded into future sessions to skip auto-detection and to enforce a consistent layout across teammates.

- **File → Save Configuration…** (`Ctrl+S`) — writes the current layout to a `.json` file.
- **File → Load Configuration…** — loads a configuration file and clears any open logs. Open logs again afterwards to apply the layout.

Because `Open…` auto-detects configurations, double-clicking a saved configuration file also works (it loads the layout without opening any logs).

> Filters are not currently persisted in configurations.

## Preferences

Open **Settings → Preferences…** to change application-wide settings. The dialog is split into two groups:

**Appearance** — previewed live; reverted on Cancel.

- **Style** — the Qt widget style (e.g. `fusion`, `windows11`, `macOS`). Styles available depend on your platform.
- **Font** — the application-wide UI font family.
- **Font size** — 6 to 72 pt.

**Streaming** — applied transactionally on **Ok**.

- **Stream retention (lines)** — the cap on how many lines [Stream Mode](#retention-cap) keeps in memory (range 1 000 .. 1 000 000, default 10 000).
- **Show newest lines first** — orient new stream lines at the top of the table instead of the bottom (see [Newest lines first](#newest-lines-first)).

Click **Ok** to persist (stored via `QSettings` under the organization `jan-moravec` / application `StructuredLogViewer`), or **Cancel** to revert to the last saved values. The previous configuration is automatically restored the next time you launch the application.

> Preferences are disabled while a stream is active because the parser holds an immutable snapshot of the configuration. Stop the stream to edit them.

## Keyboard Shortcuts

| Action                     | Shortcut       |
| -------------------------- | -------------- |
| Open file(s)               | `Ctrl+O`       |
| Open log stream            | `Ctrl+Shift+O` |
| Open network stream        | `Ctrl+Shift+N` |
| Save configuration         | `Ctrl+S`       |
| Find                       | `Ctrl+F`       |
| Copy selected rows as JSON | `Ctrl+C`       |
| Pause / Resume stream      | `Ctrl+Shift+P` |
| Toggle Follow newest       | `Ctrl+Shift+T` |
| Stop stream                | `Ctrl+Shift+S` |

## Troubleshooting

### "Failed to parse …" or "No valid JSON data found"

The selected file is not valid JSON Lines, or every line failed to parse. Check the error dialog for the first few offending line numbers.

### Timestamps show as raw strings

Your timestamp key is not named `timestamp`, `time`, or `t`, or its format is not ISO 8601. Save a configuration, open it in a text editor, and add your column's key to the `keys` array of the timestamp column. You can also add additional patterns to `parseFormats` (e.g. `"%Y-%m-%d %H:%M:%S.%f"`). Reload the configuration and re-open the log.

### Timezone-related errors on first launch

On startup the application loads the IANA timezone database from a `tzdata/` folder shipped alongside the executable (or inside `Contents/Resources/tzdata` on macOS). If this folder is missing, a dialog reports the error and the application exits. Re-installing from the official release archive should resolve it.
