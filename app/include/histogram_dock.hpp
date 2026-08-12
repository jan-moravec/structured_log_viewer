#pragma once

#include "histogram_model.hpp"

#include <QDockWidget>
#include <QPointer>

#include <cstddef>

class AnchorManager;
class HistogramWidget;
class LogModel;
class LogSession;
class ThemeControl;
class QCloseEvent;
class QShowEvent;
struct SessionBindContext;

/// Bottom-docked strip that plots per-time-bucket, per-level row
/// counts over the current session (ROADMAP item 2).
///
/// Owns a `HistogramModel` (subscribed to `LogModel`) and a
/// `HistogramWidget` (custom paint), and forwards their signals so
/// navigation logic stays in `MainWindow`. `closed()` mirrors
/// `AnchorsDock::closed` for `MainWindow::WireDockToggle`.
class HistogramDock : public QDockWidget
{
    Q_OBJECT

public:
    /// @p anchors, when non-null, enables the tick strip above the
    /// bars (one coloured tick per palette slot in each anchored
    /// bucket). Pass `nullptr` to disable anchor tracking entirely.
    HistogramDock(LogModel *model, ThemeControl *theme, AnchorManager *anchors, QWidget *parent = nullptr);

    /// Non-owning accessor for tests.
    [[nodiscard]] HistogramModel *ModelForTest() const noexcept
    {
        return mModel;
    }

    /// Non-owning accessor for tests.
    [[nodiscard]] HistogramWidget *WidgetForTest() const noexcept
    {
        return mWidget;
    }

    /// Task 5.6: swap the dock's active session context.
    ///
    /// Snapshots the outgoing session's bucket-size pin into its
    /// `SessionHistogramState`, then cancels the coalesce timer,
    /// calls `HistogramModel::BindSources(context.model,
    /// context.anchors)`, and reapplies the incoming session's
    /// pin so an auto-picker rebuild cannot silently override it.
    ///
    /// When the dock is not currently visible the source rebuild
    /// is deferred: a hidden strip has nothing to paint, so the
    /// expensive full-model walk waits until the next `showEvent`
    /// (or until the caller explicitly reveals the dock). The
    /// source subscriptions themselves are still installed so
    /// incremental appends can accumulate into the (idle) bucket
    /// index for the next reveal.
    ///
    /// Passing a `SessionBindContext::MakeUnbound()` detaches from
    /// every session-owned source (equivalent to
    /// `HistogramModel::BindSources(nullptr, nullptr)`).
    void Bind(const SessionBindContext &context);

    /// Explicit unbind. Equivalent to `Bind(SessionBindContext::
    /// MakeUnbound())` but reads clearer at teardown call sites.
    void Unbind();

    /// Test seam: the session this dock currently mirrors state
    /// into on Bind / Unbind. Null before the first Bind or after
    /// an Unbind.
    [[nodiscard]] LogSession *boundSessionForTest() const noexcept
    {
        return mBoundSession.data();
    }

signals:
    /// Bar clicked (no drag). Consumer maps to a source row via
    /// `HistogramModel::FirstRowInBucket`.
    void bucketClicked(std::size_t bucketIndex);

    /// Tick-strip clicked on an anchored column. `sourceRow` is the
    /// earliest anchored source-model row in that column, so
    /// consumers can route it to `MainWindow::SelectSourceRow`.
    void anchorClicked(int sourceRow);

    /// User dragged a range and released. Bounds are epoch
    /// microseconds, inclusive.
    void timeRangeSelected(qint64 fromEpochMicros, qint64 toEpochMicros);

    /// Fired on genuine user dismissal (X / system close).
    void closed();

protected:
    void closeEvent(QCloseEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    /// Save the currently-bound session's presentation state
    /// (bucket pin) into its `SessionHistogramState`. No-op when
    /// no session is currently bound.
    void SaveStateIntoBoundSession();

    /// Reapply the incoming session's `SessionHistogramState`
    /// to `mModel` (e.g. `SetBucketSize` when the pin latch
    /// says so). Called after `BindSources` so the auto-picker
    /// rebuild inside `OnModelReset` cannot silently override
    /// the user's choice.
    void RestoreStateFromSession(LogSession *session);

    QPointer<HistogramModel> mModel;
    HistogramWidget *mWidget = nullptr;

    /// Session this dock currently mirrors state into. Null before
    /// the first `Bind` or after an explicit `Unbind`. `QPointer`
    /// so a session torn down out-of-order zeroes the alias rather
    /// than dangling into a `MutableHistogramState` write on the
    /// next Bind.
    QPointer<LogSession> mBoundSession;

    /// True when a `Bind` was received while the dock was hidden;
    /// deferred `HistogramModel::Rebuild()` runs on the next
    /// `showEvent`. Keeps hidden strips off the expensive full-
    /// model walk while still letting incremental appends
    /// accumulate through the installed subscriptions.
    bool mDeferredRebuildOnShow = false;
};
