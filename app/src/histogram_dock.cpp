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
    // Save before swapping sources, then restore the incoming pin
    // after automatic sizing so the explicit pin wins. Keep the
    // outgoing session alias until the swap is complete.

    // Session source objects are stable for a session's lifetime,
    // so a live same-session bind can safely skip the reset and rebuild.
    LogSession *incoming = context.session.data();
    if (!mBoundSession.isNull() && mBoundSession.data() == incoming)
    {
        return;
    }

    SaveStateIntoBoundSession();

    if (mModel != nullptr)
    {
        mModel->CancelPendingEmit();
        // Hidden binds subscribe immediately but defer the full walk.
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

LogSession *HistogramDock::boundSessionForTest() const noexcept
{
    return mBoundSession.data();
}

void HistogramDock::SaveStateIntoBoundSession()
{
    if (mBoundSession.isNull() || mModel == nullptr)
    {
        return;
    }
    auto &state = mBoundSession->MutableHistogramState();
    // Persist only an explicit user pin; automatic choices must be
    // recalculated against the session's current range.
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
        // Include auto-sizing against rows accumulated while hidden.
        mModel->PumpDeferredBind();
    }
}
