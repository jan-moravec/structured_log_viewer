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

## Opening Log Files

You can open a log file in three ways:

1. **File → Open…** (`Ctrl+O`) — opens a file picker that auto-detects whether the selected file is a log or a [configuration](#configurations) file.
1. **File → Open JSON Logs…** — forces the JSON parser regardless of content. Use this if auto-detection mistakenly treats a log as a configuration.
1. **Drag & drop** one or more files onto the main window.

Opening multiple files at once **merges** their records into a single table. If parsing errors occur, the first 20 are shown in a dialog; the rest are summarized as "… and N more error(s)".

For a file that is **still being written**, see [Stream Mode](#stream-mode-live-tail) below.

## Stream Mode (live tail)

Stream Mode opens a single log file and continuously tails it: it pre-fills the table with the last `N` complete lines on disk and then appends every new line as it is written. The mode is targeted at developer workflows where you want to watch a service's log as you reproduce a bug or run a smoke test, without alt-tabbing to a terminal.

### Opening a stream

Use **File → Open Log Stream…** (`Ctrl+Shift+O`) to pick a single file. Drag-and-drop continues to use the static path (the assumption is that drag-and-drop is for "open this archive", not "tail this"). Errors during open (file not found, permission denied) are reported via the existing parse-error dialog and the previous session is preserved.

While a stream is active the status bar shows `Streaming <file> — N lines, M errors`.

### Toolbar and **Stream** menu

A toolbar appears above the table while a stream is running, mirroring the **Stream** menu:

| Action         | Shortcut       | Notes                                                                   |
| -------------- | -------------- | ----------------------------------------------------------------------- |
| Pause / Resume | `Ctrl+Shift+P` | Pause freezes the visible table; new lines accumulate in memory.        |
| Follow tail    | `Ctrl+Shift+T` | Toggle auto-scroll. Auto-disengages when you scroll up to read history. |
| Stop           | `Ctrl+Shift+S` | Ends the session and leaves the rows visible as a static snapshot.      |

**Pause** and **Follow tail** are independent toggles. The status bar shows `Paused — N lines, K buffered` while paused, where `K` is the number of lines that arrived during the pause but have not yet been promoted to the visible table. **Resume** drains the buffer in a single batch.

### Retention cap

Stream Mode keeps at most `N` lines in memory; older lines are evicted in FIFO order so the resident set stays bounded across long-running tails. The default cap is **10 000 lines**. To change it, open **Settings → Preferences…** → **Streaming** → **Stream retention (lines)** (range 1 000 .. 1 000 000). The setting is persisted via `QSettings` and applies to future sessions and to the *current* session: lowering the cap on a running stream FIFO-trims existing rows immediately; lowering it while paused trims the paused buffer (visible rows are preserved, per the pause-suspends-eviction rule).

### File rotation

Stream Mode tolerates the common log-rotation patterns:

- **logrotate `create`** (rename + new file at the original path)
- **logrotate `copytruncate`** (copy aside, then truncate in place)
- **In-place truncation** (`: > app.log`)
- **Delete-then-recreate** (path disappears for a moment, then reappears)

When a rotation is detected, the viewer keeps every line that is already in memory, switches to the new on-disk content, and briefly appends `— rotated` to the status bar so you can tell it happened. **Rotated content is not pulled back from disk**: only the lines you have already seen survive the rotation in memory.

### Stop semantics and configuration menus

While a stream is active the **Configuration** menus (Save / Load / column manipulation) are disabled, mirroring the existing static-streaming behaviour. **Stop** ends the session, joins the background tailing thread within ~500 ms, and re-enables the configuration menus. Closing the application while a stream is active stops cleanly via the same teardown path.

### Non-goals

The following are **out of scope** for the current Stream Mode implementation:

- **Compressed rotations** (`app.log.1.gz` etc.) — only uncompressed rotations are handled.
- **Pulling rotated history off disk** — the viewer does not read `app.log.1` to recover lines older than the in-memory cap.
- **TCP / UDP / stdin / named-pipe sources** — only file tailing is wired up. The `LogSource` abstraction in `loglib` is designed to accommodate them in a future round.
- **Auto-detect "this file is being actively written → open in Stream Mode"** — Stream Mode is always an explicit `File → Open Log Stream…` action.
- **Per-file or per-session retention overrides** — the retention cap is a single application-wide setting.
- **Streaming for non-JSON formats** — currently only JSON Lines streams. The seam in `loglib` is format-agnostic so a future CSV / logfmt parser inherits the feature for free.

### Automatic Column Detection

The first time you open a file, Structured Log Viewer builds the column list from the keys it sees in the JSON records:

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

Open **Settings → Preferences…** to change the application's appearance:

- **Style** — the Qt widget style (e.g. `fusion`, `windows11`, `macOS`). Styles available depend on your platform.
- **Font** — the application-wide UI font family.
- **Font size** — 6 to 72 pt.

Changes preview live. Click **Ok** to persist them (stored via `QSettings` under the organization `jan-moravec` / application `StructuredLogViewer`), or **Cancel** to revert to the last saved values. The previous configuration is automatically restored the next time you launch the application.

## Keyboard Shortcuts

| Action                     | Shortcut       |
| -------------------------- | -------------- |
| Open file(s)               | `Ctrl+O`       |
| Open log stream            | `Ctrl+Shift+O` |
| Save configuration         | `Ctrl+S`       |
| Find                       | `Ctrl+F`       |
| Copy selected rows as JSON | `Ctrl+C`       |
| Pause / Resume stream      | `Ctrl+Shift+P` |
| Toggle Follow tail         | `Ctrl+Shift+T` |
| Stop stream                | `Ctrl+Shift+S` |

## Troubleshooting

### "Failed to parse …" or "No valid JSON data found"

The selected file is not valid JSON Lines, or every line failed to parse. Check the error dialog for the first few offending line numbers.

### Timestamps show as raw strings

Your timestamp key is not named `timestamp`, `time`, or `t`, or its format is not ISO 8601. Save a configuration, open it in a text editor, and add your column's key to the `keys` array of the timestamp column. You can also add additional patterns to `parseFormats` (e.g. `"%Y-%m-%d %H:%M:%S.%f"`). Reload the configuration and re-open the log.

### Timezone-related errors on first launch

On startup the application loads the IANA timezone database from a `tzdata/` folder shipped alongside the executable (or inside `Contents/Resources/tzdata` on macOS). If this folder is missing, a dialog reports the error and the application exits. Re-installing from the official release archive should resolve it.
