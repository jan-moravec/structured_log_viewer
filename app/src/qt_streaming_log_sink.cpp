#include "qt_streaming_log_sink.hpp"

#include "log_model.hpp"

#include <loglib/log_data.hpp>
#include <loglib/log_table.hpp>

#include <QMetaObject>
#include <QThread>

#include <algorithm>
#include <iterator>
#include <memory>
#include <utility>

QtStreamingLogSink::QtStreamingLogSink(LogModel *model, QObject *parent) : QObject(parent), mModel(model)
{
}

loglib::StopToken QtStreamingLogSink::Arm()
{
    // Bump drops any still-queued OnBatch from a prior parse.
    mGeneration.fetch_add(1, std::memory_order_acq_rel);
    mStopSource.emplace();
    {
        std::lock_guard<std::mutex> lock(mPausedMutex);
        mPausedBatches.clear();
    }
    mPaused.store(false, std::memory_order_release);
    mPausedDropCount.store(0, std::memory_order_release);
    mActive = true;
    return mStopSource->get_token();
}

void QtStreamingLogSink::RequestStop()
{
    // Cooperative only. Bumping the generation here would let drain-phase
    // OnBatch/OnFinished pass the mismatch check; do that in
    // `DropPendingBatches` after the worker has joined.
    if (mStopSource.has_value())
    {
        mStopSource->request_stop();
    }
}

void QtStreamingLogSink::DropPendingBatches()
{
    // Caller must have joined the worker first so no further callbacks
    // can capture the new generation.
    mGeneration.fetch_add(1, std::memory_order_acq_rel);
    {
        std::lock_guard<std::mutex> lock(mPausedMutex);
        mPausedBatches.clear();
    }
    mPaused.store(false, std::memory_order_release);
    mActive = false;
}

bool QtStreamingLogSink::IsActive() const noexcept
{
    return mActive;
}

void QtStreamingLogSink::Pause() noexcept
{
    mPaused.store(true, std::memory_order_release);
}

void QtStreamingLogSink::Resume()
{
    Q_ASSERT(QThread::currentThread() == thread());

    // Swap the buffer and clear `mPaused` under one lock so a
    // concurrent `OnBatch` either lands in the buffer we just took
    // ownership of, or sees the cleared flag and posts via
    // QueuedConnection — which lands after the synchronous AppendBatch
    // below, preserving FIFO order.
    std::vector<loglib::StreamedBatch> drained;
    {
        std::lock_guard<std::mutex> lock(mPausedMutex);
        if (!mPaused.load(std::memory_order_acquire))
        {
            return;
        }
        drained.swap(mPausedBatches);
        mPaused.store(false, std::memory_order_release);
    }

    if (drained.empty() || !mModel)
    {
        return;
    }
    mModel->AppendBatch(CoalesceLocked(std::move(drained)));
}

bool QtStreamingLogSink::IsPaused() const noexcept
{
    return mPaused.load(std::memory_order_acquire);
}

size_t QtStreamingLogSink::PausedLineCount() const
{
    std::lock_guard<std::mutex> lock(mPausedMutex);
    return PausedLineCountLocked();
}

uint64_t QtStreamingLogSink::PausedDropCount() const noexcept
{
    return mPausedDropCount.load(std::memory_order_acquire);
}

void QtStreamingLogSink::SetRetentionCap(size_t cap) noexcept
{
    mRetentionCap.store(cap, std::memory_order_release);
}

void QtStreamingLogSink::TrimPausedBufferTo(size_t maxBufferedLines)
{
    std::lock_guard<std::mutex> lock(mPausedMutex);
    size_t total = PausedLineCountLocked();
    if (total <= maxBufferedLines)
    {
        return;
    }
    size_t toDrop = total - maxBufferedLines;
    // Drop whole batches, then trim the prefix of the first survivor.
    // Static-path batches (with `localLineOffsets`) are dropped whole
    // because their offsets may exceed `lines.size()`; partial trimming
    // would break `LogTable::AppendBatch`'s invariant.
    auto it = mPausedBatches.begin();
    while (it != mPausedBatches.end() && toDrop > 0)
    {
        const size_t rows = it->lines.size();
        const bool hasStaticContent = !it->localLineOffsets.empty();
        if (rows <= toDrop || hasStaticContent)
        {
            toDrop = (toDrop > rows) ? toDrop - rows : 0;
            ++it;
        }
        else
        {
            it->lines.erase(it->lines.begin(), it->lines.begin() + static_cast<std::ptrdiff_t>(toDrop));
            it->firstLineNumber += toDrop;
            toDrop = 0;
        }
    }
    if (it != mPausedBatches.begin())
    {
        mPausedBatches.erase(mPausedBatches.begin(), it);
    }
}

std::optional<loglib::StreamedBatch> QtStreamingLogSink::TakePausedBuffer()
{
    std::vector<loglib::StreamedBatch> drained;
    {
        std::lock_guard<std::mutex> lock(mPausedMutex);
        drained.swap(mPausedBatches);
    }
    if (drained.empty())
    {
        return std::nullopt;
    }
    return CoalesceLocked(std::move(drained));
}

loglib::KeyIndex &QtStreamingLogSink::Keys()
{
    return mModel->Table().Keys();
}

void QtStreamingLogSink::OnStarted()
{
    // No-op: the GUI's streaming state is armed synchronously by
    // `LogModel::BeginStreaming` before the worker is spawned.
}

void QtStreamingLogSink::OnBatch(loglib::StreamedBatch batch)
{
    // While paused, route batches into the paused buffer instead of
    // posting them across threads, so the Qt event queue does not
    // become a third unbounded memory pool.
    bool routedToPausedBuffer = false;
    if (mPaused.load(std::memory_order_acquire))
    {
        const size_t cap = mRetentionCap.load(std::memory_order_acquire);
        std::lock_guard<std::mutex> lock(mPausedMutex);
        // Re-check under the lock; a concurrent Resume could have
        // already swapped the buffer out.
        if (mPaused.load(std::memory_order_acquire))
        {
            mPausedBatches.push_back(std::move(batch));
            // Bound the paused buffer alone to `cap`. The combined
            // visible+buffered budget is enforced on Resume by
            // `LogModel::AppendBatch`'s FIFO eviction.
            if (cap != 0)
            {
                size_t total = PausedLineCountLocked();
                if (total > cap)
                {
                    size_t toDrop = total - cap;
                    // Count actual evicted lines, not `toDrop`: a
                    // static-content batch is evicted whole and may
                    // overshoot.
                    size_t linesDropped = 0;
                    auto it = mPausedBatches.begin();
                    while (it != mPausedBatches.end() && toDrop > 0)
                    {
                        const size_t rows = it->lines.size();
                        const bool hasStaticContent = !it->localLineOffsets.empty();
                        if (rows <= toDrop || hasStaticContent)
                        {
                            linesDropped += rows;
                            toDrop = (toDrop > rows) ? toDrop - rows : 0;
                            ++it;
                        }
                        else
                        {
                            it->lines.erase(it->lines.begin(), it->lines.begin() + static_cast<std::ptrdiff_t>(toDrop));
                            it->firstLineNumber += toDrop;
                            linesDropped += toDrop;
                            toDrop = 0;
                        }
                    }
                    if (it != mPausedBatches.begin())
                    {
                        mPausedBatches.erase(mPausedBatches.begin(), it);
                    }
                    mPausedDropCount.fetch_add(linesDropped, std::memory_order_acq_rel);
                }
            }
            routedToPausedBuffer = true;
        }
    }
    if (routedToPausedBuffer)
    {
        return;
    }

    const uint64_t gen = mGeneration.load(std::memory_order_acquire);
    // Qt's queued-connection storage requires CopyConstructible; wrap
    // the move-only batch in shared_ptr.
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
            mActive = false;
            mModel->EndStreaming(cancelled);
        },
        Qt::QueuedConnection
    );
}

loglib::StreamedBatch QtStreamingLogSink::CoalesceLocked(std::vector<loglib::StreamedBatch> &&batches)
{
    loglib::StreamedBatch out;
    if (batches.empty())
    {
        return out;
    }
    // Pre-size so a long paused tail does not thrash on `push_back`.
    size_t lineRows = 0;
    size_t lineOffsetCount = 0;
    size_t errorCount = 0;
    size_t newKeyCount = 0;
    for (const auto &batch : batches)
    {
        lineRows += batch.lines.size();
        lineOffsetCount += batch.localLineOffsets.size();
        errorCount += batch.errors.size();
        newKeyCount += batch.newKeys.size();
    }
    out.lines.reserve(lineRows);
    out.localLineOffsets.reserve(lineOffsetCount);
    out.errors.reserve(errorCount);
    out.newKeys.reserve(newKeyCount);
    out.firstLineNumber = batches.front().firstLineNumber;
    for (auto &batch : batches)
    {
        std::move(batch.lines.begin(), batch.lines.end(), std::back_inserter(out.lines));
        std::move(
            batch.localLineOffsets.begin(), batch.localLineOffsets.end(), std::back_inserter(out.localLineOffsets)
        );
        std::move(batch.errors.begin(), batch.errors.end(), std::back_inserter(out.errors));
        std::move(batch.newKeys.begin(), batch.newKeys.end(), std::back_inserter(out.newKeys));
    }
    return out;
}

size_t QtStreamingLogSink::PausedLineCountLocked() const
{
    size_t total = 0;
    for (const auto &batch : mPausedBatches)
    {
        total += batch.lines.size();
    }
    return total;
}
