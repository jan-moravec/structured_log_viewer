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
    Q_ASSERT(QThread::currentThread() == thread());

    // Drain the paused buffer **before** clearing `mPaused` so the worker
    // never observes the unpaused state with batches still sitting in the
    // buffer. Otherwise the following sequence would invert ordering:
    //   1. GUI: `mPaused.exchange(false)` (older code path)
    //   2. Worker: `OnBatch` reads `mPaused == false`, skips the buffer,
    //      posts batch_N+1 via `QueuedConnection`.
    //   3. GUI: `invokeMethod(... Qt::QueuedConnection)` posts the
    //      coalesced paused batch — *behind* batch_N+1 in the queue.
    //   4. Event loop runs batch_N+1 first, then the older paused rows.
    // Since `Resume()` already runs on the GUI thread, deliver the
    // coalesced batch synchronously: there is no other GUI-thread work
    // racing with it, and any worker batches posted after `mPaused` is
    // cleared land in the Qt queue *after* this synchronous call returns
    // — preserving FIFO order without the QueuedConnection round-trip.
    std::vector<loglib::StreamedBatch> drained;
    {
        std::lock_guard<std::mutex> lock(mPausedMutex);
        if (!mPaused.load(std::memory_order_acquire))
        {
            return; // already running
        }
        drained.swap(mPausedBatches);
        // Clear under the lock so a concurrent `OnBatch` either appended
        // its batch to the (now-emptied) buffer before we swapped — in
        // which case we already own it — or it sees `mPaused == false`
        // on its next check and posts via QueuedConnection (correctly
        // ordered after this synchronous AppendBatch).
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
    // Walk the buffer head-to-tail dropping whole batches, then trimming
    // the prefix of the first surviving batch if it is a pure live-tail
    // (`streamLines`-only) batch. Static-path batches (`lines` +
    // `localLineOffsets`) are treated atomically: `localLineOffsets` may
    // be longer than `lines` (the pipeline accumulates offsets from
    // all-error chunks past the line count), so partially trimming would
    // break the parser invariant `LogTable::AppendBatch` relies on.
    auto it = mPausedBatches.begin();
    while (it != mPausedBatches.end() && toDrop > 0)
    {
        const size_t rows = it->lines.size() + it->streamLines.size();
        const bool hasStaticContent = !it->lines.empty() || !it->localLineOffsets.empty();
        if (rows <= toDrop || hasStaticContent)
        {
            toDrop = (toDrop > rows) ? toDrop - rows : 0;
            ++it;
        }
        else
        {
            it->streamLines.erase(
                it->streamLines.begin(), it->streamLines.begin() + static_cast<std::ptrdiff_t>(toDrop)
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
                    // Accumulate the *actual* number of lines evicted by
                    // the loop below so the counter ticks correctly on
                    // both branches. A snapshot of `toDrop` up-front would
                    // under-report on the whole-batch-erased branch when
                    // the head is a static-content batch with more rows
                    // than `toDrop` requires: that batch is evicted
                    // atomically (its `localLineOffsets` may exceed
                    // `lines`, so partial-prefix trimming would break the
                    // invariant `LogTable::AppendBatch` relies on), so the
                    // real drop count is `rows`, not `toDrop`. The PRD
                    // 4.2.2.iv "dropped while paused" indicator must
                    // surface that overshoot.
                    size_t linesDropped = 0;
                    // Same partial-trim caveat as `TrimPausedBufferTo`:
                    // static-path batches are evicted whole (their
                    // `localLineOffsets` may be longer than `lines`), so
                    // partial-prefix trimming is only safe on pure
                    // live-tail batches.
                    auto it = mPausedBatches.begin();
                    while (it != mPausedBatches.end() && toDrop > 0)
                    {
                        const size_t rows = it->lines.size() + it->streamLines.size();
                        const bool hasStaticContent = !it->lines.empty() || !it->localLineOffsets.empty();
                        if (rows <= toDrop || hasStaticContent)
                        {
                            // Whole batch evicted — count `rows`, which
                            // may exceed the remaining `toDrop` for
                            // atomic static-content batches.
                            linesDropped += rows;
                            toDrop = (toDrop > rows) ? toDrop - rows : 0;
                            ++it;
                        }
                        else
                        {
                            it->streamLines.erase(
                                it->streamLines.begin(), it->streamLines.begin() + static_cast<std::ptrdiff_t>(toDrop)
                            );
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
    // Pre-size so a long paused tail does not thrash on `push_back`. We
    // must coalesce **every** field of `StreamedBatch` — the static-
    // streaming path (`JsonParser::ParseStreaming(LogFile&, ...)`) emits
    // batches whose row payload lives in `lines` + `localLineOffsets`,
    // not in `streamLines`. The Pause action is reachable from the
    // **Stream** menu during a static parse (the toolbar is hidden but
    // the menu items stay enabled), so dropping `lines`/`localLineOffsets`
    // here would silently lose every row buffered while paused.
    size_t lineRows = 0;
    size_t streamLineRows = 0;
    size_t lineOffsetCount = 0;
    size_t errorCount = 0;
    size_t newKeyCount = 0;
    for (const auto &batch : batches)
    {
        lineRows += batch.lines.size();
        streamLineRows += batch.streamLines.size();
        lineOffsetCount += batch.localLineOffsets.size();
        errorCount += batch.errors.size();
        newKeyCount += batch.newKeys.size();
    }
    out.lines.reserve(lineRows);
    out.streamLines.reserve(streamLineRows);
    out.localLineOffsets.reserve(lineOffsetCount);
    out.errors.reserve(errorCount);
    out.newKeys.reserve(newKeyCount);
    out.firstLineNumber = batches.front().firstLineNumber;
    for (auto &batch : batches)
    {
        std::move(batch.lines.begin(), batch.lines.end(), std::back_inserter(out.lines));
        std::move(batch.streamLines.begin(), batch.streamLines.end(), std::back_inserter(out.streamLines));
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
    // Both row payloads count toward the buffered total — see
    // `CoalesceLocked` for why static-path `lines` matter here.
    size_t total = 0;
    for (const auto &batch : mPausedBatches)
    {
        total += batch.lines.size() + batch.streamLines.size();
    }
    return total;
}
