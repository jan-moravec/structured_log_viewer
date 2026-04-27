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
    // Bumping the generation drops any still-queued OnBatch from a prior parse
    // before the fresh stop source is installed.
    mGeneration.fetch_add(1, std::memory_order_acq_rel);
    mStopSource.emplace();
    return mStopSource->get_token();
}

void QtStreamingLogSink::RequestStop()
{
    // Bump generation before requesting stop so any in-flight OnBatch drops.
    mGeneration.fetch_add(1, std::memory_order_acq_rel);
    if (mStopSource.has_value())
    {
        mStopSource->request_stop();
    }
}

loglib::KeyIndex &QtStreamingLogSink::Keys()
{
    return mModel->Table().Data().Keys();
}

void QtStreamingLogSink::OnStarted()
{
    const uint64_t gen = mGeneration.load(std::memory_order_acquire);
    QMetaObject::invokeMethod(
        this,
        [this, gen]() {
            if (mGeneration.load(std::memory_order_acquire) != gen || !mModel)
            {
                return;
            }
        },
        Qt::QueuedConnection
    );
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
