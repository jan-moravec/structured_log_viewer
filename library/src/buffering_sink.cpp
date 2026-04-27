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
    // The pipeline built `batch.lines` against this sink's `mKeys` (borrowed
    // via `Keys()`), so we can splice everything in without rebinding KeyIds.
    // `newKeys` is ignored: `TakeData()` re-derives the full key set from
    // `mKeys`, so a per-batch slice is redundant for this consumer.
    //
    // Important: do *not* `mLines.reserve(mLines.size() + batch.lines.size())`
    // per batch. Both libstdc++ and MSVC STL implement `vector::reserve(n)` as
    // "grow capacity to exactly `n`", so a per-batch reserve re-allocates the
    // whole vector every batch, turning the buffered path into O(N²/B).
    // `vector::insert(end, first, last)` grows capacity geometrically and
    // keeps total cost amortised O(N).
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
