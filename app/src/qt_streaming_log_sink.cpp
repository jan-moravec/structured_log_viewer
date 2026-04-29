#include "qt_streaming_log_sink.hpp"

#include "log_model.hpp"

#include <loglib/log_data.hpp>
#include <loglib/log_table.hpp>

#include <QMetaObject>

#include <memory>
#include <utility>

QtStreamingLogSink::QtStreamingLogSink(LogModel *model, QObject *parent) : QObject(parent), mModel(model)
{
}

loglib::StopToken QtStreamingLogSink::Arm()
{
    // Bumping the generation drops any still-queued OnBatch from a prior parse
    // before the fresh stop source is installed.
    mGeneration.fetch_add(1, std::memory_order_acq_rel);
    mStopSource.emplace();
    return mStopSource->get_token();
}

void QtStreamingLogSink::RequestStop()
{
    // Cooperative cancel only — do **not** touch the generation here. The
    // worker keeps draining for a while after this returns (Stage B parallel
    // tasks already in flight, plus Stage C's tail flush + OnFinished), and
    // every OnBatch / OnFinished it makes during that window reads
    // `mGeneration` to embed in its queued lambda. If we bumped here, those
    // reads would see the bumped value (the stop_token's request_stop has
    // release semantics paired with the worker's acquire load, so the bump
    // would be guaranteed visible by the time the worker observes the stop)
    // and the queued lambdas would then *pass* the generation-mismatch check
    // on the GUI thread — running `AppendBatch` / `EndStreaming` after the
    // caller has already torn the model state down (use-after-free on the
    // unmapped `LogFile`, plus a spurious second `streamingFinished`). The
    // bump belongs on the *post-wait* side, see `DropPendingBatches`.
    if (mStopSource.has_value())
    {
        mStopSource->request_stop();
    }
}

void QtStreamingLogSink::DropPendingBatches()
{
    // Caller must have already joined the worker (typically via
    // `QFutureWatcher::waitForFinished()`), so no further OnBatch /
    // OnFinished call can race this bump and capture the new generation.
    mGeneration.fetch_add(1, std::memory_order_acq_rel);
}

loglib::KeyIndex &QtStreamingLogSink::Keys()
{
    return mModel->Table().Keys();
}

void QtStreamingLogSink::OnStarted()
{
    // Intentionally a no-op: the streaming UI state (status-bar label,
    // configuration-menu gating, `mStreamingActive` flag) is set
    // synchronously from `LogModel::BeginStreaming` on the GUI thread,
    // *before* the worker is spawned, so the GUI side never needs to
    // observe `OnStarted` to pick up the transition. Posting a queued
    // empty lambda from this hot-path callback would just add scheduler
    // wakeup churn to the GUI's event loop for no observable side
    // effect.
    //
    // Kept as an explicit override (rather than relying on the default
    // `StreamingLogSink::OnStarted` no-op) so that wiring it to a future
    // `LogModel::parseStarted` signal is a one-line change should we
    // ever want a worker-arrival-time hook on the GUI side.
}

void QtStreamingLogSink::OnBatch(loglib::StreamedBatch batch)
{
    const uint64_t gen = mGeneration.load(std::memory_order_acquire);
    // Qt's queued-connection storage requires a CopyConstructible callable;
    // wrap the move-only batch in shared_ptr to satisfy that without copying.
    auto batchPtr = std::make_shared<loglib::StreamedBatch>(std::move(batch));
    QMetaObject::invokeMethod(
        this,
        [this, gen, batchPtr]() {
            if (mGeneration.load(std::memory_order_acquire) != gen || !mModel)
            {
                return;
            }
            mModel->AppendBatch(std::move(*batchPtr));
        },
        Qt::QueuedConnection
    );
}

void QtStreamingLogSink::OnFinished(bool cancelled)
{
    const uint64_t gen = mGeneration.load(std::memory_order_acquire);
    QMetaObject::invokeMethod(
        this,
        [this, gen, cancelled]() {
            if (mGeneration.load(std::memory_order_acquire) != gen || !mModel)
            {
                return;
            }
            mModel->EndStreaming(cancelled);
        },
        Qt::QueuedConnection
    );
}
