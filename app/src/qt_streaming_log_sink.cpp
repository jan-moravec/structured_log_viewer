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

std::stop_token QtStreamingLogSink::BeginParse()
{
    // GUI-thread only. Bumping the generation first means anything still queued
    // from the previous parse — even an OnFinished that has not yet run — is
    // dropped by the receiver-side check. Only after that do we install the new
    // stop source so the parser sees a fresh, unstopped token.
    mGeneration.fetch_add(1, std::memory_order_acq_rel);
    mStopSource.emplace();
    return mStopSource->get_token();
}

void QtStreamingLogSink::RequestStop()
{
    // GUI-thread only. Order matters: bump the generation FIRST so the queued
    // OnBatch calls already in the event loop are dropped on receipt, then ask
    // the parser to stop. Any in-flight batches still emitted before the parser
    // notices the stop request dissolve into the same generation-dropped sink.
    mGeneration.fetch_add(1, std::memory_order_acq_rel);
    if (mStopSource.has_value())
    {
        mStopSource->request_stop();
    }
}

loglib::KeyIndex &QtStreamingLogSink::Keys()
{
    // Routed straight through to the model's canonical LogData KeyIndex. Safe
    // to access from any thread for the duration of a parse (the model never
    // replaces its LogTable mid-parse — Clear() goes through RequestStop first).
    return mModel->Table().Data().Keys();
}

void QtStreamingLogSink::OnStarted()
{
    // Capture the live generation at queue time so a stop request that lands
    // between this call and GUI-thread delivery still drops the event.
    const uint64_t gen = mGeneration.load(std::memory_order_acquire);
    QMetaObject::invokeMethod(
        this,
        [this, gen]() {
            if (mGeneration.load(std::memory_order_acquire) != gen)
            {
                return;
            }
            if (!mModel)
            {
                return;
            }
            // No-op on the GUI side for now; LogModel::BeginStreaming has
            // already initialised the table. Reserved for future progress-UI
            // that wants a dedicated "started" event.
        },
        Qt::QueuedConnection
    );
}

void QtStreamingLogSink::OnBatch(loglib::StreamedBatch batch)
{
    const uint64_t gen = mGeneration.load(std::memory_order_acquire);
    // StreamedBatch is move-only (LogLine is non-copyable), so we cannot let
    // QMetaObject::invokeMethod copy it into its queued-connection storage.
    // Wrap in a shared_ptr so the lambda — which is required to be CopyConstructible
    // by Qt's queued-invocation machinery — can carry the batch to the GUI thread
    // without copying its contents.
    auto batchPtr = std::make_shared<loglib::StreamedBatch>(std::move(batch));
    QMetaObject::invokeMethod(
        this,
        [this, gen, batchPtr]() {
            if (mGeneration.load(std::memory_order_acquire) != gen)
            {
                return;
            }
            if (!mModel)
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
            if (mGeneration.load(std::memory_order_acquire) != gen)
            {
                return;
            }
            if (!mModel)
            {
                return;
            }
            mModel->EndStreaming(cancelled);
        },
        Qt::QueuedConnection
    );
}
