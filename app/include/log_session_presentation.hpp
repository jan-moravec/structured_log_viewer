#pragma once

#include <QString>

#include <compare>
#include <cstddef>
#include <cstdint>

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
