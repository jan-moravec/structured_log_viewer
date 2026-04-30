# PRD: Log File Stream Mode

## 1. Introduction / Overview

Today, Structured Log Viewer opens a log file as a static, fully-parsed snapshot. The user sees only what was on disk at the moment of opening, and must re-open the file to see new lines. This is the right mode for forensic analysis of large historical logs (`loglib`'s memory-mapped, compact-storage, TBB-parallel pipeline is optimised exactly for that) but it is not useful while a service is running.

This PRD adds a **Stream Mode** for opening one or more log files, in which the viewer continuously tails the file(s), shows the newest lines as they are written, and lets the user pause, scroll back through recent history, and resume — without reloading. Stream mode is targeted at thousands to low-tens-of-thousands of lines kept resident at any one time, not the millions-of-lines-per-file workload the existing path is tuned for. It is therefore allowed to skip the mmap / compact-storage / TBB optimisations and to favour a simpler, owning data model that survives file rotation (rotated content is gone from disk, but stays visible in memory).

To keep the project healthy, this feature is also the occasion to introduce a **`LogSource` abstraction** in `loglib` — a new seam upstream of the existing `LogParser`, behind which both the current mmap-based "open file" path and the new tailing path live. The same seam is the future home of TCP, UDP, stdin, and named-pipe sources, which are out of scope for this PRD but must not be architecturally precluded.

**Goal:** ship a Stream Mode that opens a log file, displays the last N lines, follows new lines as they are written, survives common log rotations, exposes pause and follow-tail controls in the GUI, and does so without breaking or regressing the existing static-file open path.

## 2. Goals

1. **G1 — Live tail.** When a user opens a log file in Stream Mode, the table shows the last N lines from disk and continues to append new lines as the file grows, with a typical end-to-end latency under 500 ms from `write()` on the producer to row-visible in the GUI.
1. **G2 — Pause / resume.** The user can freeze the visible table at any time and resume later without losing intervening lines (subject to the configured retention cap).
1. **G3 — Follow tail.** The user can toggle whether the view auto-scrolls to the newest line. Auto-scroll engages by default and disengages automatically when the user scrolls up to read history.
1. **G4 — Rotation tolerance.** A standard log rotation (rename + new file, copytruncate, or in-place `truncate`) does not crash the viewer, does not silently lose new lines, and does not erase already-displayed history from memory.
1. **G5 — Bounded memory.** The streamed view never exceeds a configurable line cap (default `10 000`); once the cap is reached, oldest lines are evicted in FIFO order.
1. **G6 — `LogSource` abstraction.** `loglib` gains a `LogSource` interface that both the existing mmap-based file open and the new tailing source implement. The existing static-file open path keeps its current performance characteristics (no regression on the `[large]` / `[wide]` benchmarks; same `±3 %` bar that already governs the project).
1. **G7 — Multi-source future-proofing.** Even though Stream Mode opens a single source at a time in this round, the data flow must allow multiple `LogSource`s to feed a single `LogTable` in the future without a re-architecture.
1. **G8 — Format-agnostic.** Stream Mode reuses the existing `LogParser` / `JsonParser` interface and `KeyIndex` / `LogConfiguration` / `LogTable` machinery, so a future CSV / logfmt parser inherits Stream Mode for free.

## 3. User Stories

- **US-1 — Live debugging.** As a developer running my service locally, I want to open `app.log` in Stream Mode so that I can watch new error lines appear as I reproduce a bug, without alt-tabbing to a terminal.
- **US-2 — Pause to inspect.** As a developer, I want to click **Pause** when I see an interesting line, read the surrounding rows undisturbed, and then click **Resume** to catch up — without losing the lines that arrived during the pause.
- **US-3 — Scroll back without losing the live edge.** As a developer, I want to scroll up to read older lines while the file keeps growing; the table should not yank me back to the bottom. When I scroll back to the bottom, the live tail should re-engage.
- **US-4 — Survive rotation.** As a developer running a service that rotates `app.log` every hour, I want the viewer to keep showing my pre-rotation lines (which are no longer on disk) and seamlessly continue tailing the freshly-rotated file.
- **US-5 — Filter and search a live tail.** As a developer, I want to apply a level-`error` filter or use Find on a streaming view, and have new matching lines appear (and non-matching ones be hidden) as they arrive.
- **US-6 — Configure retention.** As a developer who runs noisy services, I want to raise the in-memory retention cap from 10 000 to e.g. 100 000 lines via Preferences so I can scroll further back without re-opening the file.
- **US-7 — No surprise on the static path.** As an existing user who opens 200 MB historical JSON logs, I want the existing **File → Open…** flow to keep its performance and behaviour unchanged.

## 4. Functional Requirements

### 4.1 Opening a file in Stream Mode

1. The application must expose a new menu item **File → Open Log Stream…** (suggested shortcut `Ctrl+Shift+O`) that opens a file picker.
1. Drag-and-drop must continue to use the static path (existing behaviour); Stream Mode is opened only via the explicit menu entry. Rationale: drag-and-drop a file is overwhelmingly used for "open this archive", not "tail this".
1. Opening a file in Stream Mode replaces any currently-open log session (consistent with the existing single-session model). If a static or stream session is already open, the user is prompted before the existing session is closed (or the existing session is unconditionally closed; behaviour matches today's static-open flow — see Open Question OQ-1).
1. The file picker accepts a single file in this round. The interface must not preclude multiple files in the future (see G7 / 4.7).
1. On open, the application must:
   1. **Pre-fill** the table with the last `N` *complete* lines on disk, where `N` is the configured retention cap (4.5). A line is *complete* iff it terminates with a `\n`; a trailing partial line (no terminating `\n`) is **not** pre-filled, and instead becomes the seed of the partial-line buffer (§7 — *Line buffering*) at the file's pre-fill EOF. If the file has fewer than `N` complete lines, all of them are shown.
   1. Begin a **tailing read** that emits every subsequent line written to the file as a new row. The tailing read offset is the byte offset at which pre-fill stopped consuming bytes — i.e. the start of any pre-fill partial line, or the file's pre-fill EOF if the file ended on `\n`. Lines crossing the pre-fill / tail boundary must be emitted exactly once.
1. The pre-fill must read the tail of the file efficiently — i.e. seek backward from EOF in fixed-size chunks (e.g. 64 KiB), counting newlines until `N` complete lines are found, rather than parsing the whole file. For files smaller than the chunk size, read the whole file. Pre-fill is a `TailingFileSource`-specific concern; the `LogSource` abstraction itself does not require pre-fill, since stdin / TCP / UDP cannot rewind (4.9.8).
1. Errors during open (file not found, permission denied) must be reported via the existing parse-error dialog and the application must remain in its previous state (no half-opened session).
1. The status bar must display `Streaming <file> — N lines, M errors` while the stream is active, mirroring the existing static-open status string.

### 4.2 Pause and Resume

1. The toolbar (or the **View** menu) must expose a **Pause** toggle action.
1. When **Pause** is active:
   1. The visible table must not change. No new rows are shown; no rows are evicted (FIFO retention is suspended); columns may still grow only if the user changes Configuration (which is unlikely while paused — see 4.6).
   1. New lines arriving from the source must continue to be received, parsed, and **buffered in memory** outside the visible model.
   1. The status bar must display a clear `Paused — K buffered` indicator.
   1. The retention cap (4.5) must still bound *total* memory: the visible rows plus the paused-buffer rows together must not exceed the cap. If the buffered count would push the total above the cap, the **paused buffer** drops oldest entries (the visible rows are preserved). See OQ-2 for the rationale and the rejected alternative.
   1. The bridging streaming sink must implement Pause as a **worker-side** flag that *redirects* parsed batches into the in-memory paused buffer rather than continuing to post `Qt::QueuedConnection` lambdas to the GUI thread per batch. Posting per-batch lambdas across an indefinite pause would otherwise accumulate Qt-event-queue entries proportional to the inter-arrival rate — a third memory pool not bounded by 4.5 and invisible to the `K buffered` counter. Resume drains the paused buffer by posting a single coalesced batch back to the GUI thread.
1. When **Resume** is clicked:
   1. The buffered batches must be appended to the visible model in arrival order.
   1. The standard FIFO eviction (4.5) must apply to bring the visible rows back within the cap.
   1. The status bar must return to the running `Streaming <file> — N lines, M errors` indicator.
1. **Pause** and **Follow tail** (4.3) are independent toggles. Toggling Pause must not change the Follow-tail state and vice versa.

### 4.3 Follow Tail (auto-scroll)

1. The toolbar (or the **View** menu) must expose a **Follow tail** toggle action, on by default.
1. When **Follow tail** is on, every appended row must cause the table to scroll so the newest row is visible at the bottom.
1. **Follow tail** must auto-disengage when the user manually scrolls up away from the bottom (the "VS Code terminal" pattern). It must auto-re-engage when the user scrolls back to the bottom.
1. When the user has applied a sort by a non-time column, **Follow tail** must continue to scroll to the row corresponding to the *newest* line (i.e. the row most recently appended), not to the bottom of the current sort order. (This may visually "jump" in unsorted-time views; the simpler alternative is to grey out / disable Follow tail when sorted by a column other than the source order or a time column. Either is acceptable; pick the simpler one.)

### 4.4 Auto-detection in the file picker (no behavioural change to static path)

1. **File → Open…** continues to behave exactly as today: it auto-detects format and opens via the static path. Note that the existing static path is *itself* parsed via `JsonParser::ParseStreaming` over an mmap'd `LogFile` (the GUI streams parsed batches into the table for responsiveness while the file is finite); this PRD reserves the term **Stream Mode** strictly for the live-tail case driven by `TailingFileSource`.
1. There is no auto-detection of "this file is being actively written → open in Stream Mode". Stream Mode is always an explicit user choice in this round.

### 4.5 In-memory retention

1. The viewer must retain at most `N` log lines in memory, where `N` is a single application-wide setting visible in **Settings → Preferences → Streaming** as **"Stream retention (lines)"**.
1. Default `N = 10 000`. Allowed range `1 000 .. 1 000 000`. Persisted via the existing `QSettings` mechanism.
1. When a new line would push the live row count above `N`, the **oldest** line in the model must be removed before the new line is appended. Removal must use the proper `QAbstractTableModel::beginRemoveRows` / `endRemoveRows` signals.
1. Eviction must update the running line counter shown in the status bar (`N lines` is the *current visible* count, not an "all-time seen" count). Errors are cumulative for the session.
1. The retention setting must be editable while a stream is active.
   1. **While running:** lowering it must immediately FIFO-trim the existing rows (using `beginRemoveRows` / `endRemoveRows`); raising it has no immediate effect (rows already evicted are gone).
   1. **While paused:** the new value is recorded but no eviction runs; the trim is deferred to Resume so 4.2.2.i ("Pause suspends FIFO eviction on visible rows") is preserved. If the new value is also smaller than the paused-buffer size, 4.2.2.iv applies immediately — the paused buffer trims to the new cap minus the visible row count.
1. Selection, the Find bar's current match, and any active filters must remain consistent across eviction:
   1. Selections (and the Find current match) on rows inside the evicted range are **dropped**. Selections on rows that survive the eviction are preserved by Qt's standard `rowsRemoved` row-shifting — no custom `LineId`-based selection persistence is required (deliberately a non-goal; see §5.11).
   1. Filter rules in `LogFilterModel` operate row-locally and survive eviction without special handling; the proxy model receives the corresponding `rowsRemoved` signal and updates its mapping.
1. **Pause** suspends FIFO eviction on the visible rows (4.2.2.i); see 4.2.2.iv for the paused-buffer eviction policy.

### 4.6 Configuration / column behaviour during streaming

1. Stream Mode must use the same `LogConfiguration` / `LogConfigurationManager` machinery as the static path. New keys arriving on later batches grow the column layout via `AppendKeys`.
1. Auto-detection of timestamp columns (the existing `timestamp` / `time` / `t` heuristic and the back-fill of newly-promoted time columns) must work for streamed lines.
1. The **Configuration** menus (Save / Load / column manipulation) follow the same gating rule already used by the static streaming path: they are locked while a stream is active, and re-enabled when the stream is stopped.
1. **File → Save Configuration…** and **File → Load Configuration…** must work in Stream Mode in the same way they work in static mode (Save snapshots the current layout; Load is unavailable until the stream is stopped).

### 4.7 Stopping a stream

1. The toolbar must expose a **Stop** action that ends the streaming session and leaves the most-recently-buffered rows visible (the model becomes a static snapshot of what was in memory at stop time).
1. **Stop** must terminate any background tailing thread and the file-system watcher cleanly within 500 ms. It must not deadlock on a producer that is still actively writing.
   1. **Order of teardown is mandatory:** first `LogSource::Stop()` (unblocks any blocking `Read` / `WaitForBytes` in the worker), then signal the parser `ParserOptions::stopToken`, then `waitForFinished()` on the worker future, then `DropPendingBatches()` on the bridging sink (mirrors the current `LogModel::~LogModel` / `LogModel::Clear()` sequence). The two stop signals are **separate** because the parser's stop token alone does not unblock a worker parked inside `Read` / `WaitForBytes`; the source-level stop is what releases that I/O wait.
   1. **Partial-line flush.** Any bytes still in the source's partial-line buffer at Stop time must be flushed as a final synthetic line, **unless** a rotation is concurrently in progress (in which case the partial buffer is discarded, per 4.8.7.ii). Without this flush, a producer that exits without writing a trailing `\n` would silently lose its last line.
1. After **Stop**, **Pause** / **Resume** / **Follow tail** become disabled. Any rows still in the paused buffer at Stop time are flushed into the visible model first (so Stop never silently discards already-parsed lines) before the buffer is released. The user can still sort, filter, search, and copy rows.
1. Closing the application while a stream is active must Stop cleanly — i.e. follow the teardown order in 4.7.2.i before tearing down the borrowed model, mirroring the existing `LogModel::~LogModel` invariant.

### 4.8 File rotation handling

The viewer must detect and recover from the following rotation patterns, *without* clearing the in-memory rows already shown to the user:

1. **Rename-and-create (logrotate `create`).** `app.log` is renamed to `app.log.1` and a fresh empty `app.log` is created.
1. **Copy-and-truncate (logrotate `copytruncate`).** `app.log` is copied to `app.log.1` and then truncated in place (size goes to 0, path stays the same).
1. **In-place truncation (`: > app.log`).** Same as 2 from the viewer's perspective.
1. **Delete-then-recreate.** `app.log` disappears for some interval and reappears (the producer recreates it).

The detection rules:

5. The viewer must use [efsw](https://github.com/SpartanJ/efsw) (or a comparable cross-platform watcher; see Technical Considerations) to receive native filesystem change notifications, with a polling fallback (`stat` every 250 ms) on file systems where the native event stream is unavailable, throttled, or known-unreliable (network shares, container bind mounts, WSL `/mnt`, FAT/exFAT, and macOS where `FSEvents` only fires at directory granularity). On filesystems where the native watcher is reliable, the poll runs as a long-period (e.g. every 5 s) heartbeat to catch dropped events. Watching a single file generally requires watching its **parent directory** on Linux (`inotify`) and macOS (`FSEvents`); the watcher implementation must filter directory events down to the open file's basename and gracefully handle the case where the parent directory has many unrelated files.
1. Rotation is detected by inspecting the *path* and the *open handle*. Conditions are evaluated in this order on every poll tick (and on every native event); the **first** match wins:
   1. **File-identity changed.** The path's current file index (POSIX `stat::st_ino`; Windows `nFileIndexLow:nFileIndexHigh` from `GetFileInformationByHandle`) differs from the originally-opened file's. → rename-and-create or delete-then-recreate. **Recovery:** close the old handle, open the new path, seek to offset 0, resume.
   1. **Path missing.** The path cannot be `stat`'d (`ENOENT` / `ERROR_FILE_NOT_FOUND`). → delete-then-recreate in progress, or transient. **Recovery:** enter the "waiting" state (4.8.8), retry the path every 250 ms; on first successful re-open, install the new handle, seek to offset 0, resume. If the path reappears with non-zero size between two polls, branch (i) wins on the next poll.
   1. **Size shrunk on the open handle.** The open handle's `fstat::st_size` (POSIX) / `GetFileSizeEx` (Windows) is strictly less than the last-known read offset, *and* branch (i) did not match. → copytruncate or in-place truncation. **Recovery:** seek the existing handle to offset 0, resume reading. Do **not** re-open (the handle may have an exclusive share-mode that the producer is relying on; reopening could race with the truncation).
      On filesystems where `nFileIndexLow:nFileIndexHigh` is not stable (FAT/exFAT, some SMB) the size-shrunk branch covers copytruncate; rename-and-create still requires the path-vs-handle inode comparison and degrades to undetected on those filesystems (a documented limitation).
1. On any rotation branch, the viewer must:
   1. Discard the partial-line buffer (its content cannot be completed from the new content).
   1. Apply the recovery for the matched branch (re-open + seek-0 for branches i / ii; seek-0 only for branch iii).
   1. **Keep all existing in-memory rows** intact. They age out naturally via FIFO eviction.
   1. Increment the session-local `LineId` counter monotonically across the rotation (4.10.4) so post-rotation lines do not collide with pre-rotation lines.
   1. Briefly indicate rotation in the status bar (e.g. `Streaming <file> — rotated, 12345 lines, 0 errors`).
1. If the file has not yet reappeared (branch ii's "Path missing" persists across more than one poll), the viewer must enter a "waiting" state, retry every 250 ms, and indicate `Source unavailable` in the status bar. The retry loop ends when the user presses **Stop** or when the file reappears.
1. Multiple rapid rotations within a short window (\<1 s) must collapse into a single rotation event (debounce on the watcher) — both to avoid status-bar flicker and to keep `mPostSnapshotTimeKeys` back-fill from re-running on every transient burst.
1. Rotation detection must continue to work while the stream is **Paused**: ingestion (and therefore rotation detection) continues even while the visible model is frozen. Paused-buffered rows remain valid across rotation; only the partial-line buffer (which is the source's, not the sink's) is discarded.

### 4.9 Library: `LogSource` abstraction

1. `loglib` must gain a new abstract base class **`LogSource`** representing "a thing that produces log bytes / lines over time, with optional rotation semantics". The concrete implementations introduced in this PRD:
   1. **`MappedFileSource`** — wraps the existing `LogFile` (memory-mapped, finite, parsed once). The existing static-open path is refactored to go through this source.
   1. **`TailingFileSource`** — opens a file, optionally seeks to the tail, follows growth via the watcher in 4.8.5, and survives rotation per 4.8.6.
1. `LogSource` is a **byte / line producer**. It deliberately does **not** know about `StreamingLogSink` or `ParserOptions` — coupling those concerns to the source layer would conflate parser orchestration with byte delivery and would make stdin / TCP / UDP sources awkward to add later. The recommended public surface (sketch in §7) is:
   1. **`Read(buffer)`** — yields a contiguous span of bytes since the last call. Returns 0 on transient EOF for `TailingFileSource`; returns terminal EOF for `MappedFileSource` once the file is exhausted (and analogously for any future stdin / TCP / UDP source whose peer has closed).
   1. **`WaitForBytes(timeout)`** — parks the caller until at least one byte is available, the deadline elapses, or `Stop()` is called, so the parser worker can sleep instead of busy-polling.
   1. **`Stop()`** — unblocks any in-flight `Read` / `WaitForBytes` and causes subsequent calls to report terminal EOF. Must be safe to call from any thread, including the GUI thread during model teardown. **Distinct from** `ParserOptions::stopToken` (4.7.2.i): the source's `Stop` releases I/O so the parser's hot loop can observe the parser stop token at the next batch boundary.
   1. **`DisplayName()`** — for status-bar wiring.
   1. An optional rotation-event hook (callback or sink) so the parser can flush its partial-line buffer (4.7.2.ii / 4.8.7.i) and so `LogModel` can surface the rotation indicator in the status bar.
1. The existing `LogParser` interface stays. Its `ParseStreaming(LogFile&, sink, options)` overload is kept for backward compatibility on the static path; a new `ParseStreaming(LogSource&, sink, options)` overload is the recommended shape going forward. Format-specific parsers receive bytes from the source rather than directly from a `LogFile`. The parser is the *driver*; the source is the *byte producer*.
1. `JsonParser` must be refactored so that its hot loop operates on bytes / lines yielded by a `LogSource`, not on a memory-mapped pointer specifically. The TBB pipeline (`detail::RunParserPipeline`) remains the implementation for `MappedFileSource` (Stage A pulls full chunks via `LogSource::Read`). For `TailingFileSource`, a simpler single-threaded line-by-line parsing path is acceptable — and preferred, given the throughput target (thousands of lines/s, not millions). The two parsing paths must share key interning, escape decoding, and timestamp promotion via the existing `WorkerScratchBase` / `BuildTimeColumnSpecs` machinery.
1. The existing `[large]` / `[wide]` / `[allocations]` / `[cancellation]` benchmarks must continue to pass within the project's standing `±3 %` regression bar after the refactor (per the convention in `CONTRIBUTING.md` § Benchmarking).
1. The following `loglib` machinery must be **shared** between the two sources without duplication:
   1. `KeyIndex`,
   1. `LogConfiguration` / `LogConfigurationManager` and its `AppendKeys` flow,
   1. `LogTable` (the same `BeginStreaming` / `AppendBatch` / `EndStreaming` flow),
   1. `StreamingLogSink`,
   1. timestamp parsing (`TryParseTimestamp`) and back-fill (`BackfillTimestampColumn`).
1. `LogFile`'s mmap-specific concerns (line-offset table, `OwnedString` arena, `MmapSlice` compact-value variants) and `LogLine`'s compact 16-byte storage are explicitly **allowed to stay file-mode-only**. Stream Mode introduces a sibling lightweight log-record type (recommended name: `StreamLogLine`) and a sibling reference type (recommended: `StreamLineReference`) that together own:
   1. The line's parsed values directly (e.g. `std::vector<std::pair<KeyId, LogValue>>` with `std::string` payloads — no arena, no `MmapSlice`, no `LogFile *`).
   1. A copy of the original raw line bytes, for the `LogModelItemDataRole::CopyLine` data role. The existing `LogFileReference::GetLine()` reads from the mmap, which `TailingFileSource` does not have; `StreamLineReference::GetLine()` returns its owned bytes instead.
   1. A session-local monotonic `LineId` (4.10.4).
      `LogTable` must be able to ingest both line types — implementation may be via a thin `ILogRecord`-style interface, an `std::variant<LogLine, StreamLogLine>` per row, or a templated `LogTable` — left to the implementer's design judgement.
      The today's-`LogData::AppendBatch` invariant `assert(mFiles.size() == 1)` (which routes `lineOffsets` into `LogFile::AppendLineOffsets`) is **bypassed** for the streaming path: streamed batches feed the `LogTable` directly without going through the line-offsets accumulator. The static path's invariant stays; the assertion is gated on the line-type variant.
1. The `LogSource` abstraction must be designed so that future implementations (`StdinSource`, `TcpSource`, `UdpSource`, `NamedPipeSource`) can plug in without `loglib` changes beyond adding the new subclass. Specifically, `LogSource` must not assume a `std::filesystem::path` or a finite size in its public API.
1. Stream Mode **must not** use mmap for the tail; use buffered `std::ifstream` or `read(2)` / `ReadFile`. Detailed rationale (Linux truncation invalidates the mapping; Windows `mio` mappings open with `FILE_SHARE_READ` only and block rename/unlink; `tmpfs` / SMB / NFS are unreliable) is in §7 — *No mmap on the tail*.

### 4.10 GUI: streaming sink and model

1. The existing `QtStreamingLogSink` must be extended (or a sibling sink introduced) to support the long-lived tailing case: every batch arriving from `TailingFileSource` is dispatched to the GUI thread via `Qt::QueuedConnection`, where it goes through `LogModel::AppendBatch` exactly as today. **Pause** (4.2.2.v) is implemented as a worker-side flag inside the sink that *redirects* incoming batches into the bounded paused buffer instead of `QueuedConnection`-posting per batch; Resume drains the buffer with a single coalesced post.
1. `LogModel` must gain a new method (recommended name: `BeginStreaming(std::unique_ptr<LogSource>)`) that opens a stream session against a `LogSource`. The model takes ownership of the source for the lifetime of the session and is responsible for spawning the parser worker against it. The existing `BeginStreaming(std::unique_ptr<LogFile>, parseCallable)` overload is rewritten as a thin wrapper that constructs a `MappedFileSource` and forwards (so the static path still works unchanged from the caller's perspective).
1. `LogModel` must implement FIFO eviction (4.5) on each batch:
   1. Predict the post-mutation row count via `LogTable::PreviewAppend`.
   1. If the predicted count would exceed the cap, fire `beginRemoveRows` / `endRemoveRows` for the prefix of the visible model that is about to be dropped, **before** `beginInsertRows` for the appended rows. This widens the existing append-only sequence (which only fires `begin/endInsertRows` and `begin/endInsertColumns`) into a remove-then-insert sequence per batch.
   1. When a batch's row count alone exceeds the retention cap, the eviction collapses the prefix of the batch directly (the head of the batch is discarded before `LogTable::AppendBatch`) so the visible model never breaches the cap and so per-batch eviction stays O(cap) rather than O(batch size).
1. Each ingested line must carry a stable monotonic session-local `LineId`:
   1. For `MappedFileSource`-driven sessions the `LineId` matches today's `LogFileReference::GetLineNumber()` (1-based, file-local).
   1. For `TailingFileSource`-driven sessions the `LineId` is a session-local counter that increments monotonically across rotations (so post-rotation lines do not collide with pre-rotation lines) and starts at 1 at `BeginStreaming` time. Rotated lines' `LineId`s are *not* reset on the rotation event (4.8.7.iv).
   1. The `LineId` is **not** used to track GUI selection (which is handled by Qt's standard `rowsRemoved` mechanism per 4.5.6); it is the canonical identity used by tests, the streaming line ledger, error-line composition (today's `Error on line N: ...` formatting in the parser pipeline), and the rotation-event log. The `LogModelItemDataRole::CopyLine` role continues to read raw bytes via the line's reference (`LogFileReference::GetLine()` for static, `StreamLineReference::GetLine()` for streaming).
1. Errors emitted by `OnBatch` must accumulate in the existing `mStreamingErrors` vector and tick the existing `errorCountChanged` signal exactly as today.

### 4.11 Find and filtering during streaming

1. The Find bar must work over the visible streamed rows. New lines arriving after a Find search must not auto-shift the user's current match; the user must press **Next** / **Previous** to advance.
1. All existing filters (text match modes, timestamp range) must work over streaming rows. New lines that fail the active filter must be retained in the underlying model (so they re-appear when the filter is cleared) but hidden by `LogFilterModel`.
1. Eviction (4.5) must use the underlying source-order line, not the proxy model order. The proxy model receives the corresponding `rowsRemoved` signal and updates its mapping.

## 5. Non-Goals (Out of Scope)

1. **TCP / UDP / stdin / named-pipe sources.** Only `TailingFileSource` ships in this PRD. The `LogSource` abstraction must accommodate them but no concrete implementation is required.
1. **Tabbed or multi-pane UI.** Single-session model continues. Multiple `LogSource`s feeding one view is left as a future design (the abstraction should not preclude it).
1. **Pulling rotated history off disk.** When rotation is detected, the viewer does not read `app.log.1` (or the rename target) to recover lines older than what is in memory. Rotated content visible to the user is exclusively what was already buffered before rotation.
1. **Compressed-rotation support.** No `gzip` / `bzip2` reader for `app.log.1.gz` and friends.
1. **Source health metrics, throughput counters, alerting hooks.** Status bar shows line count, error count, and a coarse state (`Streaming` / `Paused` / `Source unavailable`); no detailed telemetry.
1. **A second pass of large-file performance optimisation.** The static-open performance bar is "no regression"; the stream path is explicitly allowed to be slower per line.
1. **Streaming mode for non-JSON formats.** Currently only `JsonParser` ships. Stream Mode reuses the parser interface so that a future CSV / logfmt parser inherits the feature, but no new parser is added in this PRD.
1. **Persisting stream-mode session state.** Reopening the application does not re-attach to the previously-streamed file; the user must reopen it explicitly.
1. **Per-file or per-session retention overrides.** A single application-wide `N` is enough for this round.
1. **Auto-detection of "this file is being actively written".** Stream Mode is always an explicit `File → Open Log Stream…` action.
1. **`LineId`-based selection / Find-match persistence across FIFO eviction.** When eviction removes rows, selections / Find matches inside the evicted range are dropped (4.5.6.i); selections on surviving rows are preserved by Qt's standard `rowsRemoved` row-shifting. Re-establishing a removed selection by `LineId` after eviction would require a parallel `QSet<LineId>` ledger and a custom `selectionModel` integration, which is out of scope for this round.

## 6. Design Considerations (UI / UX)

- **Toolbar.** Above the table, expose three icon buttons that are visible (and only enabled) in Stream Mode:
  1. **Pause / Resume** — toggle, with distinct icons for the two states.
  1. **Follow tail** — toggle, on by default.
  1. **Stop** — stops the stream.
     Each action is also reachable from the **View** menu (or a new **Stream** menu) and bound to a keyboard shortcut. Final bindings are an implementation detail; see OQ-8 for the constraints (avoid `Ctrl+Space`, `Ctrl+End`, `Ctrl+.`).
- **Status bar.** Three states the label cycles through during a stream: `Streaming <file> — N lines, M errors` (running), `Paused — N lines, K buffered` (paused), `Source unavailable — last seen <file> — N lines, M errors` (waiting for the file to reappear). After **Stop**, the streaming label is hidden and the status bar reverts to its empty static state, mirroring today's static-open behaviour where `mStatusLabel` is hidden when `mStreamingActive` is false.
- **Rotation indicator.** A short-lived (e.g. 3 s) substring `— rotated` appended to the status label after each detected rotation, so the user is aware their on-disk source has flipped.
- **Column auto-resize during streaming.** The static path runs `MainWindow::UpdateUi` (which calls `resizeColumnToContents` on each column) once the parse finishes. For Stream Mode, the equivalent runs **once on the first non-empty batch** after `BeginStreaming` so the initial widths fit the pre-filled rows; thereafter the user resizes manually. Repeating auto-resize on every batch would yank columns under the user's mouse and is explicitly avoided.
- **Preferences.** A new **Streaming** group in **Settings → Preferences…** with a single field, **Stream retention (lines)**, default `10 000`, range `1 000 .. 1 000 000`. Tooltip: *"Maximum number of streamed lines kept in memory. Oldest lines are dropped when the cap is reached. Higher values use more memory."* Note: today's `PreferencesEditor` is appearance-only and is closed via OK/Cancel against `AppearanceControl`; the new field must persist via `QSettings` independently of the appearance settings so a Cancel-from-appearance does not also revert the retention change (or a single Apply pattern that covers both — implementer's choice, document the chosen semantics).
- **Open dialog.** **File → Open Log Stream…** uses the same file-picker UX as **File → Open JSON Logs…**.
- **No mockups attached.** The visual design should follow the existing application's conventions (Qt Widgets, current toolbar style).

## 7. Technical Considerations

- **Filesystem watcher choice.** `efsw` is the suggested library: header + small static lib, native backends (`ReadDirectoryChangesW` / `inotify` / `FSEvents`) with its own polling fallback. Add it via `FetchContent` next to the existing dependencies (`cmake/FetchDependencies.cmake`), with a matching `USE_SYSTEM_EFSW=ON` knob. Note that `efsw` builds an actual static library (not header-only), so on Windows MSVC the runtime-library setting (`/MD` vs `/MT`) and CRT linkage must match the rest of the project — coordinate via `cmake/FetchDependencies.cmake`. Acceptable alternatives if `efsw` proves problematic: `Qt`'s `QFileSystemWatcher` (already a dependency, but its limits are well-known on Linux — `inotify` watch limit, no rename detection on the file itself, only on the parent directory), or a hand-rolled per-platform implementation. The poll-only fallback (4.8.5) is mandatory on filesystems where the native watcher is unreliable; on filesystems where it is reliable, the poll runs only as a long-period heartbeat to catch dropped events (avoiding double-firing on every file write).
- **No mmap on the tail.** mmap is invalidated by truncation on Linux (SIGBUS on access past the post-truncation EOF), and on Windows `mio`-style mappings open the file with `dwShareMode = FILE_SHARE_READ` (no `FILE_SHARE_DELETE`), which blocks rename and unlink — exactly the operations a rotating producer needs. Behaviour on `tmpfs` and SMB / NFS mounts is unreliable. Use buffered standard I/O (`std::ifstream`, or `read(2)` / `ReadFile`) for `TailingFileSource`. (The static path's mmap of a finite file, where the producer is gone, is unaffected and stays.)
- **Line buffering.** Reads can land mid-line. The tailing source must keep a partial-line buffer between reads and flush it as a single line only when a `\n` arrives. Two exceptions to the "wait for `\n`" rule:
  1. On rotation detection (4.8.6), the partial buffer is **discarded** — its content cannot be completed from the new file.
  1. On `LogSource::Stop()` (4.7.2.ii), the partial buffer is **flushed** as a final synthetic line, unless the stop is concurrent with a rotation (in which case (i) wins).
- **Threading.** A single dedicated worker thread per `TailingFileSource` is sufficient (no TBB pipeline). The thread reads, parses, and posts batches to the GUI thread via the existing `QtStreamingLogSink` queued-connection pattern. The `TBB::global_control` block in the static-path pipeline is bypassed for the tailing path.
- **Batching and latency.** Coalesce arriving lines into batches of `~250 lines or 100 ms` (whichever comes first) before posting to the GUI, mirroring the existing pipeline's Stage C coalescing (`pipeline_detail::kStreamFlushLines = 1000`, `kStreamFlushInterval = 50 ms`) — the smaller numbers chosen here trade a little more cross-thread overhead for tighter G1 latency. With the 250 ms poll-only fallback worst case, total budget = 250 ms (poll) + 100 ms (coalesce) + ε (queued connection) ≈ 350 ms p95, well under G1's 500 ms.
- **`LogSource` shape.** Recommended sketch (final API at the implementer's discretion). The source is a *byte / line producer*, not a parser orchestrator:
  ```cpp
  class LogSource {
  public:
      virtual ~LogSource() = default;

      /// Pulls available bytes since the last call. Returns the number
      /// of bytes written into @p buffer; 0 on a transient EOF for
      /// tailing sources, terminal EOF (returns 0 and `IsClosed()`
      /// flips true) for mapped sources or after `Stop()`.
      virtual size_t Read(std::span<char> buffer) = 0;

      /// Blocks the caller until at least one byte is available, the
      /// configured timeout elapses, or `Stop()` is called.
      virtual void WaitForBytes(std::chrono::milliseconds timeout) = 0;

      /// Unblocks any in-flight `Read` / `WaitForBytes` and causes
      /// subsequent calls to report terminal EOF. Safe from any thread.
      virtual void Stop() noexcept = 0;

      [[nodiscard]] virtual bool IsClosed() const noexcept = 0;

      [[nodiscard]] virtual std::string DisplayName() const = 0;

      /// Optional rotation-event callback. Invoked from the source's
      /// own thread when 4.8.6 fires.
      virtual void SetRotationCallback(std::function<void()> /*cb*/) {}
  };
  ```
  with `MappedFileSource::Read` returning the next slice of the mmap and `TailingFileSource::Read` returning bytes appended to the file since its last read. `LogSource::Stop()` is **distinct from** `ParserOptions::stopToken` (4.7.2.i): the source's `Stop` releases I/O so the parser's hot loop can observe the parser stop token at the next batch boundary. Both must be wired on Stop / model teardown.
- **`LogFile` lifetime invariant.** The static path's `string_view`-into-mmap optimisation depends on `LogFile` outliving every `LogLine` referencing it. Stream Mode does **not** rely on this: `StreamLogLine` owns its values directly (4.9.7), so the existing `LogModel::Clear()` / `~LogModel()` `waitForFinished()` join on the streaming watcher remains the right cleanup pattern. The `mLogTable` invariant during streaming changes only insofar as the row vector now holds a mix of line types if the implementer picks a `std::variant` representation; the lifetime of the source it borrows from is owned by `LogModel` (4.10.2).
- **Cross-platform file identity.** For rotation detection: POSIX `stat::st_ino`; Windows `GetFileInformationByHandle::nFileIndexLow/High`. Wrap behind a tiny helper. Falling back to "size shrunk" detection covers copytruncate; rename-and-create on filesystems where the file index isn't stable (FAT/exFAT, some network shares) is an undetected case and a documented limitation (4.8.6).
- **Testing the tail.** Existing `test/lib/` Catch2 tests cover the static path. New tests must cover, at minimum: pre-fill of last N lines on a small file; pre-fill on a file shorter than N; growth detection across many small writes; rotation via rename-and-create; rotation via copytruncate; rotation while paused; FIFO eviction; cancellation via the stop token; the partial-line-on-rotation buffer discard. Most can be exercised with a synthetic temp file the test process writes to, plus a small abstraction that lets the test inject "fake clock" events into the polling fallback.
- **Qt smoke tests.** `test/app/` should gain at least one stream-mode test that opens a temp file, has the test process append lines, and asserts the rows arrive in the model. Use the existing offscreen-Qt pattern.
- **CI.** No new CI matrix is needed; the streaming tests must pass on Linux, Windows, and macOS using the existing `release` workflow preset. Polling-fallback timing must be tolerant of slow CI runners.

## 8. Success Metrics

1. **End-to-end latency.** From a producer's `write()` + `fflush()` to the row appearing in the GUI: median ≤ 250 ms, p95 ≤ 500 ms, on a developer-class machine for a producer writing ≤ 10 000 lines/s. The latency budget chain (per §7 *Batching and latency*) is poll/event ≤ 250 ms + coalesce ≤ 100 ms + queued-connection ε ≈ 350 ms p95 worst-case. Measured via a new Catch2 benchmark (`[stream_latency]`) that writes synthetic lines on one thread and measures the time until they arrive at a custom test sink wired up to the same path.
1. **No regression on the static path.** The existing `[large]` / `[wide]` / `[allocations]` / `[cancellation]` benchmarks stay within the project's standing `±3 %` bar, and the `[allocations]` `string_view` fast-path fraction stays ≥ 99 %.
1. **Rotation tolerance.** A unit test simulates each of the four rotation patterns (4.8) and asserts that no line written after rotation is dropped, and no line displayed before rotation is removed (other than via FIFO eviction).
1. **Memory bound.** Under the default `N = 10 000`, the resident set of the application is bounded across a 24 h tail of a 1 000 lines/s producer (i.e. it does not grow unboundedly). Verified by a manual long-running soak prior to release.
1. **No crashes / leaks under cancellation.** Opening, pausing, and stopping a stream 100 times in a row leaks no heap memory under ASan / leak sanitiser, and produces no use-after-free under a tail of a file being concurrently rotated. A nightly stress test exercises this.
1. **Stop latency.** From the GUI's **Stop** action to the worker thread joining: ≤ 500 ms (4.7.2). A unit test measures the elapsed time across `LogSource::Stop()` → `stopToken.request_stop()` → `waitForFinished()` against a worker that is parked in `WaitForBytes` (the typical case on an idle producer), and against a worker actively decoding a 100 KiB-buffered batch.
1. **User-facing acceptance.** A developer (project maintainer) can productively use Stream Mode to debug a local service for ≥ 30 minutes without restarting the viewer.

## 9. Open Questions

- **OQ-1 — Existing-session prompt.** When the user invokes **File → Open Log Stream…** while a static or stream session is open, do we silently close the existing session (consistent with today's static `Open…`, which calls `mModel->Clear()` unconditionally in `MainWindow::OpenFilesWithParser`) or prompt for confirmation? Recommendation: silent close (matches existing behaviour). Confirm with the maintainer.
- **OQ-2 — Pause + retention interaction.** *Resolved in this PRD revision (4.2.2.iv–v):* total memory is bounded by `N`. When the cap would be exceeded during a pause, the *paused buffer* drops oldest entries; visible rows are preserved. Qt event-queue accumulation is avoided by routing batches through a worker-side pause flag in the bridging sink (rather than per-batch `QueuedConnection` lambdas). The alternative ("let memory grow until Resume") was rejected on the grounds that an indefinitely-paused viewer attached to a noisy producer would OOM.
- **OQ-3 — Sort behaviour during streaming.** When the user has applied a sort and new rows arrive, the visible row order updates; this is the existing behaviour for static streaming opens. `QSortFilterProxyModel` re-sorts on every `rowsInserted`, which at the 10 000-line cap is an O(log N) insertion per row — fine within budget. Confirm that no special "freeze sort while streaming" affordance is required.
- **OQ-4 — Where do streaming controls live?** Toolbar above the table is recommended. Alternative: status-bar inline buttons (smaller footprint but less discoverable). Confirm the toolbar placement.
- **OQ-5 — `LogSource` exact shape.** *Refined in this PRD revision (4.9.1–4 / §7):* `LogSource` is a byte / line producer that does **not** know about `StreamingLogSink` or `ParserOptions`; the parser pulls from it. Final method names settle during implementation but the responsibility split (source = bytes + rotation; parser = parse; sink = GUI / buffering) is fixed.
- **OQ-6 — Auto-promote `t` / `time` / `timestamp` on first batch.** The existing static path applies the heuristic on the first batch; this is preserved for Stream Mode. Confirm no new heuristic is desired (e.g. a "look at the first line of the tail and infer columns" pre-flight). Note that with `TailingFileSource`, "the first batch" means the first batch that pre-fill produces — typically representative because pre-fill drains the last `N` complete lines on disk before tailing starts.
- **OQ-7 — Watcher choice.** `efsw` is recommended; `QFileSystemWatcher` is a fallback. The implementer may evaluate at the start of work and propose a final pick. If `QFileSystemWatcher` is chosen, code against its known limits (Linux `inotify` watch limit, no native rename detection on the file itself — only on the parent directory; macOS `FSEvents` directory-only granularity).
- **OQ-8 — Streaming-mode keyboard shortcuts.** The originally suggested bindings (`Ctrl+Shift+O` for Open Log Stream, `Ctrl+Space` for Pause/Resume, `Ctrl+End` for Follow tail, `Ctrl+.` for Stop) collide with widely-used existing semantics: `Ctrl+Space` is a near-universal completion shortcut, `Ctrl+End` is `QAbstractItemView`'s built-in scroll-to-bottom, and `Ctrl+.` is macOS's inline-toggle (and also clashes with `Cmd+.` "cancel current action"). The implementer should pick non-conflicting bindings (e.g. `Ctrl+Shift+O`, `Ctrl+Shift+P`, `Ctrl+Shift+F`, `Ctrl+Shift+S`) and document them in the View / Stream menu.
- **OQ-9 — `StreamLineReference` ownership model.** This PRD prescribes that streamed lines own their raw bytes for the `CopyLine` role (4.9.7). An alternative — sharing the raw bytes via reference-counted byte chunks owned by `TailingFileSource` — is cheaper RAM-wise (one allocation per chunk instead of per line) but considerably more complex (lifetime spans rotation; the chunk must outlive every line that references it, which interacts with FIFO eviction in non-obvious ways). Recommendation: per-line `std::string` ownership for the first cut. Revisit only if soak tests show the per-line raw-bytes term dominates the resident set.
- **OQ-10 — Preferences persistence semantics.** The new "Stream retention (lines)" preference adds a non-appearance setting to `PreferencesEditor`. Today's editor commits or reverts via `AppearanceControl::SaveConfiguration` / `LoadConfiguration` (Ok/Cancel buttons). Confirm whether the retention field should follow the same Ok/Cancel transactional pattern (and `AppearanceControl` widens to cover it), or whether retention should commit immediately on edit independent of the appearance buttons. Recommendation: same Ok/Cancel pattern, widen `AppearanceControl` (or rename it) to cover the new setting.
