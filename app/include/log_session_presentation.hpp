#pragma once

#include <QString>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

/// Value types shared between `LogSession`, `LogSessionView`, and
/// `MainWindow`. Every type here is Qt-value semantics only: no
/// pointer to a window, dock, or model. The shell reads snapshots
/// and observes signals; it never inspects model internals.
///
/// This header is deliberately minimal in Phase 1 (task 1.5–1.7). It
/// captures the fixed vocabulary and shape. Phase 2 populates the
/// commands and Phase 3 refines the presentation snapshot when the
/// view lands its progress and dirty projection.

/// Stable identity for a `LogSession`. Assigned once at construction
/// via `Next()` and never reused; independent of the persistence
/// UUID (which may still be empty for a session that has not yet
/// been autosaved). Tabs / restore / autosave use this handle to
/// pin their target when the active tab changes underneath a
/// callback (PRD §2.1 objective 5, §8.1).
class SessionInstanceId
{
public:
    using Value = std::uint64_t;

    SessionInstanceId() = default;
    constexpr explicit SessionInstanceId(Value value) noexcept
        : mValue(value)
    {
    }

    [[nodiscard]] constexpr Value value() const noexcept
    {
        return mValue;
    }

    /// A default-constructed id is the sentinel "no session".
    [[nodiscard]] constexpr bool isValid() const noexcept
    {
        return mValue != 0;
    }

    friend constexpr bool operator==(SessionInstanceId, SessionInstanceId) noexcept = default;
    friend constexpr auto operator<=>(SessionInstanceId, SessionInstanceId) noexcept = default;

    /// Issue the next monotonically-increasing id. Thread-safe. The
    /// counter is process-scoped so two windows never collide.
    [[nodiscard]] static SessionInstanceId Next() noexcept;

private:
    Value mValue = 0;
};

/// Which kind of source the session is currently hosting.
///
/// Single-valued: the tab-strip badge selector reads one value and
/// picks a chrome. Priority (highest first) when several
/// classifications apply to the same session:
///
///   1. `Stdin` / `Network` -- source-kind override; the whole
///      file-family projection is skipped.
///   2. `Bundle` -- the session was opened from a session-bundle
///      file (embedded config armed). Bundle is compressed by
///      construction and can be live-tailed in principle, but the
///      tab still wants the "bundle" affordance because that is
///      the most distinctive.
///   3. `LiveTail` -- streaming producer active. Wins over
///      MultiFile / Compressed / StaticFile because live-tail
///      state changes frequently and dominates the badge.
///   4. `MultiFile` -- static open of more than one locator
///      (e.g. rotation history append). Renders as "app.log +N"
///      in the tab title (PRD FR-15).
///   5. `Compressed` -- decompression worker armed (source was
///      opened from a compressed archive that is not a bundle).
///      Transient today because the flag clears after decompression
///      finishes; phase 3 grows the "was compressed" latch.
///   6. `StaticFile` -- plain single-file static open.
///   7. `Idle` -- no source bound.
enum class SessionSourceMode : std::uint8_t
{
    Idle,
    StaticFile,
    LiveTail,
    Stdin,
    Network,
    Bundle,
    Compressed,
    MultiFile,
};

/// The high-level operation slot currently busy inside a session.
/// Tabs project the top-priority state onto their compact indicator
/// (PRD FR-18); several states can co-exist so this is a bitmask.
enum class SessionOperationState : std::uint32_t
{
    Idle = 0,
    /// Static parse is starting up but has not yet delivered its
    /// first batch to the model -- i.e. the tab should show a
    /// "loading" spinner. Clears on the first non-empty batch
    /// (`FirstStreamingBatchSeen()` flips true), even though the
    /// parse itself may still be streaming subsequent batches.
    /// Once the tab has rows the row-count is a better user cue
    /// than a spinner, so consumers usually pair this bit with
    /// `rowCount == 0` when picking a spinner variant.
    Parsing = 1U << 0,
    Decompressing = 1U << 1, ///< Async `DecompressingByteSource` worker armed.
    Exporting = 1U << 2,     ///< Filtered-row or bundle export worker armed.
    Ingesting = 1U << 3,     ///< Live-tail / network producer active.
    Paused = 1U << 4,        ///< Producer paused via `TogglePauseStream`.
    SourceWaiting = 1U << 5, ///< `SourceStatus::Waiting` latched.
    Disconnected = 1U << 6,  ///< Network / stdin placeholder awaiting reconnect.
};

/// Dirty state a session presents to the tab strip and window title.
struct SessionDirtyState
{
    bool filtersDirty = false;            ///< Unsaved user edits since last autosave.
    bool restorableInPlace = false;       ///< Restorable-source: safe to auto-preserve on close.
    bool ephemeralUnreproducible = false; ///< Stdin / network with no restore path.
};

/// Outcome of a *side-effecting* close request. `MainWindow::closeEvent`
/// aggregates the vector of per-session results and vetoes the window
/// close when any child returns `Cancelled` or `Failed`.
///
/// The Phase 2 body of `LogSession::RequestClose` is still stubbed;
/// the shell drives the actual worker-drain / user-prompt sequence
/// from `MainWindow::closeEvent`. Phase 3 wires this in and can then
/// return `Cancelled` for a user "keep this tab" click, or `Failed`
/// for a teardown that could not finish (autosave-write error, etc.).
///
/// The synchronous side-effect-free variant is
/// `SessionClosePreconditions` below, which the shell uses to *ask*
/// whether a session needs a follow-up (worker drain, user prompt)
/// before invoking `RequestClose`.
enum class SessionCloseResult : std::uint8_t
{
    Closed,    ///< Cleanly torn down and workers drained.
    Cancelled, ///< User cancelled a confirmation prompt.
    Failed,    ///< Teardown could not finish; window must not proceed.
};

/// Bitmask of reasons a close *cannot* complete silently. Populated
/// by `LogSession::PreCheckClose()`, which is idempotent and
/// side-effect-free: the shell calls it once during its close
/// aggregation walk and only invokes the side-effecting
/// `RequestClose` on the sessions whose preconditions are non-zero.
///
/// A zero mask means the session can be torn down without a prompt
/// or worker drain (equivalent to the old `SessionCloseResult::Closed`
/// short-circuit). Non-zero masks distinguish the follow-ups the
/// shell owns:
///
///   * `FiltersDirty`   -- unsaved user edits; the shell shows a
///                          save/close prompt.
///   * `DecompressionInFlight` / `ExportInFlight` -- an async worker
///                          is armed; the shell must cancel-and-drain
///                          before the tab can close.
///
/// Phase 3 grows this taxonomy (e.g. `Ephemeral` for stdin/network
/// sources that would be lost on tab close). The names are chosen so
/// each new flag maps 1:1 to a documented shell follow-up rather than
/// smearing across the ambiguous "Cancelled" bucket that motivated
/// this split.
enum class SessionClosePreconditions : std::uint32_t
{
    None = 0,
    FiltersDirty = 1U << 0,
    DecompressionInFlight = 1U << 1,
    ExportInFlight = 1U << 2,
};

[[nodiscard]] constexpr std::uint32_t operator|(SessionClosePreconditions a, SessionClosePreconditions b) noexcept
{
    return static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b);
}

[[nodiscard]] constexpr std::uint32_t operator|(std::uint32_t a, SessionClosePreconditions b) noexcept
{
    return a | static_cast<std::uint32_t>(b);
}

[[nodiscard]] constexpr std::uint32_t operator&(std::uint32_t a, SessionClosePreconditions b) noexcept
{
    return a & static_cast<std::uint32_t>(b);
}

constexpr std::uint32_t &operator|=(std::uint32_t &a, SessionClosePreconditions b) noexcept
{
    a |= static_cast<std::uint32_t>(b);
    return a;
}

/// Session-owned Find query state that survives a Bind / Unbind
/// cycle on `FindDock` / `FindRecordWidget` (task 5.3). Captures
/// the query text plus the two mutually-exclusive matcher toggles
/// so a phase-6 tab switch back to a previously-bound session
/// restores what the user last typed and how they had the matcher
/// configured. Match-count state is not persisted -- it is a
/// pure function of the query + the session's model, so the
/// dock re-arms the debounce on Bind and lets `MatchCountRequested`
/// refresh it.
///
/// A default-constructed value represents the pristine "no query
/// yet" state -- equivalent to a session immediately after
/// construction.
struct SessionFindQueryState
{
    QString query;          ///< Last search text (may be empty).
    bool wildcards = false; ///< Wildcard-matcher toggle.
    bool regex = false;     ///< Regex-matcher toggle. Mutually exclusive with `wildcards`.
};

/// Session-owned parse-error state that survives a Bind / Unbind
/// cycle on `ParseErrorsDock` (task 5.4). The dock replays the
/// batches on Bind and captures the current state on Unbind; the
/// authoritative store lives here so a phase-6 tab switch to a
/// previously-bound session restores every entry, the running
/// counts, and the "auto-raise once per session" latch exactly.
///
/// A default-constructed value represents the pristine "no errors
/// yet" state -- equivalent to a session immediately after
/// construction or after `LogSession::ResetParseErrorLog()`.
struct SessionParseErrorBatch
{
    QString title;                   ///< Group header (e.g. `tr("Error Parsing Logs")`).
    std::vector<std::string> errors; ///< Error rows, in insertion order.
};

struct SessionParseErrorLog
{
    /// Every batch appended since the last `ResetParseErrorLog()`,
    /// in insertion order. The dock replays them on `Bind`.
    std::vector<SessionParseErrorBatch> batches;

    /// Errors evicted by the dock's `MAX_DISPLAYED_ERRORS` cap.
    /// Mirrored here so restoring on `Bind` re-renders the "N
    /// earlier dropped" overflow footer.
    int droppedCount = 0;

    /// One-shot `ParseErrorsDock::firstBatchArrived` latch. Cleared
    /// only by `ResetParseErrorLog`; the dock's in-line Clear
    /// button does NOT re-arm this so the user cannot be yanked
    /// back to a dock they explicitly dismissed.
    bool hasSeenFirstBatch = false;
};

/// Session-owned histogram presentation state that survives a
/// Bind / Unbind cycle on `HistogramDock` / `HistogramModel`
/// (task 5.6). The bucket-size pin is user-driven (context menu
/// entries "1 s / 10 s / 1 min / ..."), so a phase-6 tab switch
/// back to a previously-bound session must restore both the
/// chosen rung and the "manual pin" latch (otherwise the next
/// auto re-pick would silently override the user's choice).
///
/// A default-constructed value represents the pristine "no pin
/// yet" state -- equivalent to a fresh session that has never
/// visited the histogram dock.
///
/// The bucket-size value is stored as `std::uint8_t` matching the
/// underlying type of `loglib::HistogramBucketSize` so this
/// header does not have to pull in loglib. Consumers cast on the
/// boundary; the dock is the only writer.
struct SessionHistogramState
{
    /// True after the user picked a specific rung via the widget's
    /// context menu. Suppresses `ApplyAutoBucketSize` re-picks on
    /// subsequent rebuilds until the user hits "Reset zoom (auto)".
    bool bucketSizePinned = false;

    /// Pinned rung, encoded as the underlying type of
    /// `loglib::HistogramBucketSize`. `std::nullopt` when no pin
    /// has ever been applied. When `bucketSizePinned == false`
    /// this may still carry the last pinned rung (harmless -- the
    /// pin latch gates its application).
    std::optional<std::uint8_t> bucketSize;
};

/// Session-owned record-detail pin state that survives a Bind /
/// Unbind cycle on `RecordDetailDock` (task 5.7). Persists the
/// last-pinned source row so a phase-6 tab switch back to a
/// previously-bound session restores the record the user was
/// looking at rather than showing the default "select a row"
/// placeholder.
///
/// `pinnedSourceRow == -1` and `everPinned == false` is the
/// pristine default. `pinnedSourceRow == -1` with
/// `everPinned == true` distinguishes "the pinned row was
/// evicted" from "no row was ever pinned"; the dock uses the
/// distinction to show the `EvictedRecordPlaceholder()` copy.
/// Session-owned selection state for `AnchorsDock` (task 5.5, origin
/// review finding M9). Persisting the selected anchor's stable key
/// across a phase-6 tab switch matches FR-60 (tab state is
/// session-authoritative; navigation state does not silently drift
/// on the switch back). Empty `keyLocator` + `keyLineId == 0` means
/// "no anchor was selected".
struct SessionAnchorsSelection
{
    std::string keyLocator;
    std::uint64_t keyLineId = 0;
};

struct SessionRecordDetailPin
{
    /// -1 means "no pin (or evicted)". Row number is a fallback
    /// used only when the stable `keyLocator`/`keyLineId` fail
    /// to resolve on restore (e.g. the target line was compacted
    /// out and the file no longer has any row bearing its id).
    int pinnedSourceRow = -1;
    /// Sticky latch; only reset by explicit `Clear`. Distinguishes
    /// "never pinned anything, show default placeholder" from
    /// "had a pin, row got evicted, show 'record is gone'
    /// placeholder".
    bool everPinned = false;

    /// Stable identity mirroring `AnchorManager::Key`. Populated
    /// on save when the pinned row has a resolvable anchor key;
    /// preferred over `pinnedSourceRow` on restore because it
    /// survives leading-row eviction (finding H4). Empty
    /// `keyLocator` + `keyLineId == 0` means "no stable key was
    /// available", in which case restore falls back on
    /// `pinnedSourceRow`.
    std::string keyLocator;
    std::uint64_t keyLineId = 0;
};

/// Snapshot the shell reads for menus, toolbars, status bar, tab
/// labels, and window title. Phase 3 fills the remaining fields as
/// `LogSessionView` and `LogSession` migrate their state.
struct SessionPresentationSnapshot
{
    SessionSourceMode mode = SessionSourceMode::Idle;
    std::uint32_t operations = 0; ///< OR of `SessionOperationState` values.
    SessionDirtyState dirty;

    QString shortLabel;    ///< For the tab title (elided-safe, no path).
    QString tooltip;       ///< For the tab tooltip (full source or set).
    QString sourceLabel;   ///< For the status bar / window title.
    QString statusSummary; ///< Localised source-status line.

    qsizetype rowCount = 0;      ///< Retained rows in the session model.
    qsizetype visibleRows = 0;   ///< Rows after filter/sort.
    qsizetype errorCount = 0;    ///< Parse-error count for indicators.
    qsizetype droppedErrors = 0; ///< Errors dropped by retention.

    /// Whether the shell may currently show mutation commands
    /// (columns editor, filter menus, sort toggles). The session
    /// mutation gate is per-session, not window-wide.
    bool mutationsAllowed = true;

    /// Whether the shell should offer a save/close confirmation.
    /// True whenever an in-flight worker cannot yet be silently
    /// discarded (Phase 2 fills in the finer taxonomy).
    bool confirmBeforeClose = false;
};
