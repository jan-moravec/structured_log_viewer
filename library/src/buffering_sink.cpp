#include "buffering_sink.hpp"

#include <iterator>
#include <utility>

namespace loglib
{

BufferingSink::BufferingSink(std::unique_ptr<LogFile> logFile) : mFile(std::move(logFile))
{
}

void BufferingSink::OnStarted()
{
    mLines.clear();
    mLineOffsets.clear();
    mErrors.clear();
    mFinished = false;
}

void BufferingSink::OnBatch(StreamedBatch batch)
{
    // Stage B/C built the batch's lines against this sink's mKeys (the parser
    // borrows it via Keys()) and Stage C already populated newKeys from the
    // same canonical index, so we can splice everything in without rebinding
    // back-pointers or remapping KeyIds. We deliberately ignore newKeys here:
    // the buffered final LogData re-derives the full key set from mKeys in
    // TakeData(), so any per-batch slice would be redundant for this consumer
    // (Qt-side sinks use newKeys for incremental column extension instead).
    //
    // Important: PRD §4.8 / parser-perf task 9.0 — do *not* call
    // `mLines.reserve(mLines.size() + batch.lines.size())` here. Both libstdc++
    // and the MSVC STL implement `vector::reserve(n)` as "grow capacity to
    // exactly `n`", which means a per-batch `reserve` re-allocates the whole
    // vector every batch and the buffered legacy `Parse(path)` path turns into
    // O(N²/B) — on the `[wide]` 1 M-line / 700-batch fixture that's
    // ~1.4 billion `LogLine` moves, easily 7+ seconds of Stage C wall time.
    // `vector::insert(end, first, last)` is range-aware and grows the
    // capacity geometrically (same factor as `push_back`), keeping the total
    // cost amortised O(N).
    if (!batch.lines.empty())
    {
        mLines.insert(
            mLines.end(),
            std::make_move_iterator(batch.lines.begin()),
            std::make_move_iterator(batch.lines.end())
        );
    }
    if (!batch.localLineOffsets.empty())
    {
        mLineOffsets.insert(
            mLineOffsets.end(),
            std::make_move_iterator(batch.localLineOffsets.begin()),
            std::make_move_iterator(batch.localLineOffsets.end())
        );
    }
    if (!batch.errors.empty())
    {
        mErrors.insert(
            mErrors.end(),
            std::make_move_iterator(batch.errors.begin()),
            std::make_move_iterator(batch.errors.end())
        );
    }
}

void BufferingSink::OnFinished(bool /*cancelled*/)
{
    mFinished = true;
}

KeyIndex &BufferingSink::Keys()
{
    return mKeys;
}

LogData BufferingSink::TakeData()
{
    // Move the line offsets into the LogFile so GetLine(i) keeps working on the
    // returned LogData. Order matters: AppendLineOffsets walks the file's
    // existing offsets, so do this before the file is moved into LogData.
    if (mFile && !mLineOffsets.empty())
    {
        mFile->AppendLineOffsets(mLineOffsets);
        mLineOffsets.clear();
    }
    return LogData(std::move(mFile), std::move(mLines), std::move(mKeys));
}

std::vector<std::string> BufferingSink::TakeErrors()
{
    return std::move(mErrors);
}

} // namespace loglib
