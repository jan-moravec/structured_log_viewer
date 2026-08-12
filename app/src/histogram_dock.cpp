#include "histogram_dock.hpp"

#include "anchor_manager.hpp"
#include "histogram_model.hpp"
#include "histogram_widget.hpp"
#include "log_model.hpp"
#include "log_session.hpp"
#include "log_session_presentation.hpp"
#include "session_bind_context.hpp"

#include <loglib/histogram_bucket_index.hpp>

#include <QCloseEvent>
#include <QShowEvent>
#include <QString>
#include <QVBoxLayout>
#include <QWidget>

#include <cstdint>

HistogramDock::HistogramDock(LogModel *model, ThemeControl *theme, AnchorManager *anchors, QWidget *parent)
    : QDockWidget(tr("Histogram"), parent)
{
    setObjectName(QStringLiteral("histogramDock"));
    setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);

    mModel = new HistogramModel(model, anchors, this);
    mWidget = new HistogramWidget(mModel, theme, this);
    setWidget(mWidget);

    connect(mWidget, &HistogramWidget::bucketClicked, this, &HistogramDock::bucketClicked);
    connect(mWidget, &HistogramWidget::anchorClicked, this, &HistogramDock::anchorClicked);
    connect(mWidget, &HistogramWidget::timeRangeSelected, this, &HistogramDock::timeRangeSelected);
}

void HistogramDock::Bind(const SessionBindContext &context)
{
    // Task 5.6: swap the active session context.
    //
    // Sequence (must stay this order):
    //   1. Snapshot the outgoing session's bucket-pin state so
    //      a phase-6 tab switch back to it restores what the
    //      user last chose (and its manual-pin latch).
    //   2. Cancel the coalesce timer so a stale bucketsChanged
    //      cannot land after the source swap.
    //   3. Ask the model to swap its guarded source pointers.
    //      Pass `deferRebuild=true` when the dock is hidden so the
    //      full-model walk waits for the next `showEvent` (a
    //      hidden strip has nothing to paint). Column-index
    //      recompute + availability signal still fire so the
    //      accessor stays honest even on the deferred path.
    //   4. Restore the incoming session's pin state *after* the
    //      source swap's auto-picker rebuild, so the pin wins.
    //   5. Update `mBoundSession` last -- if any of the above
    //      throws, the guard still points at the outgoing session
    //      and the next Bind can retry cleanly.
    //
    // A `MakeUnbound()` context (both session and view null)
    // detaches from every session-owned source; the dock retains
    // its widget subtree so a subsequent Bind can re-arm.

    // Same-session early-return: a redundant Bind on the same
    // session is a no-op. Skips the pin-latch reset in
    // `HistogramModel::BindSources` and the full model walk that
    // would otherwise fire from `Rebuild()`. Mirrors the
    // short-circuits on `FindDock::Bind` / `ParseErrorsDock::
    // Bind` / `AnchorsDock::Bind`. Only fires when the dock is
    // already bound (`mBoundSession` non-null); a fresh Bind
    // through a never-bound dock still goes the full path.
    //
    // Phase-5 invariant: a `LogSession` does not swap its model
    // quintet in-place -- `Model()` / `Anchors()` stay stable for
    // the session's lifetime. So `mBoundSession == incoming`
    // implies `context.model` / `context.anchors` still match
    // what we're pointing at.
    LogSession *incoming = context.session.data();
    if (!mBoundSession.isNull() && mBoundSession.data() == incoming)
    {
        return;
    }

    SaveStateIntoBoundSession();

    if (mModel != nullptr)
    {
        mModel->CancelPendingEmit();
        // Note: the model's `BindSources(defer=true)` still
        // installs the source subscriptions and latches
        // `mDeferredBindPending`; `showEvent` calls
        // `PumpDeferredBind()` to run the walk against the
        // accumulated state on reveal.
        const bool deferRebuild = !isVisible();
        mModel->BindSources(context.model, context.anchors, deferRebuild);
        mDeferredRebuildOnShow = deferRebuild;
    }

    RestoreStateFromSession(incoming);

    mBoundSession = incoming;
}

void HistogramDock::Unbind()
{
    Bind(SessionBindContext::MakeUnbound());
}

void HistogramDock::SaveStateIntoBoundSession()
{
    if (mBoundSession.isNull() || mModel == nullptr)
    {
        return;
    }
    auto &state = mBoundSession->MutableHistogramState();
    // Honest pin: only persist the rung as "pinned" when the user
    // actually chose it via `SetBucketSize`; an auto-picked rung
    // should re-auto-pick against the incoming session's time
    // range on re-Bind. Reading `HistogramModel::IsBucketSizePinned`
    // is the only truthful source (`Index().BucketSize()` returns
    // the same enum whether it was auto-picked or user-chosen).
    // Origin-review fix (finding M1): the prior implementation
    // pinned every non-default rung including auto-picked ones,
    // so a single tab round-trip stuck a session on whichever rung
    // it happened to auto-pick.
    if (mModel->IsBucketSizePinned())
    {
        state.bucketSizePinned = true;
        state.bucketSize = static_cast<std::uint8_t>(mModel->Index().BucketSize());
    }
    else
    {
        state.bucketSizePinned = false;
        state.bucketSize.reset();
    }
}

void HistogramDock::RestoreStateFromSession(LogSession *session)
{
    if (session == nullptr || mModel == nullptr)
    {
        return;
    }
    const auto &state = session->HistogramState();
    if (!state.bucketSizePinned || !state.bucketSize.has_value())
    {
        return;
    }
    // Cast back to the loglib enum. Guard against a hypothetical
    // out-of-range value from a corrupted state store.
    const auto raw = state.bucketSize.value();
    switch (static_cast<loglib::HistogramBucketSize>(raw))
    {
    case loglib::HistogramBucketSize::OneSecond:
    case loglib::HistogramBucketSize::TenSeconds:
    case loglib::HistogramBucketSize::OneMinute:
    case loglib::HistogramBucketSize::TenMinutes:
    case loglib::HistogramBucketSize::OneHour:
    case loglib::HistogramBucketSize::OneDay:
        mModel->SetBucketSize(static_cast<loglib::HistogramBucketSize>(raw));
        break;
    }
}

void HistogramDock::closeEvent(QCloseEvent *event)
{
    // Mirror `AnchorsDock::closeEvent`: let the base class run first
    // so `closed()` fires only when the close actually goes through
    // (not on a vetoed close, which would leave the toggle out of
    // sync with a still-visible dock).
    QDockWidget::closeEvent(event);
    if (event->isAccepted())
    {
        emit closed();
    }
}

void HistogramDock::showEvent(QShowEvent *event)
{
    QDockWidget::showEvent(event);
    if (mDeferredRebuildOnShow && mModel != nullptr)
    {
        mDeferredRebuildOnShow = false;
        // Pump the model's deferred `Rebuild` + `ApplyAutoBucketSize`
        // that `BindSources(..., deferRebuild=true)` skipped. Not a
        // bare `Rebuild()` -- the auto-pick needs to run against the
        // freshly-accumulated range, otherwise the strip renders at
        // whatever rung the outgoing session had (or the default if
        // fresh).
        mModel->PumpDeferredBind();
    }
}
