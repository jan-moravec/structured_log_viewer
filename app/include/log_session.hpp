#pragma once

#include "log_session_commands.hpp"
#include "log_session_presentation.hpp"

#include <loglib/filter_expression.hpp>
#include <loglib/internal/decompressing_byte_source.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/stop_token.hpp>

#include <QAtomicInteger>
#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <memory>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class AnchorManager;
class HighlightRuleSet;
class LogFilterModel;
class LogModel;
class RegexTemplateRegistry;
class RowOrderProxyModel;
class SessionHistoryManager;
class ThemeControl;

/// Non-visual owner of one session's data models, source lifecycle,
/// filters, workers, persistence identity, and safe teardown.
///
/// Task 2.1 lands the model quintet (`AnchorManager`,
/// `HighlightRuleSet`, `LogModel`, `RowOrderProxyModel`,
/// `LogFilterModel`) inside `LogSession` in the lifetime order the
/// current `MainWindow` constructor uses. Later Phase 2 subtasks
/// bring in the migration listed in
/// `tasks/architecture-inventory.md`:
///
/// - Runtime filter leaves + sort + column + anchor + highlight +
///   dirty state (task 2.4).
/// - Source descriptor + session mode + counters + queues + guards
///   (task 2.5).
/// - Rotation expansion / undo / launch override / static-prefix
///   live-tail handoff (task 2.7).
/// - Decompression / export / bundle-export lifecycles (2.8–2.9).
/// - Live-tail / stdin / network producer lifecycles (2.10).
/// - Find query + parse-error state (2.11).
/// - Configuration mirror + autosave identity + shutdown ordering
///   (2.12–2.14).
///
/// The class remains a `QObject` (not `QMainWindow`) so tests can
/// exercise it without instantiating menus, docks, toolbars, or the
/// window shell (PRD §2.1 objective 3).
class LogSession : public QObject, public LogSessionCommands
{
    Q_OBJECT

public:
    /// Streaming session kind; gates UI variants and drives the
    /// close/autosave gates. The old `MainWindow::SessionMode`
    /// mapped 1:1 to these values and is aliased to this enum via
    /// `using MainWindow::SessionMode = LogSession::SessionMode;`
    /// so migrated call sites keep the short qualified name.
    enum class Mode : int
    {
        Idle,
        Static,
        LiveTail,
    };

    /// Non-owning service handles are passed by pointer so tests can
    /// pass `nullptr` for services that don't apply.
    explicit LogSession(
        ThemeControl *theme = nullptr,
        SessionHistoryManager *historyManager = nullptr,
        RegexTemplateRegistry *regexTemplateRegistry = nullptr,
        QObject *parent = nullptr
    );
    ~LogSession() override;

    LogSession(const LogSession &) = delete;
    LogSession &operator=(const LogSession &) = delete;
    LogSession(LogSession &&) = delete;
    LogSession &operator=(LogSession &&) = delete;

    // -----------------------------------------------------------------
    // LogSessionCommands (Phase 2 fills bodies).
    // -----------------------------------------------------------------

    void RequestNewSession() override;
    void RequestOpenFiles(const QStringList &files, OpenMode mode) override;
    void RequestOpenLogStream(const QString &filePath) override;
    void RequestAutoSaveSnapshot(bool publishOpenWindow) override;
    [[nodiscard]] std::uint32_t PreCheckClose() const override;
    [[nodiscard]] SessionCloseResult RequestClose() override;

    // -----------------------------------------------------------------
    // Presentation snapshot the shell projects into title / status /
    // tab labels. Phase 2 populates the fields as state moves in.
    // -----------------------------------------------------------------

    [[nodiscard]] SessionPresentationSnapshot PresentationSnapshot() const;

    // -----------------------------------------------------------------
    // Service accessors (non-owning). Docks read these through a
    // `SessionBindContext` during the bind step; this API stays for
    // migration + tests that construct a session directly.
    //
    // Definitions live in the .cpp so this header stays free of the
    // service headers.
    // -----------------------------------------------------------------

    [[nodiscard]] ThemeControl *Theme() const noexcept;
    [[nodiscard]] SessionHistoryManager *HistoryManager() const noexcept;
    [[nodiscard]] RegexTemplateRegistry *RegexTemplates() const noexcept;

    // -----------------------------------------------------------------
    // Owned models (task 2.1). Non-null for the lifetime of the
    // session. Definitions live in the .cpp so the header does not
    // need to pull in the model definitions.
    //
    // Lifetime order (construction, so also reverse-destruction):
    //   1. `AnchorManager`   — destroyed last; `LogModel` reads it.
    //   2. `HighlightRuleSet` — destroyed after `LogModel`; the model
    //      holds a non-owning pointer used by its paint cascade.
    //   3. `LogModel`         — sinks streaming batches / owns `LogTable`.
    //   4. `RowOrderProxyModel` — mirrors indices for newest-first.
    //   5. `LogFilterModel`   — filter + column-sort proxy.
    // -----------------------------------------------------------------

    [[nodiscard]] AnchorManager *Anchors() const noexcept;
    [[nodiscard]] HighlightRuleSet *Highlights() const noexcept;
    [[nodiscard]] LogModel *Model() const noexcept;
    [[nodiscard]] RowOrderProxyModel *RowOrderProxy() const noexcept;
    [[nodiscard]] LogFilterModel *FilterProxy() const noexcept;

    // -----------------------------------------------------------------
    // Dirty-state transitions (task 2.4). "Filters dirty" is the
    // legacy name for the session-modified marker that drives the
    // `[*]` in the window title (via Qt's `setWindowModified`) and
    // the "save configuration" prompt on close. It flips true on any
    // configuration-affecting mutation and false after a successful
    // save or on completion of a configuration load.
    //
    // `IsLoadingConfiguration()` is the re-entrancy gate load paths
    // use to coalesce per-filter mutations into a single dirty-state
    // update on scope exit; while it is `true`, `MarkFiltersDirty()`
    // becomes a no-op.
    // -----------------------------------------------------------------

    [[nodiscard]] bool IsFiltersDirty() const noexcept
    {
        return mFiltersDirty;
    }

    [[nodiscard]] bool IsLoadingConfiguration() const noexcept
    {
        return mLoadingConfiguration;
    }

    /// Set the modified marker to `true` and emit
    /// `filtersDirtyChanged(true)` (plus `presentationChanged()`,
    /// because `dirty.filtersDirty` and `confirmBeforeClose` on
    /// the snapshot both flip) iff the marker actually
    /// transitioned. Suppressed entirely while
    /// `IsLoadingConfiguration()` returns `true` so a bulk load
    /// path does not race its own intermediate mutations into an
    /// observable dirty state. On scope exit the load path calls
    /// `ClearFiltersDirty()` to reset the marker to `false`, so
    /// the observable behaviour is "suppress-and-reset", not
    /// coalesce-into-one-transition (review finding #14 doc fix).
    void MarkFiltersDirty();

    /// Set the modified marker to `false` unconditionally and emit
    /// `filtersDirtyChanged(false)` (plus `presentationChanged()`)
    /// iff the marker actually transitioned. Used after a
    /// successful configuration save or on scope exit of a
    /// configuration load.
    void ClearFiltersDirty();

    /// Toggle the loading-configuration gate. Emits nothing on its
    /// own. Callers wrap the load body in a `qScopeGuard` that
    /// pairs `SetLoadingConfiguration(true)` at entry with
    /// `SetLoadingConfiguration(false)` + `ClearFiltersDirty()` at
    /// scope exit. The gate suppresses any `MarkFiltersDirty()`
    /// invoked mid-load; the exit clear then resets the marker to
    /// its pre-load value (typically `false` for a fresh session).
    void SetLoadingConfiguration(bool loading) noexcept;

    // -----------------------------------------------------------------
    // Deferred sort (task 2.4). A loaded session's sort is latched
    // here after `LoadConfiguration` / `RestoreLastSession` and
    // consumed by `OnStreamingFinished` (or an early-return
    // streaming path). The proxy's `sortByColumn(-1, …)` reset that
    // brackets a `Reset()` would otherwise clobber the loaded sort
    // before streaming had a chance to reach the finish point.
    //
    // Behavioural pin:
    // `TestRestoreLastSessionDefersSortUntilStreamingFinishes` — a
    // 1 GB restore under an active sort falls into the O(N²)
    // per-row insert path in `LogFilterModel::OnSourceRowsInserted`
    // and "never finishes"; the latch keeps sorting off until the
    // rows are all in.
    //
    // `MirrorSessionStateToConfiguration` reads this flag so an
    // auto-save mid-stream preserves the loaded sort instead of
    // writing the proxy's transient `-1`.
    // -----------------------------------------------------------------

    [[nodiscard]] bool HasPendingApplySortFromConfig() const noexcept
    {
        return mPendingApplySortFromConfig;
    }

    void SetPendingApplySortFromConfig(bool pending) noexcept;

    // -----------------------------------------------------------------
    // Simple-mode runtime filter leaves (task 2.4). The map is
    // keyed by the filter's UUID; each entry mirrors one top-level
    // `Leaf` child of `LogConfiguration::expression`.
    // `SimpleLeafOrder()` preserves display order because unordered
    // map iteration is not stable; the pair is invariant — every
    // mutation site updates both.
    //
    // Advanced-only sub-trees (Or / Not roots, or mixed shapes) live
    // on `LogConfiguration::expression`, not here; see
    // `RebuildFilterExpressionFromSimpleLeaves` for the reconciler.
    //
    // The mutating accessors keep the existing MainWindow methods
    // (`AddLogFilter`, `RemoveLogFilter`, `ResetSimpleFilterState`,
    // …) buildable during the incremental extraction; later Phase 2
    // subtasks move those methods themselves into `LogSession` and
    // drop the mutating accessors in favour of a narrow command
    // API.
    // -----------------------------------------------------------------

    [[nodiscard]] const std::unordered_map<std::string, loglib::LeafRule> &SimpleLeaves() const noexcept
    {
        return mSimpleLeaves;
    }

    [[nodiscard]] std::unordered_map<std::string, loglib::LeafRule> &MutableSimpleLeaves() noexcept
    {
        return mSimpleLeaves;
    }

    [[nodiscard]] const std::vector<std::string> &SimpleLeafOrder() const noexcept
    {
        return mSimpleLeafOrder;
    }

    [[nodiscard]] std::vector<std::string> &MutableSimpleLeafOrder() noexcept
    {
        return mSimpleLeafOrder;
    }

    /// Clear the simple-mode filter state. Used by session-switch
    /// paths and by `LoadConfiguration` before it re-hydrates the
    /// map from the loaded expression.
    void ResetSimpleFilterState() noexcept;

    /// Recompose `LogConfiguration::expression` on the owned model
    /// from the current simple-mode leaves (`SimpleLeafOrder()` /
    /// `SimpleLeaves()`) merged with any Advanced-mode structure
    /// already carried on the expression. The result is a top-level
    /// `And` whose children are, in order:
    ///
    ///   1. One `Leaf` per entry in `SimpleLeafOrder()` (missing
    ///      ids from a stale order vector are skipped).
    ///   2. Every non-`Leaf` child of the existing root `And`, so
    ///      Advanced-mode `Or`/`Not` subtrees survive a simple-mode
    ///      edit that happens between load and save.
    ///   3. An existing root `Or`/`Not` preserved wholesale as a
    ///      single non-`Leaf` child.
    ///   4. An existing bare-`Leaf` root preserved as a single
    ///      child, unless the same rule is already reflected in
    ///      `SimpleLeaves()`.
    ///
    /// This is the shared reconciler used by every simple-mode
    /// filter mutation and by `RebuildFiltersFromConfiguration` on
    /// the load path. It never mutates `SimpleLeaves()` /
    /// `SimpleLeafOrder()`; it only rewrites the model expression.
    void RebuildFilterExpressionFromSimpleLeaves();

    /// Mirror the live proxy sort column/order into
    /// `LogConfiguration::sort` on the owned model so autosave and
    /// `SaveSession` write what the user sees. When
    /// `HasPendingApplySortFromConfig()` is true and the proxy is
    /// still unsorted (`SortColumn() < 0`), the configuration's
    /// existing sort is preserved because the live `-1` is transient
    /// — the deferred-sort latch is still waiting to reapply the
    /// loaded sort on `OnStreamingFinished`.
    void MirrorSortToConfiguration();

    /// Mirror the current anchor entries into
    /// `LogConfiguration::anchors` on the owned model. Both autosave
    /// and manual `SaveSession` rely on this so the persisted
    /// snapshot round-trips the anchor set.
    void MirrorAnchorsToConfiguration();

    // -----------------------------------------------------------------
    // Session mode (task 2.5). `SessionMode() == Static` covers a
    // finished static parse or an in-progress one; `LiveTail` marks
    // an in-flight live tail (rotation-aware). `Idle` is the null
    // state — no open source, no worker running.
    //
    // `LastTerminalMode()` mirrors the transition-to-`Idle` step
    // that `streamingFinished` performs: `SetMode(Idle)` snapshots
    // the previous non-`Idle` value here so a `closeEvent` firing
    // after a live tail already finished still sees `LiveTail` and
    // routes through the correct auto-save gate.
    // -----------------------------------------------------------------

    [[nodiscard]] Mode SessionMode() const noexcept
    {
        return mMode;
    }

    [[nodiscard]] Mode LastTerminalMode() const noexcept
    {
        return mLastTerminalMode;
    }

    [[nodiscard]] bool IsSessionActive() const noexcept
    {
        return mMode != Mode::Idle;
    }

    [[nodiscard]] bool IsLiveTailSession() const noexcept
    {
        return mMode == Mode::LiveTail;
    }

    /// Assign the live session mode. Transitions into `Idle` also
    /// latch `LastTerminalMode()` to the previous mode iff that
    /// previous mode was non-`Idle`, so the auto-save gate can
    /// distinguish "closed after live tail" from "never opened".
    /// Emits `presentationChanged()` on every write.
    void SetMode(Mode mode);

    /// Test / bulk-reset entry point used by close/session-switch
    /// paths that need to clear both the live and mirrored fields
    /// in one step. Emits `presentationChanged()` exactly once.
    void ResetMode();

    // -----------------------------------------------------------------
    // Enum-column rebuild re-entrancy latch (task 2.4). Set true
    // while `enumColumnsChanged` is repairing filter leaves whose
    // resolved column had its enum values change; a nested
    // `enumColumnsChanged` fired from the rewrite itself would
    // otherwise observe half-rewritten state. Every call site pairs
    // the setter with a `qScopeGuard` so an exception during the
    // rebuild still clears the flag.
    // -----------------------------------------------------------------

    [[nodiscard]] bool IsApplyingEnumRebuild() const noexcept
    {
        return mApplyingEnumRebuild;
    }

    void SetApplyingEnumRebuild(bool applying) noexcept;

    // -----------------------------------------------------------------
    // Session-switch guard (task 2.5). Set true while a destructive
    // open path is running `Reset()`; every `streamingFinished`
    // handler inspects this so the synchronous `Cancelled` emit that
    // `Reset()` fires does not re-enter the finished-session
    // post-processing (auto-save, presentation refresh, …) against
    // the outgoing session. The RAII helper
    // `MainWindow::SessionSwitchScope` still owns the on/off pairing
    // so early-return paths cannot forget the reset.
    // -----------------------------------------------------------------

    [[nodiscard]] bool IsSessionSwitchInProgress() const noexcept
    {
        return mSessionSwitchInProgress;
    }

    void SetSessionSwitchInProgress(bool inProgress) noexcept;

    // -----------------------------------------------------------------
    // Streaming progress counters (task 2.5). Session-local scalars
    // consumed by the status bar / parse-errors dock:
    //
    //   * `StreamingLineCount()`     — retained rows in the source
    //     model. Reset per file at streaming start.
    //   * `StreamingErrorCount()`    — retained parse errors.
    //   * `StreamingErrorsCut()`     — high-water mark into
    //     `Model()->StreamingErrors()` consumed by the last per-file
    //     batch. Multi-file static opens accumulate every file's
    //     errors in a single vector on the model; this watermark
    //     lets us peel off only the errors produced by the file that
    //     just finished.
    //   * `FirstStreamingBatchSeen()` — gates the one-shot column
    //     auto-resize on the first non-empty live-tail batch.
    //   * `IsSourceWaiting()`        — `SourceStatus::Waiting` latch;
    //     drives the "waiting for input…" status line and skips
    //     `— finished` toasts.
    //   * `StreamingFileName()`      — user-visible display label
    //     for the file currently being streamed (or the primary of
    //     a multi-source open). Cleared alongside the counters.
    //
    // `ResetStreamingCountersAndFileName()` bundles the six-field
    // clear used on session-switch / retention-fail paths.
    // `ResetStreamingProgress()` covers the per-file start pattern
    // (`mStreamingLineCount = mStreamingErrorCount = 0;
    // mFirstStreamingBatchSeen = false;`).
    // -----------------------------------------------------------------

    [[nodiscard]] qsizetype StreamingLineCount() const noexcept
    {
        return mStreamingLineCount;
    }

    void SetStreamingLineCount(qsizetype count) noexcept;

    [[nodiscard]] qsizetype StreamingErrorCount() const noexcept
    {
        return mStreamingErrorCount;
    }

    /// Emits `presentationChanged()` on a real change (the
    /// snapshot's `errorCount` mirrors this scalar).
    void SetStreamingErrorCount(qsizetype count);

    [[nodiscard]] std::size_t StreamingErrorsCut() const noexcept
    {
        return mStreamingErrorsCut;
    }

    /// Emits `presentationChanged()` on a real change (the
    /// snapshot's `droppedErrors` mirrors this scalar).
    void SetStreamingErrorsCut(std::size_t cut);

    [[nodiscard]] bool FirstStreamingBatchSeen() const noexcept
    {
        return mFirstStreamingBatchSeen;
    }

    /// Emits `presentationChanged()` on a real change (flips the
    /// `Parsing` op bit that gates the tab strip's static-loading
    /// spinner).
    void SetFirstStreamingBatchSeen(bool seen);

    [[nodiscard]] bool IsSourceWaiting() const noexcept
    {
        return mSourceWaiting;
    }

    /// Emits `presentationChanged()` on a real change (flips the
    /// `SourceWaiting` op bit and, for a live-tail session, the
    /// derived `Ingesting` bit as well).
    void SetSourceWaiting(bool waiting);

    /// True while the "\u2014 rotated" status-bar flash is armed
    /// for this session. Per-session (not per-window) so a
    /// multi-tab window never projects one tab's flash onto
    /// another (post-review-4 finding #4): switching to a sibling
    /// tab that isn't flashing hides the label immediately;
    /// switching back to a still-flashing tab restores it.
    ///
    /// Set true by `TriggerRotationFlash()`; cleared automatically
    /// ~3 s later via a session-owned `QTimer::singleShot(this,
    /// ...)` whose callback dies with the session and therefore
    /// cannot fire into a torn-down shell.
    [[nodiscard]] bool IsRotationFlashActive() const noexcept
    {
        return mRotationFlashActive;
    }

    /// Start (or refresh) the rotation-flash window for this
    /// session. Sets `mRotationFlashActive = true` and emits
    /// `rotationFlashChanged(true)` on the rising edge; schedules
    /// (or re-schedules) a `QTimer::singleShot` receiver-bound to
    /// `this` (session) so a session teardown safely cancels the
    /// pending clear.
    ///
    /// Repeated calls within an active flash window extend the
    /// clear deadline by another full 3 s -- matching the
    /// pre-migration shell behaviour where every `rotationDetected`
    /// emit reset the countdown. The Nth call after the last real
    /// change still fires `rotationFlashChanged(true)` only once
    /// (the boolean itself does not diff -> re-emit); consumers
    /// that need the edge count should subscribe to the underlying
    /// `LogModel::rotationDetected` instead.
    void TriggerRotationFlash();

    /// Duration of the rotation-flash window. Exposed as a public
    /// constant so tests can drive a shorter waiter without
    /// relying on the wall-clock timer.
    static constexpr int ROTATION_FLASH_DURATION_MS = 3000;

    [[nodiscard]] const QString &StreamingFileName() const noexcept
    {
        return mStreamingFileName;
    }

    void SetStreamingFileName(QString fileName);
    void ClearStreamingFileName();

    // -----------------------------------------------------------------
    // Current source descriptor (task 2.5). Matches what the model
    // currently holds: file path for `File`, producer name for
    // `NetworkStream`, sentinel for `Stdin`. Set on open / load;
    // survives `Success` / `Cancelled` streaming finish (the rows
    // are still there); cleared on `Failed` or the next open's
    // `Reset()`. Mirrored into `LogConfiguration::source` before a
    // `SaveScope::Full` save via
    // `MainWindow::MirrorSessionStateToConfiguration`.
    //
    // `MutableCurrentSource()` returns the underlying optional so
    // sites that pin fields on the descriptor
    // (`Source::followRotationSiblings`, `AppendLocator(...)`) or
    // reset it can do so in place without a copy round-trip. The
    // const overload guards read-only sites.
    //
    // IMPORTANT: mutation through `MutableCurrentSource()` does
    // NOT fan `presentationChanged` -- the accessor cannot see
    // whether the caller intends a snapshot-affecting change. Two
    // options for callers that need the fan:
    //
    //   1. Prefer `SetCurrentSource(...)` / `ResetCurrentSource()`
    //      for whole-value writes and clears; both emit on a real
    //      change.
    //   2. Wrap in-place field mutation in `MutateCurrentSource(fn)`
    //      below, which runs the lambda and fans
    //      `presentationChanged` on scope exit unconditionally
    //      (the caller opted in by choosing the helper).
    //   3. Call `NotifyPresentationChanged()` explicitly after a
    //      batch of raw accessor edits when the whole-value or
    //      helper form is impractical.
    //
    // Callers that mutate raw and skip the fan are responsible for
    // any stale tab-strip / window-title state; the shell tolerates
    // this today because no Phase 2 consumer subscribes to
    // `presentationChanged`. Phase 3 subscribers (the view) will
    // rely on the fan being complete.
    // -----------------------------------------------------------------

    [[nodiscard]] const std::optional<loglib::LogConfiguration::Source> &CurrentSource() const noexcept
    {
        return mCurrentSource;
    }

    [[nodiscard]] std::optional<loglib::LogConfiguration::Source> &MutableCurrentSource() noexcept
    {
        return mCurrentSource;
    }

    void SetCurrentSource(std::optional<loglib::LogConfiguration::Source> source);
    void ResetCurrentSource();

    /// Run `fn` against the mutable source descriptor and fan
    /// `presentationChanged` on scope exit, regardless of what
    /// the callback actually changed. Preferred over raw
    /// `MutableCurrentSource()` for any edit that could affect the
    /// snapshot (source-mode projection, restorability, labels).
    /// The unconditional fan keeps the helper cheap to reason
    /// about; if a hot-path caller ever needs a diff-guarded fan,
    /// use the explicit `SetCurrentSource(...)` form which does
    /// its own boundary check.
    template <typename Fn> void MutateCurrentSource(Fn &&fn)
    {
        std::forward<Fn>(fn)(mCurrentSource);
        emit presentationChanged();
    }

    /// Explicit fan trigger for cases where the caller edited raw
    /// via `MutableCurrentSource()` (or one of the other raw
    /// accessors below) and needs the presentation subscribers to
    /// refresh. Cheap and idempotent from the session's side;
    /// downstream slots handle any coalescing.
    void NotifyPresentationChanged();

    // -----------------------------------------------------------------
    // Pending static-file queue + error buckets (task 2.5).
    //
    //   * `PendingOpenFiles()`   — FIFO queue of user-requested static
    //     files not yet handed to a parse worker. Populated by
    //     `StartStreamingOpenQueue`, drained one entry at a time by
    //     `StreamNextPendingFile`, and forcibly cleared by every
    //     destructive session-switch or cancel path so a queued file
    //     never crosses sessions.
    //   * `PendingOpenErrors()`  — parse/open errors collected while
    //     draining the queue; presented as one modal batch under the
    //     `tr("Error Opening File")` title on `OnStreamingFinished`
    //     when the queue empties.
    //   * `PendingDecompressionErrors()` — decompression-specific
    //     errors from the same drain, kept separate so the batch can
    //     be labelled `tr("Error Decompressing File")`. User cancels
    //     surface as a status-bar toast instead of a modal.
    //
    // Convenience `ClearPendingOpenQueues()` matches the three-way
    // reset used by the session-switch and destructive-open seams.
    // -----------------------------------------------------------------

    [[nodiscard]] const QStringList &PendingOpenFiles() const noexcept
    {
        return mPendingOpenFiles;
    }

    [[nodiscard]] QStringList &MutablePendingOpenFiles() noexcept
    {
        return mPendingOpenFiles;
    }

    void SetPendingOpenFiles(QStringList files);
    void ClearPendingOpenFiles();

    [[nodiscard]] const std::vector<std::string> &PendingOpenErrors() const noexcept
    {
        return mPendingOpenErrors;
    }

    [[nodiscard]] std::vector<std::string> &MutablePendingOpenErrors() noexcept
    {
        return mPendingOpenErrors;
    }

    void ClearPendingOpenErrors() noexcept;

    [[nodiscard]] const std::vector<std::string> &PendingDecompressionErrors() const noexcept
    {
        return mPendingDecompressionErrors;
    }

    [[nodiscard]] std::vector<std::string> &MutablePendingDecompressionErrors() noexcept
    {
        return mPendingDecompressionErrors;
    }

    void ClearPendingDecompressionErrors() noexcept;

    /// Clear all three pending-open buckets in one call. Matches the
    /// `mPendingOpenFiles.clear(); mPendingOpenErrors.clear();
    /// mPendingDecompressionErrors.clear();` triplet the legacy
    /// `MainWindow` used at every destructive seam.
    void ClearPendingOpenQueues() noexcept;

    // -----------------------------------------------------------------
    // Session-owned parse-error log (task 5.4).
    //
    // The visible state on `ParseErrorsDock` (grouped error rows,
    // dropped count, first-batch latch) is authoritative here so a
    // phase-6 tab switch to a previously-bound session restores
    // every entry, the running counts, and the auto-raise latch
    // exactly. The dock's `Bind(SessionBindContext)` snapshots the
    // outgoing session's log and replays the incoming session's log;
    // its `Unbind()` snapshots and clears.
    //
    // `MutableParseErrorLog()` is the primary write path: the dock
    // moves its shadow store here on Unbind and reads from it on
    // Bind. The const overload guards read-only sites (e.g. tab-
    // indicator projection in phase 6 that never rebinds the dock).
    //
    // `ResetParseErrorLog()` clears every field including the
    // first-batch latch. Called from every destructive session-
    // switch seam on the shell where the pre-migration
    // `ParseErrorsDock::ResetSessionState()` fired.
    // -----------------------------------------------------------------

    [[nodiscard]] const SessionParseErrorLog &ParseErrorLog() const noexcept
    {
        return mParseErrorLog;
    }

    [[nodiscard]] SessionParseErrorLog &MutableParseErrorLog() noexcept
    {
        return mParseErrorLog;
    }

    void ResetParseErrorLog() noexcept;

    // -----------------------------------------------------------------
    // Session-owned Find query state (task 5.3). `FindDock` /
    // `FindRecordWidget` snapshot the visible query into this on
    // `Unbind()` and restore from this on `Bind()`. Match-count
    // state is not persisted -- the dock re-arms the debounce
    // on Bind and lets `MatchCountRequested` refresh it against
    // the current model.
    // -----------------------------------------------------------------

    [[nodiscard]] const SessionFindQueryState &FindQuery() const noexcept
    {
        return mFindQuery;
    }

    [[nodiscard]] SessionFindQueryState &MutableFindQuery() noexcept
    {
        return mFindQuery;
    }

    /// Reset the stored query to its default (empty text, both
    /// toggles off). Intentionally NOT called from the
    /// destructive session-switch seams on the shell: the user's
    /// current search string is UX-load-bearing across an Open
    /// File / New Session reload of the SAME session (matches
    /// pre-migration behaviour where the bar text survived).
    /// Wired for phase-6 tab-close and future "reset session
    /// state" menu actions.
    void ResetFindQuery() noexcept;

    // -----------------------------------------------------------------
    // Session-owned histogram-dock state (task 5.6). `HistogramDock`
    // saves the current pinned bucket-size rung here on `Unbind()`
    // and reapplies it on `Bind()`; a phase-6 tab switch back to a
    // previously-bound session therefore restores what the user
    // last chose without the auto-picker overriding it.
    // -----------------------------------------------------------------

    [[nodiscard]] const SessionHistogramState &HistogramState() const noexcept
    {
        return mHistogramState;
    }

    [[nodiscard]] SessionHistogramState &MutableHistogramState() noexcept
    {
        return mHistogramState;
    }

    /// Reset stored histogram state (drop the user-pinned bucket
    /// size). Not called from destructive session-switch seams:
    /// a pinned rung should survive an Open File / New Session
    /// on the SAME session. Wired for phase-6 tab-close.
    void ResetHistogramState() noexcept;

    // -----------------------------------------------------------------
    // Session-owned record-detail pin state (task 5.7).
    // `RecordDetailDock` saves the currently-pinned source row here
    // on `Unbind()` and reapplies it on `Bind()`; a phase-6 tab
    // switch back to a previously-bound session therefore restores
    // the record the user was looking at rather than defaulting
    // back to the "select a row" placeholder.
    // -----------------------------------------------------------------

    [[nodiscard]] const SessionRecordDetailPin &RecordDetailPin() const noexcept
    {
        return mRecordDetailPin;
    }

    [[nodiscard]] SessionRecordDetailPin &MutableRecordDetailPin() noexcept
    {
        return mRecordDetailPin;
    }

    /// Reset the stored pin (clear row, key, and `everPinned`).
    /// Not called from destructive session-switch seams: the
    /// `LogModel::Reset()` those seams already trigger fires
    /// `modelReset` -> `RecordDetailDock::Clear()`, which drops
    /// the persistent index; the session's stored pin then
    /// naturally reflects the cleared state on the next
    /// `SaveStateIntoBoundSession`. Wired for phase-6 tab-close
    /// and forced-reset menu actions.
    void ResetRecordDetailPin() noexcept;

    /// Session-owned anchors-dock selection state (origin-review
    /// finding M9). Persists the selected anchor's stable key
    /// (`AnchorManager::Key`) across a phase-6 tab switch so the
    /// user's cursor into the anchor list does not silently jump
    /// to whatever entry happened to sit first in the incoming
    /// session's tree.
    [[nodiscard]] const SessionAnchorsSelection &AnchorsSelection() const noexcept
    {
        return mAnchorsSelection;
    }

    [[nodiscard]] SessionAnchorsSelection &MutableAnchorsSelection() noexcept
    {
        return mAnchorsSelection;
    }

    // -----------------------------------------------------------------
    // Rotation-expansion / static-prefix-to-live-tail promotion state
    // (task 2.7). Session-local scalars consumed by the rotation
    // sibling expander and by the queue drain that promotes a
    // pending live-tail primary once the historical prefix finishes:
    //
    //   * `PendingLiveTailPrimary()`   — file path to tail after
    //     the queued historical prefix drains. Destructive session
    //     changes must clear it via `ClearPendingLiveTailPromotion`.
    //   * `PendingLiveTailRetention()` — retention cap saved across
    //     a historical-prefix load; restored on the promotion so the
    //     tail keeps the caller's retention preference.
    //   * `HasPendingLiveTailPromotion()` — non-empty primary OR
    //     non-zero retention; the OR is intentional so a promotion
    //     that only carries retention (rescue path) is still visible.
    //   * `DisableRotationHistoryOverride()` — per-window CLI opt-out
    //     that also latches for the duration of an undo so the
    //     restore does not re-expand the siblings. Cleared by the
    //     Settings toggle.
    //   * `LastRotationExpansionOriginalInputs()` — exact caller
    //     inputs restored by `UndoRotationExpansion`.
    //   * `LastRotationExpansionWasLiveTail()` — whether the last
    //     expansion started as a live tail; drives the reopen entry
    //     point (`OpenLogStreamFromPath` vs. static queue) in undo.
    // -----------------------------------------------------------------

    [[nodiscard]] const QString &PendingLiveTailPrimary() const noexcept
    {
        return mPendingLiveTailPrimary;
    }

    [[nodiscard]] std::size_t PendingLiveTailRetention() const noexcept
    {
        return mPendingLiveTailRetention;
    }

    [[nodiscard]] bool HasPendingLiveTailPromotion() const noexcept
    {
        return !mPendingLiveTailPrimary.isEmpty() || mPendingLiveTailRetention != 0;
    }

    void SetPendingLiveTailPromotion(QString primary, std::size_t retention);

    /// Clear both live-tail promotion fields together. Matches the
    /// `mPendingLiveTailPrimary.clear();
    /// mPendingLiveTailRetention = 0;` pair that every destructive
    /// session-switch path performed.
    void ClearPendingLiveTailPromotion() noexcept;

    [[nodiscard]] bool DisableRotationHistoryOverride() const noexcept
    {
        return mDisableRotationHistoryOverride;
    }

    void SetDisableRotationHistoryOverride(bool disable) noexcept;

    [[nodiscard]] const QStringList &LastRotationExpansionOriginalInputs() const noexcept
    {
        return mLastRotationExpansionOriginalInputs;
    }

    [[nodiscard]] bool LastRotationExpansionWasLiveTail() const noexcept
    {
        return mLastRotationExpansionWasLiveTail;
    }

    /// Record the exact caller inputs for the most recent rotation
    /// expansion so `UndoRotationExpansion` can restore them; the
    /// `wasLiveTail` bit drives the reopen entry point.
    void SetLastRotationExpansion(QStringList originalInputs, bool wasLiveTail);

    /// Consume the pending live-tail promotion, returning the
    /// captured primary + retention as a pair and clearing the
    /// fields in one step. Used by `ContinueLiveTailAfterPrefix`
    /// so the promotion cannot fire twice against the same
    /// pending pair.
    [[nodiscard]] std::pair<QString, std::size_t> TakePendingLiveTailPromotion() noexcept;

    /// Clear the two `mLastRotationExpansion*` fields in one call.
    /// Matches the `mLastRotationExpansionOriginalInputs.clear();
    /// mLastRotationExpansionWasLiveTail = false;` pair the legacy
    /// `MainWindow` used at every destructive seam.
    void ClearRotationExpansionUndoState() noexcept;

    // -----------------------------------------------------------------
    // Auto-save persistence identity (task 2.12). Session-local
    // scalars that pin this session's owned recents entry:
    //
    //   * `AutoSaveUuid()`          — the uuid of the recents entry
    //     this session owns. Assigned after the first successful
    //     `WriteSnapshot` so subsequent saves rewrite the same JSON
    //     instead of appending one entry per save. Empty until the
    //     first save (or until `OpenRecentSession` / restore pins
    //     the caller-provided uuid).
    //   * `IsAutoSaveUuidPublished()` — true iff `AutoSaveUuid()` is
    //     currently in the process-shared `openWindowsAtQuit` set.
    //     Lets `DetachAutoSaveUuid` skip the cross-process
    //     `RemoveOpenWindowUuid` round-trip when nothing was
    //     published. Must stay in lockstep with `AddOpenWindowUuid`
    //     call sites (`RestoreLastSessionFromPath`,
    //     `OpenRecentSession`, `AutoSaveSessionSnapshot`).
    // -----------------------------------------------------------------

    [[nodiscard]] const QString &AutoSaveUuid() const noexcept
    {
        return mAutoSaveUuid;
    }

    [[nodiscard]] bool IsAutoSaveUuidPublished() const noexcept
    {
        return mAutoSaveUuidPublished;
    }

    void SetAutoSaveUuid(QString uuid);
    void SetAutoSaveUuidPublished(bool published) noexcept;

    /// Clear both the pinned uuid and its publish latch. Matches the
    /// `mAutoSaveUuid.clear(); mAutoSaveUuidPublished = false;` pair
    /// the legacy `MainWindow::DetachAutoSaveUuid` used after
    /// dropping the cross-process publish.
    void ClearAutoSaveUuid() noexcept;

    /// Drop this session's ownership of its recents entry. Pairs the
    /// cross-process `SessionHistoryManager::RemoveOpenWindowUuid`
    /// call with `ClearAutoSaveUuid()`; a no-op when no uuid is
    /// pinned. `MainWindow::DetachAutoSaveUuid` forwards here.
    void DetachAutoSaveUuid();

    /// The uuid to fan-restore for this session if the process were
    /// to quit now; empty when this window is not worth restoring.
    ///
    /// Gate list (all must hold):
    ///
    ///   1. `mAutoSaveUuid` is non-empty (the session has an
    ///      identity to fan-restore).
    ///   2. Source is one of:
    ///      * absent (columns-only restore -- the user explicitly
    ///        clicked a recents entry with no source of its own),
    ///      * `Kind::File` with a non-empty locator vector.
    ///      Stream sources (`Stdin`, `NetworkStream`) fail this
    ///      gate because they cannot be re-bound from a saved
    ///      locator; a `File` descriptor whose locator vector was
    ///      emptied by a partially-cancelled open also fails so
    ///      the loader is not asked to re-open nothing (review
    ///      finding #6).
    ///
    /// Deliberately does **not** null-check `mHistoryManager`.
    /// `mAutoSaveUuid` is only ever assigned via paths that
    /// already require a bound manager
    /// (`RestoreLastSessionFromPath`, `OpenRecentSession`,
    /// `AutoSaveSessionSnapshot`), so a session with a pinned
    /// uuid but no manager can only exist in a synthetic test
    /// fixture. Callers that need the stricter "will this actually
    /// persist?" answer should combine this with
    /// `ShouldAutoSaveAfterStreaming` or an explicit
    /// `HistoryManager() != nullptr` check.
    ///
    /// `MainWindow::RestorableActiveSessionUuid` forwards here.
    [[nodiscard]] QString RestorableSessionUuid() const noexcept;

    /// True iff a snapshot of this session should be persisted after
    /// the just-finished streaming completion. Gates:
    ///   * A `SessionHistoryManager` is bound.
    ///   * The session has a File source (network + stdin can't be
    ///     reopened from a saved locator).
    ///   * The just-finished mode is not `LiveTail` (a live-tail
    ///     session looks like a static File source on disk but
    ///     binds a tailing producer; reopening would silently
    ///     downgrade the user to a one-shot static load).
    ///
    /// `MainWindow::ShouldAutoSaveSession` forwards here.
    [[nodiscard]] bool ShouldAutoSaveAfterStreaming(Mode justFinishedMode) const noexcept;

    /// True iff the per-window CLI opt-out is off and the
    /// process-global `ui/autoDetectRotatedHistory` `QSettings` key
    /// is on. `MainWindow::ShouldAutoDetectRotationHistory` forwards
    /// here.
    [[nodiscard]] bool ShouldAutoDetectRotationHistory() const;

    /// True iff `ShouldAutoDetectRotationHistory()` is on **and**
    /// the current source (if any) still has
    /// `followRotationSiblings = true`. Matches
    /// `MainWindow::EffectiveAutoDetectRotationHistory`.
    [[nodiscard]] bool EffectiveAutoDetectRotationHistory() const;

    // -----------------------------------------------------------------
    // Live-tail wall-clock (task 2.10). Wraps a monotonic
    // ``QElapsedTimer`` re-armed at the start of each live-tail
    // open and consumed by the status-bar formatter to show
    // "N since start". Deliberately *not* cleared by
    // `StopLiveTailTicker`: the final status line after a stop
    // should still report how long the session ran.
    // -----------------------------------------------------------------

    [[nodiscard]] const QElapsedTimer &LiveTailElapsedTimer() const noexcept
    {
        return mLiveTailElapsedTimer;
    }

    /// Restart the monotonic clock so subsequent
    /// ``elapsed()`` reads report time since this call.
    void StartLiveTailElapsedTimer() noexcept
    {
        mLiveTailElapsedTimer.start();
    }

    // -----------------------------------------------------------------
    // Worker QFutureWatchers (task 2.8/2.9). Session-owned so the
    // watcher's lifetime naturally ends with the session it
    // observes: reaping the session detaches every future callout
    // and drops any pending ``finished`` slot invocation against a
    // dead shell.
    //
    // The lazy-allocation policy lives on the session itself
    // (review finding #13). Callers request the watcher via
    // ``EnsureDecompressionWatcher()`` / ``EnsureExportWatcher()``,
    // which allocates on first use and returns the cached
    // instance on subsequent calls. The shell wires the
    // ``finished`` signal to its slot with
    // ``Qt::UniqueConnection`` so re-invocation across successive
    // ``BeginAsync*`` runs never accumulates duplicate slot
    // callouts.
    //
    // The setter form used earlier was replaced because it was
    // public, untyped, and would have silently leaked the previous
    // watcher if a caller re-assigned without draining the old
    // instance first.
    // -----------------------------------------------------------------

    using DecompressionByteSourcePtr = std::shared_ptr<loglib::internal::DecompressingByteSource>;
    using DecompressionWatcher = QFutureWatcher<DecompressionByteSourcePtr>;
    using ExportWatcher = QFutureWatcher<void>;

    [[nodiscard]] DecompressionWatcher *DecompressionWatcherPtr() const noexcept
    {
        return mDecompressionWatcher;
    }

    /// Lazily allocate the decompression watcher on first call
    /// and return the cached instance on subsequent calls. The
    /// returned watcher is parented on ``*this`` so tab / session
    /// teardown reaps it automatically. Callers should wire the
    /// ``finished`` signal with ``Qt::UniqueConnection`` to stay
    /// safe against being called repeatedly (see
    /// ``MainWindow::BeginAsyncDecompression``).
    [[nodiscard]] DecompressionWatcher *EnsureDecompressionWatcher();

    [[nodiscard]] ExportWatcher *ExportWatcherPtr() const noexcept
    {
        return mExportWatcher;
    }

    /// Lazily allocate the export watcher. Same contract as
    /// ``EnsureDecompressionWatcher`` above.
    [[nodiscard]] ExportWatcher *EnsureExportWatcher();

    // -----------------------------------------------------------------
    // Navigation helpers over the session's model quintet
    // (task 2.11 groundwork). Pure computations on the owned
    // models; no shell state involved. ``MainWindow`` forwards to
    // these from its Go To Timestamp / Anchor navigation paths.
    // -----------------------------------------------------------------

    /// Locate the first source row whose timestamp in ``timeCol`` is
    /// ``>= targetMicros`` and that survives the active proxy
    /// chain. Returns -1 when no such row exists, when either
    /// argument is out of range, or when a model is missing (bare
    /// test fixtures).
    ///
    /// Fast path (no user sort + monotonic source): O(log N) binary
    /// search of the source rows plus at most O(N_visible) proxy
    /// walk for the "hidden by filter" case.
    ///
    /// Slow paths (non-monotonic source or user sort active):
    /// O(N_visible) linear scan of the outer proxy so the answer
    /// respects the user's chosen sort.
    [[nodiscard]] int FindFirstRowAtOrAfterTimestamp(int timeCol, std::int64_t targetMicros) const;

    /// Locate the source-row index of a column whose keys exactly
    /// match ``keys``. Returns -1 when the model is missing, when
    /// ``keys`` is empty, or when no column matches. Matches the
    /// legacy ``MainWindow::FindColumnIndexByKeys``.
    [[nodiscard]] int FindColumnIndexByKeys(const std::vector<std::string> &keys) const;

    // -----------------------------------------------------------------
    // Bundle embedded-configuration gate (task 2.8). Records the
    // session-bundle path whose embedded `LogConfiguration` may be
    // applied when the pending decompression finishes. Empty
    // disables the apply; replacing the path lets the latest open
    // request supersede an in-flight bundle open without racing.
    // `OnDecompressionFinished` matches the pending path against
    // `mDecompressionOriginalPath` before applying so a stale
    // finished slot cannot re-apply a superseded bundle.
    // -----------------------------------------------------------------

    [[nodiscard]] const QString &ApplyEmbeddedBundleConfigForPath() const noexcept
    {
        return mApplyEmbeddedBundleConfigForPath;
    }

    [[nodiscard]] bool ShouldApplyEmbeddedBundleConfig() const noexcept
    {
        return !mApplyEmbeddedBundleConfigForPath.isEmpty();
    }

    /// Emits `presentationChanged()` iff the "set / not-set"
    /// boolean value of the gate transitions (writing a different
    /// non-empty path over an existing one is silent because the
    /// snapshot only projects `Bundle` mode on presence, not on
    /// the exact path).
    void SetApplyEmbeddedBundleConfigForPath(QString bundlePath);
    /// Emits `presentationChanged()` on a real change.
    ///
    /// Not `noexcept` because the signal fan can throw through
    /// subscriber slots; matches every other snapshot-affecting
    /// mutator on this class (see `ResetStreamingProgress`
    /// docstring for the design rule).
    void ClearApplyEmbeddedBundleConfig();

    // -----------------------------------------------------------------
    // Decompression scalar state (task 2.8). The `QFutureWatcher` +
    // `StopSource` + progress-dialog wiring stays on `MainWindow`
    // until the whole cluster moves; the scalars below are session-
    // scoped identity/timing that the queue drain, cancel paths, and
    // completion toasts read across many sites:
    //
    //   * `IsDecompressionInFlight()` — set in
    //     `BeginAsyncDecompression`, cleared in
    //     `OnDecompressionFinished` / `CancelInFlightDecompression`.
    //     Guards the finished slot against a queued callout event
    //     already dispatched before `setFuture({})` cleared the
    //     watcher's queue.
    //   * `DecompressionOriginalPath()` — user-facing path being
    //     decompressed; kept alive so progress dialog + completion
    //     toast can name the file after the worker is gone.
    //   * `DecompressionCodecName()`   — pre-sniffed codec label the
    //     poll-timer lambda renders without touching the worker.
    //   * `DecompressionStartedAt()`   — wall-clock start of the
    //     current decompression, for the post-success toast.
    //
    // `ClearDecompressionScratchPaths()` matches the paired reset the
    // legacy `MainWindow` used at every decompression teardown site
    // (`mDecompressionOriginalPath.clear();
    //  mDecompressionCodecName.clear();`).
    // -----------------------------------------------------------------

    [[nodiscard]] bool IsDecompressionInFlight() const noexcept
    {
        return mDecompressionInFlight;
    }

    /// Emits `presentationChanged()` on a real change. Flips the
    /// `Decompressing` op bit, the `Compressed` source-mode
    /// projection (under Static without a bundle armed),
    /// `mutationsAllowed`, and `confirmBeforeClose`.
    ///
    /// Bumps `DecompressionGeneration()` on the false -> true
    /// rising edge so a poll timer that captured the previous
    /// generation can detect that a queued completion silently
    /// rearmed the state for the next queued file (review finding
    /// #4). Callers must call `SetDecompressionInFlight(true)`
    /// exactly once per operation begin -- do NOT bracket a single
    /// operation with matched true/false pairs.
    void SetDecompressionInFlight(bool inFlight);

    /// Monotonic counter that bumps on every decompression
    /// begin (false -> true transition of the in-flight flag).
    /// The shell's poll-timer / progress-strip callbacks capture
    /// the value returned at `Begin*` time and compare on each
    /// tick; a mismatch means a completion drained into a queued
    /// next open while this tick was scheduled, and the tick must
    /// return without writing stale progress into the successor.
    [[nodiscard]] std::uint64_t DecompressionGeneration() const noexcept
    {
        return mDecompressionGeneration;
    }

    [[nodiscard]] const QString &DecompressionOriginalPath() const noexcept
    {
        return mDecompressionOriginalPath;
    }

    void SetDecompressionOriginalPath(QString path);

    [[nodiscard]] const QString &DecompressionCodecName() const noexcept
    {
        return mDecompressionCodecName;
    }

    void SetDecompressionCodecName(QString codec);

    [[nodiscard]] std::chrono::steady_clock::time_point DecompressionStartedAt() const noexcept
    {
        return mDecompressionStartedAt;
    }

    void SetDecompressionStartedAt(std::chrono::steady_clock::time_point startedAt) noexcept;

    /// Clear the two decompression scratch paths in one call. Matches
    /// the `mDecompressionOriginalPath.clear();
    /// mDecompressionCodecName.clear();` pair every decompression
    /// teardown / cancel site performs.
    void ClearDecompressionScratchPaths() noexcept;

    // -----------------------------------------------------------------
    // Decompression / export cooperative-cancel sources (task
    // 2.8 / 2.9). Each `StopSource` is paired with the corresponding
    // `QFutureWatcher` on `MainWindow`; `QProgressDialog::canceled`
    // calls `request_stop()`, the worker polls `stop_requested()`
    // between batches. Refreshed per-open so a cancelled operation
    // never bleeds into the next one.
    //
    // The mutable accessors return the source by reference so
    // callers can `request_stop()` and `get_token()` in place, plus
    // reassign a fresh source with `= loglib::StopSource{}` at the
    // start of each new open.
    // -----------------------------------------------------------------

    [[nodiscard]] const loglib::StopSource &DecompressionStopSource() const noexcept
    {
        return mDecompressionStopSource;
    }

    [[nodiscard]] loglib::StopSource &MutableDecompressionStopSource() noexcept
    {
        return mDecompressionStopSource;
    }

    [[nodiscard]] const loglib::StopSource &ExportStopSource() const noexcept
    {
        return mExportStopSource;
    }

    [[nodiscard]] loglib::StopSource &MutableExportStopSource() noexcept
    {
        return mExportStopSource;
    }

    // -----------------------------------------------------------------
    // Decompression / export progress atomics (task 2.8 / 2.9). Each
    // worker publishes into the "written" counters and the total is
    // seeded up-front so the poll timer can render a percentage
    // without waiting for the first tick. Widened to `qint64` to
    // match Qt's atomic contract; the payload never exceeds
    // `size_t` in practice.
    //
    // The counters live on `LogSession` so future tabs each carry
    // their own progress state; the shell keeps the paired
    // `QTimer` because rendering is a view concern. Callers reach
    // the atomic by reference so worker lambdas can capture a raw
    // pointer that outlives them (the session lifetime is pinned
    // by `MainWindow::mSession`).
    // -----------------------------------------------------------------

    [[nodiscard]] const QAtomicInteger<qint64> &DecompressionBytesIn() const noexcept
    {
        return mDecompressionBytesIn;
    }

    [[nodiscard]] QAtomicInteger<qint64> &MutableDecompressionBytesIn() noexcept
    {
        return mDecompressionBytesIn;
    }

    [[nodiscard]] const QAtomicInteger<qint64> &DecompressionTotalBytesIn() const noexcept
    {
        return mDecompressionTotalBytesIn;
    }

    [[nodiscard]] QAtomicInteger<qint64> &MutableDecompressionTotalBytesIn() noexcept
    {
        return mDecompressionTotalBytesIn;
    }

    [[nodiscard]] const QAtomicInteger<qint64> &ExportRowsWritten() const noexcept
    {
        return mExportRowsWritten;
    }

    [[nodiscard]] QAtomicInteger<qint64> &MutableExportRowsWritten() noexcept
    {
        return mExportRowsWritten;
    }

    [[nodiscard]] const QAtomicInteger<qint64> &ExportRowsTotal() const noexcept
    {
        return mExportRowsTotal;
    }

    [[nodiscard]] QAtomicInteger<qint64> &MutableExportRowsTotal() noexcept
    {
        return mExportRowsTotal;
    }

    // -----------------------------------------------------------------
    // Find match cache (task 2.11). Cached row-hit list plus the
    // per-bucket density snapshot that backs the "i of N" indicator
    // and the overview-rail match ticks. Keyed by `(needle,
    // wildcards, regex)` so Next / Previous can resolve the new `i`
    // via binary search instead of re-scanning. Owned by
    // `LogSession` because it is derived from the session's rows;
    // any row-set mutation (model reset, filter apply, sort change)
    // drops it via `ResetFindMatchCache()`.
    //
    // - `sortedRows`: deduplicated, capped at
    //   `MainWindow::MAX_FIND_MATCH_COUNT`.
    // - `totalMatches`: exact when `!overflowed`, otherwise a lower
    //   bound. A cursor at match `#10 001` or later resolves to `0`
    //   (no position highlight) under overflow.
    // - `bucketCounts`: per-bucket totals mirrored to the rail
    //   (empty when the rail had zero buckets during the scan).
    //   Restored on find-dock reveal so a cache-hit recount can't
    //   leave the rail on a top-biased strip. Presence-only --
    //   density may be incomplete after an early-exit.
    // -----------------------------------------------------------------

    /// Cap on `FindMatchCache::sortedRows` before overflow kicks in.
    /// At 10 000 the find bar switches its label from an exact
    /// count to "N+"; the rail keeps painting bucket ticks past the
    /// cap because rail density is size-independent.
    static constexpr int MAX_FIND_MATCH_COUNT = 10000;

    struct FindMatchCache
    {
        QString needle;
        bool wildcards = false;
        bool regularExpressions = false;
        bool overflowed = false;
        std::vector<int> sortedRows;
        uint32_t totalMatches = 0;
        std::vector<uint32_t> bucketCounts;
    };

    [[nodiscard]] const std::optional<FindMatchCache> &FindMatchCacheState() const noexcept
    {
        return mFindMatchCache;
    }

    [[nodiscard]] std::optional<FindMatchCache> &MutableFindMatchCacheState() noexcept
    {
        return mFindMatchCache;
    }

    /// Drop the cached match list -- matches
    /// `MainWindow::InvalidateFindMatchCache`.
    void ResetFindMatchCache() noexcept
    {
        mFindMatchCache.reset();
    }

    // -----------------------------------------------------------------
    // Export scalar state (task 2.9). Mirrors the decompression
    // block above -- the `QFutureWatcher` + `StopSource` + progress
    // dialog wiring stays on `MainWindow` until the whole cluster
    // moves; the scalars below are session-scoped identity/timing
    // that the finished slot, cancel paths, and completion toast
    // read across many sites:
    //
    //   * `IsExportInFlight()`      — set in `BeginAsyncExport` /
    //     `BeginAsyncBundleExport`, cleared in `OnExportFinished` /
    //     `CancelInFlightExport`. Guards the finished slot against
    //     a queued callout event already dispatched before
    //     `setFuture({})` cleared the watcher's queue.
    //   * `IsExportBundle()`        — chooses between the plain-
    //     export error/toast strings and their session-bundle
    //     variants.
    //   * `ExportDestinationPath()` — user-facing destination path
    //     the completion toast / message names.
    //   * `ExportFormatLabel()`     — human-readable format name
    //     (`JSON Lines`, `Session bundle`).
    //   * `ExportStartedAt()`       — wall-clock start of the
    //     current export, for the post-success toast.
    //
    // `ClearExportScratchState()` matches the paired reset every
    // export teardown / cancel / completion site performs.
    // -----------------------------------------------------------------

    [[nodiscard]] bool IsExportInFlight() const noexcept
    {
        return mExportInFlight;
    }

    /// Monotonic counter that bumps on every export begin (false ->
    /// true transition of the in-flight flag). See
    /// `DecompressionGeneration()` for the re-entry rationale.
    [[nodiscard]] std::uint64_t ExportGeneration() const noexcept
    {
        return mExportGeneration;
    }

    /// Emits `presentationChanged()` on a real change. Flips the
    /// `Exporting` op bit, `mutationsAllowed`, and
    /// `confirmBeforeClose`.
    void SetExportInFlight(bool inFlight);

    [[nodiscard]] bool IsExportBundle() const noexcept
    {
        return mExportIsBundle;
    }

    /// Label selector only -- does not enter the snapshot's
    /// current projection. Kept `noexcept` and signal-free so it
    /// can be flipped from hot paths.
    void SetExportIsBundle(bool isBundle) noexcept;

    [[nodiscard]] const QString &ExportDestinationPath() const noexcept
    {
        return mExportDestinationPath;
    }

    void SetExportDestinationPath(QString path);

    [[nodiscard]] const QString &ExportFormatLabel() const noexcept
    {
        return mExportFormatLabel;
    }

    void SetExportFormatLabel(QString label);

    [[nodiscard]] std::chrono::steady_clock::time_point ExportStartedAt() const noexcept
    {
        return mExportStartedAt;
    }

    void SetExportStartedAt(std::chrono::steady_clock::time_point startedAt) noexcept;

    /// Clear the two export scratch paths in one call. Matches the
    /// `mExportDestinationPath.clear(); mExportFormatLabel.clear();`
    /// pair every export teardown / cancel / completion site
    /// performs.
    void ClearExportScratchState() noexcept;

    /// Reset every streaming counter + file-name to the "no session"
    /// baseline. Used on `NewSession`, replace-open, retention-fail,
    /// and configuration-load paths.
    void ResetStreamingCountersAndFileName();

    /// Reset the per-file progress fields at the start of each
    /// stream (line/error count + `FirstStreamingBatchSeen`). The
    /// file-name is set separately since it comes from the caller.
    ///
    /// Coalesces every field-level update into a single
    /// `presentationChanged()` fan when at least one field
    /// actually changes -- `mFirstStreamingBatchSeen` flipping
    /// from true to false alters the `Parsing` op bit under
    /// `Mode::Static`, and non-zero error counts alter
    /// `errorCount`. On an already-zero'd session the call is a
    /// no-op with no fan (matches the diff-guarded setters).
    ///
    /// No longer `noexcept` because the signal fan can throw
    /// through subscriber slots; matches every other snapshot-
    /// affecting mutator on this class.
    void ResetStreamingProgress();

    // -----------------------------------------------------------------
    // Identity. Assigned at construction and never reused. Independent
    // of the persistence UUID (which is empty until the first autosave).
    // -----------------------------------------------------------------

    [[nodiscard]] SessionInstanceId InstanceId() const noexcept
    {
        return mInstanceId;
    }

signals:
    /// Emitted whenever the presentation snapshot could have changed:
    ///
    ///   * source-mode flip (`SetCurrentSource`,
    ///     `ResetCurrentSource`, `SetMode`, `ResetMode`,
    ///     `Set/ClearApplyEmbeddedBundleConfigForPath`);
    ///   * operation-state flip
    ///     (`SetDecompressionInFlight`, `SetExportInFlight`,
    ///     `SetSourceWaiting`, `SetFirstStreamingBatchSeen`);
    ///   * dirty-state flip (`MarkFiltersDirty`,
    ///     `ClearFiltersDirty`);
    ///   * error / dropped-error counters
    ///     (`SetStreamingErrorCount`, `SetStreamingErrorsCut`);
    ///   * user-facing labels (`SetStreamingFileName`,
    ///     `ClearStreamingFileName`).
    ///
    /// Each diff-guarded mutator fires the signal only on a real
    /// change (no spurious re-emits when the setter is called with
    /// the current value). Two documented exceptions to "real
    /// change only":
    ///
    ///   1. `SetCurrentSource(...)` emits on every set-with-value
    ///      write because `loglib::LogConfiguration::Source`
    ///      does not (yet) define `operator==`; a field-by-field
    ///      compare is brittle across future additions to
    ///      `Source`, so the setter takes the pragmatic
    ///      "value-had-or-has → fan" path. `nullopt → nullopt`
    ///      is still suppressed. `MutateCurrentSource(fn)` and
    ///      `NotifyPresentationChanged()` similarly fan
    ///      unconditionally by contract (they are the opt-in
    ///      escape hatches for callers who edited raw and want
    ///      the fan without a diff-guard).
    ///   2. `ResetStreamingCountersAndFileName()` and
    ///      `ResetStreamingProgress()` coalesce every field-level
    ///      update into a single fan when at least one tracked
    ///      field transitioned; on an already-cleared session
    ///      they are silent.
    ///
    /// Emission is direct-call by default (same thread as the
    /// mutator); Phase 3 consumers that want to coalesce a batch
    /// of setters into a single UI refresh should either subscribe
    /// with `Qt::QueuedConnection` or install a debounce timer on
    /// their side. Row-count changes propagate via the owned
    /// models' `rowsInserted` / `modelReset` signals, not via this
    /// one — the model is the authoritative row-count sender
    /// (`PresentationSnapshot()` still projects `rowCount` /
    /// `visibleRows` on demand from the model).
    ///
    /// The shell rebuilds title / status / tab chrome from
    /// `PresentationSnapshot()` in response.
    void presentationChanged();

    /// Emitted on a `false → true` or `true → false` transition of
    /// `IsFiltersDirty()`. The shell binds this to
    /// `MainWindow::UpdateWindowTitle` so the OS `[*]` marker
    /// tracks the session's modified state.
    ///
    /// `presentationChanged()` also fires on the same transition
    /// (dirty state is part of the snapshot); this signal exists
    /// as a narrow convenience for the `setWindowModified()`
    /// slot which needs the new boolean directly.
    void filtersDirtyChanged(bool dirty);

    /// Emitted on a `false -> true` or `true -> false` transition
    /// of `IsRotationFlashActive()`. The shell subscribes into
    /// `mSessionConnections` and refreshes the streaming status
    /// label; the label reads `session->IsRotationFlashActive()`
    /// inside the same slot, which resolves to the currently-
    /// bound session -- so a multi-tab window projects the flash
    /// only when the flashing session is the active tab.
    void rotationFlashChanged(bool active);

private:
    // Raw pointers rather than `QPointer` because these services are
    // owned by the application coordinator and outlive every
    // `LogSession`. Using raw pointers here also keeps the header
    // free of the service definitions (see accessor note above).
    ThemeControl *mTheme = nullptr;
    SessionHistoryManager *mHistoryManager = nullptr;
    RegexTemplateRegistry *mRegexTemplateRegistry = nullptr;

    // Owned via Qt parentage — each of the five is constructed as a
    // child of `this` in the ctor (see accessor block above for the
    // required lifetime order). Raw pointers not `QPointer` because
    // the parent-child destruction chain guarantees the members are
    // torn down before `LogSession` itself.
    AnchorManager *mAnchors = nullptr;
    HighlightRuleSet *mHighlights = nullptr;
    LogModel *mModel = nullptr;
    RowOrderProxyModel *mRowOrderProxyModel = nullptr;
    LogFilterModel *mSortFilterProxyModel = nullptr;

    /// Session-modified marker (see `IsFiltersDirty()`); flipped by
    /// `MarkFiltersDirty()` / `ClearFiltersDirty()` with change
    /// detection so `filtersDirtyChanged` fires only on transitions.
    bool mFiltersDirty = false;

    /// Re-entrancy gate for bulk configuration loads; while `true`,
    /// `MarkFiltersDirty()` is a no-op so a load path can settle its
    /// dirty state exactly once on scope exit.
    bool mLoadingConfiguration = false;

    /// Deferred-sort latch (see `HasPendingApplySortFromConfig`).
    /// Set by load / restore paths; cleared by
    /// `OnStreamingFinished` (or an early-return streaming path)
    /// after the loaded sort has been applied to the proxy.
    bool mPendingApplySortFromConfig = false;

    /// Simple-mode leaves keyed by UUID (see `SimpleLeaves()`).
    std::unordered_map<std::string, loglib::LeafRule> mSimpleLeaves;

    /// Display order for `mSimpleLeaves` (see `SimpleLeafOrder()`).
    std::vector<std::string> mSimpleLeafOrder;

    /// Re-entrancy latch for the `enumColumnsChanged`-driven filter
    /// rebuild (see `IsApplyingEnumRebuild()`).
    bool mApplyingEnumRebuild = false;

    /// Streaming session kind (see `SessionMode()`).
    Mode mMode = Mode::Idle;

    /// Mirror of `mMode` retained across the transition into `Idle`
    /// so a close after a finished live tail still routes through
    /// the correct auto-save gate. See `LastTerminalMode()`.
    Mode mLastTerminalMode = Mode::Idle;

    /// Session-switch guard (see `IsSessionSwitchInProgress()`).
    bool mSessionSwitchInProgress = false;

    /// Retained-row count in `Model()` (see `StreamingLineCount()`).
    qsizetype mStreamingLineCount = 0;

    /// Retained parse-error count (see `StreamingErrorCount()`).
    qsizetype mStreamingErrorCount = 0;

    /// High-water mark into `Model()->StreamingErrors()`
    /// (see `StreamingErrorsCut()`).
    std::size_t mStreamingErrorsCut = 0;

    /// One-shot column-auto-resize gate (see `FirstStreamingBatchSeen()`).
    bool mFirstStreamingBatchSeen = false;

    /// `SourceStatus::Waiting` latch (see `IsSourceWaiting()`).
    bool mSourceWaiting = false;

    /// Per-session rotation-flash latch (see
    /// `IsRotationFlashActive()` / `TriggerRotationFlash()`).
    /// Moved off `MainWindow` in the phase-4 review-4 resolution
    /// so multi-tab windows do not cross-contaminate flashes
    /// across sibling sessions.
    bool mRotationFlashActive = false;

    /// Display label for the file currently being streamed
    /// (see `StreamingFileName()`).
    QString mStreamingFileName;

    /// Current source descriptor (see `CurrentSource()`).
    std::optional<loglib::LogConfiguration::Source> mCurrentSource;

    /// FIFO of queued static files (see `PendingOpenFiles()`).
    QStringList mPendingOpenFiles;

    /// Parse/open errors collected while draining
    /// `mPendingOpenFiles` (see `PendingOpenErrors()`).
    std::vector<std::string> mPendingOpenErrors;

    /// Decompression errors collected while draining
    /// `mPendingOpenFiles` (see `PendingDecompressionErrors()`).
    std::vector<std::string> mPendingDecompressionErrors;

    /// Session-owned parse-error log (see `ParseErrorLog()`).
    /// `ParseErrorsDock` snapshots into this on Unbind and
    /// replays it on Bind (task 5.4).
    SessionParseErrorLog mParseErrorLog;

    /// Session-owned Find query state (see `FindQuery()`).
    /// `FindDock` / `FindRecordWidget` snapshot into this on
    /// Unbind and restore from this on Bind (task 5.3).
    SessionFindQueryState mFindQuery;

    /// Session-owned histogram pin state (see `HistogramState()`).
    /// `HistogramDock` snapshots into this on Unbind and restores
    /// from this on Bind (task 5.6).
    SessionHistogramState mHistogramState;

    /// Session-owned record-detail pin state (see
    /// `RecordDetailPin()`). `RecordDetailDock` snapshots into this
    /// on Unbind and restores from this on Bind (task 5.7).
    SessionRecordDetailPin mRecordDetailPin;

    /// Session-owned anchors-dock selection state (see
    /// `AnchorsSelection()`). `AnchorsDock` snapshots into this on
    /// Bind out and restores from this on Bind in (task 5.5,
    /// origin-review finding M9).
    SessionAnchorsSelection mAnchorsSelection;

    /// Primary path to tail after the historical prefix drains
    /// (see `PendingLiveTailPrimary()`).
    QString mPendingLiveTailPrimary;

    /// Retention cap saved across a historical-prefix load
    /// (see `PendingLiveTailRetention()`).
    std::size_t mPendingLiveTailRetention = 0;

    /// Per-window CLI opt-out for rotation history
    /// (see `DisableRotationHistoryOverride()`).
    bool mDisableRotationHistoryOverride = false;

    /// Exact caller inputs restored by `UndoRotationExpansion`
    /// (see `LastRotationExpansionOriginalInputs()`).
    QStringList mLastRotationExpansionOriginalInputs;

    /// Preserve live-tail mode across the undo (see
    /// `LastRotationExpansionWasLiveTail()`).
    bool mLastRotationExpansionWasLiveTail = false;

    /// Persistence identity for this session's recents entry
    /// (see `AutoSaveUuid()`).
    QString mAutoSaveUuid;

    /// Publish latch mirroring the process-shared `openWindowsAtQuit`
    /// set (see `IsAutoSaveUuidPublished()`).
    bool mAutoSaveUuidPublished = false;

    /// Bundle path whose embedded configuration is allowed to apply
    /// when the pending decompression finishes (see
    /// `ApplyEmbeddedBundleConfigForPath()`).
    QString mApplyEmbeddedBundleConfigForPath;

    /// Decompression-in-flight latch (see `IsDecompressionInFlight()`).
    bool mDecompressionInFlight = false;

    /// Monotonic generation for decompression operations. Bumped
    /// on each false -> true transition of `mDecompressionInFlight`
    /// (i.e. every `Begin*` call). Poll timers capture this at arm
    /// time; a mismatch means a completion already fired for the
    /// generation we care about (review finding #4).
    std::uint64_t mDecompressionGeneration = 0;

    /// User-facing path of the file being decompressed (see
    /// `DecompressionOriginalPath()`).
    QString mDecompressionOriginalPath;

    /// Human-readable codec name (see `DecompressionCodecName()`).
    QString mDecompressionCodecName;

    /// Wall-clock start of the current decompression (see
    /// `DecompressionStartedAt()`).
    std::chrono::steady_clock::time_point mDecompressionStartedAt;

    /// Export-in-flight latch (see `IsExportInFlight()`).
    bool mExportInFlight = false;

    /// Monotonic generation for export operations. See
    /// `mDecompressionGeneration` for the rationale.
    std::uint64_t mExportGeneration = 0;

    /// Bundle vs. plain-export label selector (see `IsExportBundle()`).
    bool mExportIsBundle = false;

    /// User-facing destination path for the completion toast
    /// (see `ExportDestinationPath()`).
    QString mExportDestinationPath;

    /// Human-readable format name (see `ExportFormatLabel()`).
    QString mExportFormatLabel;

    /// Wall-clock start of the current export (see
    /// `ExportStartedAt()`).
    std::chrono::steady_clock::time_point mExportStartedAt;

    /// Cooperative stop source paired with the decompression worker
    /// (see `DecompressionStopSource()`).
    loglib::StopSource mDecompressionStopSource;

    /// Cooperative stop source paired with the export worker
    /// (see `ExportStopSource()`).
    loglib::StopSource mExportStopSource;

    /// Decompression progress atomic; worker writes decompressed
    /// bytes, GUI polls (see `DecompressionBytesIn()`).
    QAtomicInteger<qint64> mDecompressionBytesIn = 0;

    /// Decompression total-bytes atomic; seeded from the compressed
    /// file size at the start of each open (see
    /// `DecompressionTotalBytesIn()`).
    QAtomicInteger<qint64> mDecompressionTotalBytesIn = 0;

    /// Export rows-written atomic; worker publishes, poll timer
    /// reads (see `ExportRowsWritten()`).
    QAtomicInteger<qint64> mExportRowsWritten = 0;

    /// Export rows-total atomic; seeded at export start
    /// (see `ExportRowsTotal()`).
    QAtomicInteger<qint64> mExportRowsTotal = 0;

    /// Cached match list backing the find bar's "i of N"
    /// indicator. Populated by `MainWindow::UpdateFindMatchInfo`
    /// and dropped on any row-set mutation.
    std::optional<FindMatchCache> mFindMatchCache;

    /// Wall-clock since the active live-tail session started
    /// (see `LiveTailElapsedTimer()`).
    QElapsedTimer mLiveTailElapsedTimer;

    /// Session-owned QFutureWatchers for the decompression + export
    /// workers. Both are lazily allocated by the shell's
    /// ``Begin*`` methods and parented on ``this`` so the watcher's
    /// lifetime naturally ends with the session it observes.
    DecompressionWatcher *mDecompressionWatcher = nullptr;
    ExportWatcher *mExportWatcher = nullptr;

    SessionInstanceId mInstanceId = SessionInstanceId::Next();
};
