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
    // Bumping drops any still-queued OnBatch from a prior parse before the
    // fresh stop source is installed.
    mGeneration.fetch_add(1, std::memory_order_acq_rel);
    mStopSource.emplace();
    return mStopSource->get_token();
}

void QtStreamingLogSink::RequestStop()
{
    // Cooperative only — bumping the generation here would let drain-phase
    // OnBatch/OnFinished pass the mismatch check (their captures would see
    // the bump) and run after teardown. Bump in `DropPendingBatches` instead.
    if (mStopSource.has_value())
    {
        mStopSource->request_stop();
    }
}

void QtStreamingLogSink::DropPendingBatches()
{
    // Caller must have joined the worker first (e.g. `waitForFinished()`),
    // so no further OnBatch/OnFinished can capture the new generation.
    mGeneration.fetch_add(1, std::memory_order_acq_rel);
}

loglib::KeyIndex &QtStreamingLogSink::Keys()
{
    return mModel->Table().Keys();
}

void QtStreamingLogSink::OnStarted()
{
    // Intentional no-op: the GUI's streaming state is armed synchronously by
    // `LogModel::BeginStreaming` before the worker is spawned.
}

void QtStreamingLogSink::OnBatch(loglib::StreamedBatch batch)
{
    const uint64_t gen = mGeneration.load(std::memory_order_acquire);
    // Qt's queued-connection storage requires CopyConstructible; wrap the
    // move-only batch in shared_ptr.
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
