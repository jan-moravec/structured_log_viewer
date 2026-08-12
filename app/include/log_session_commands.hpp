#pragma once

#include "log_session_presentation.hpp"

#include <QString>
#include <QStringList>

#include <cstdint>

/// Narrow command surface the window shell uses to drive a session.
///
/// The interface is deliberately small in Phase 1 (task 1.6): only
/// the shell-visible commands with stable meaning are declared. Each
/// phase adds the commands it moves out of `MainWindow`; Phase 2
/// grows filter / sort / stream commands, Phase 3 the view-scoped
/// navigation commands, Phase 5 the dock rebinding commands, and
/// Phase 6 the multi-source targeted variants.
///
/// Ownership contract:
///
/// - Every method dispatches immediately on the receiving session.
/// - Commands that spawn a worker capture the receiving session
///   identity, not the window's current active session (PRD §8.1).
/// - Failures surface as signals on the concrete `LogSession`
///   implementation, not as return values here; return types stay
///   `void` so a `Qt::QueuedConnection` dispatch is trivial. The
///   sole exception is `RequestClose`, which is aggregated
///   synchronously by `MainWindow::closeEvent` and therefore has to
///   return a `SessionCloseResult` so a per-session veto can bubble
///   up before window chrome is saved.
class LogSessionCommands
{
public:
    virtual ~LogSessionCommands() = default;

    LogSessionCommands(const LogSessionCommands &) = delete;
    LogSessionCommands &operator=(const LogSessionCommands &) = delete;
    LogSessionCommands(LogSessionCommands &&) = delete;
    LogSessionCommands &operator=(LogSessionCommands &&) = delete;

    // -----------------------------------------------------------------
    // Source / open / restore commands (Phase 2 fills the bodies).
    // -----------------------------------------------------------------

    /// Discard the current session and return to an empty view.
    virtual void RequestNewSession() = 0;

    /// Open the queued files in the caller's mode. Called by
    /// `File → Open…`, drag/drop, and forwarded single-instance
    /// launches once they resolve which tab to target.
    enum class OpenMode
    {
        Append,
        Replace,
    };
    virtual void RequestOpenFiles(const QStringList &files, OpenMode mode) = 0;

    /// Open a live-tail session over @p filePath.
    virtual void RequestOpenLogStream(const QString &filePath) = 0;

    // -----------------------------------------------------------------
    // Persistence commands (Phase 2).
    // -----------------------------------------------------------------

    /// Autosave the current session to its persistence identity.
    /// @p publishOpenWindow feeds the crash-restore fan.
    virtual void RequestAutoSaveSnapshot(bool publishOpenWindow) = 0;

    // -----------------------------------------------------------------
    // Close (aggregated by the shell over all hosted sessions).
    // -----------------------------------------------------------------

    /// Side-effect-free probe: return the reasons this session
    /// cannot be silently torn down (in-flight workers, unsaved
    /// filter edits, ...). A zero mask means the shell can proceed
    /// without invoking `RequestClose`. Called during the shell's
    /// close aggregation walk before any user-visible prompt.
    [[nodiscard]] virtual std::uint32_t PreCheckClose() const = 0;

    /// Ask the session to prepare for closing. Runs the
    /// side-effecting sequence (cancel-and-drain in-flight workers,
    /// prompt for unsaved edits, autosave writes) and returns the
    /// outcome so `MainWindow::closeEvent` can veto or continue.
    ///
    /// Phase 2 leaves the body as a stub because the worker-drain
    /// and prompt orchestration still live on the shell; the shell
    /// aggregates `PreCheckClose()` bitmasks and drives its own
    /// prompts. Phase 3 moves the orchestration in and `RequestClose`
    /// becomes the sole entry point.
    virtual SessionCloseResult RequestClose() = 0;

protected:
    LogSessionCommands() = default;
};
