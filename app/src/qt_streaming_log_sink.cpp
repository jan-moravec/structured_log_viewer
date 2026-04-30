#include "qt_streaming_log_sink.hpp"

#include "log_model.hpp"

#include <loglib/log_data.hpp>
#include <loglib/log_table.hpp>

#include <QMetaObject>

#include <algorithm>
#include <iterator>
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
    if (!mPaused.exchange(false, std::memory_order_acq_rel))
    {
        return; // already running
    }

    std::vector<loglib::StreamedBatch> drained;
    {
        std::lock_guard<std::mutex> lock(mPausedMutex);
        drained.swap(mPausedBatches);
    }
    if (drained.empty())
    {
        return;
    }

    auto coalesced = std::make_shared<loglib::StreamedBatch>(CoalesceLocked(std::move(drained)));
    const uint64_t gen = mGeneration.load(std::memory_order_acquire);
    QMetaObject::invokeMethod(
        this,
        [this, gen, coalesced]() {
            if (mGeneration.load(std::memory_order_acquire) != gen || !mModel)
            {
                return;
            }
            mModel->AppendBatch(std::move(*coalesced));
        },
        Qt::QueuedConnection
    );
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
    // Walk the buffer head-to-tail dropping whole batches, then trimming
    // the prefix of the first surviving batch if needed.
    auto it = mPausedBatches.begin();
    while (it != mPausedBatches.end() && toDrop > 0)
    {
        const size_t rows = it->streamLines.size();
        if (rows <= toDrop)
        {
            toDrop -= rows;
            ++it;
        }
        else
        {
            it->streamLines.erase(it->streamLines.begin(), it->streamLines.begin() + static_cast<std::ptrdiff_t>(toDrop)
            );
            // Shift the batch's start cursor forward so its `firstLineNumber`
            // still names a valid line for downstream consumers.
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
    // Intentional no-op: the GUI's streaming state is armed synchronously by
    // `LogModel::BeginStreaming` before the worker is spawned.
}

void QtStreamingLogSink::OnBatch(loglib::StreamedBatch batch)
{
    // Worker-side Pause check (PRD 4.2.2.v / 4.10.1): redirect parsed
    // batches into the paused buffer rather than posting per-batch
    // `Qt::QueuedConnection` lambdas so the Qt event-queue doesn't
    // accumulate as a third unbounded memory pool invisible to
    // `K buffered`.
    bool routedToPausedBuffer = false;
    if (mPaused.load(std::memory_order_acquire))
    {
        const size_t cap = mRetentionCap.load(std::memory_order_acquire);
        std::lock_guard<std::mutex> lock(mPausedMutex);
        // Re-check under the lock so a concurrent Resume that swapped the
        // buffer out cannot race us into a now-orphaned vector.
        if (mPaused.load(std::memory_order_acquire))
        {
            mPausedBatches.push_back(std::move(batch));
            // Keep the buffered + visible total within `cap` (PRD 4.2.2.iv).
            // The visible row count is the model's, but we cannot read it
            // from the worker thread; pessimistically bound the buffer alone
            // to `cap`. The visible count is recouped on Resume by FIFO
            // eviction in `LogModel::AppendBatch` (PRD 4.10.3). This is the
            // "paused buffer drops oldest" half of 4.2.2.iv; the GUI side
            // additionally calls `TrimPausedBufferTo` from
            // `LogModel::SetRetentionCap` to trim against the *combined*
            // visible+buffered budget per 4.5.5.ii.
            if (cap != 0)
            {
                size_t total = PausedLineCountLocked();
                if (total > cap)
                {
                    size_t toDrop = total - cap;
                    auto it = mPausedBatches.begin();
                    while (it != mPausedBatches.end() && toDrop > 0)
                    {
                        const size_t rows = it->streamLines.size();
                        if (rows <= toDrop)
                        {
                            toDrop -= rows;
                            ++it;
                        }
                        else
                        {
                            it->streamLines.erase(
                                it->streamLines.begin(), it->streamLines.begin() + static_cast<std::ptrdiff_t>(toDrop)
                            );
                            it->firstLineNumber += toDrop;
                            toDrop = 0;
                        }
                    }
                    if (it != mPausedBatches.begin())
                    {
                        const size_t dropped = static_cast<size_t>(std::distance(mPausedBatches.begin(), it));
                        mPausedBatches.erase(mPausedBatches.begin(), it);
                        mPausedDropCount.fetch_add(dropped, std::memory_order_acq_rel);
                    }
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
    size_t streamLineRows = 0;
    size_t errorCount = 0;
    size_t newKeyCount = 0;
    for (const auto &batch : batches)
    {
        streamLineRows += batch.streamLines.size();
        errorCount += batch.errors.size();
        newKeyCount += batch.newKeys.size();
    }
    out.streamLines.reserve(streamLineRows);
    out.errors.reserve(errorCount);
    out.newKeys.reserve(newKeyCount);
    out.firstLineNumber = batches.front().firstLineNumber;
    for (auto &batch : batches)
    {
        std::move(batch.streamLines.begin(), batch.streamLines.end(), std::back_inserter(out.streamLines));
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
        total += batch.streamLines.size();
    }
    return total;
}
