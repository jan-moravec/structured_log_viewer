#include "buffering_sink.hpp"

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
    if (!batch.lines.empty())
    {
        mLines.reserve(mLines.size() + batch.lines.size());
        std::move(batch.lines.begin(), batch.lines.end(), std::back_inserter(mLines));
    }
    if (!batch.localLineOffsets.empty())
    {
        mLineOffsets.reserve(mLineOffsets.size() + batch.localLineOffsets.size());
        std::move(
            batch.localLineOffsets.begin(),
            batch.localLineOffsets.end(),
            std::back_inserter(mLineOffsets)
        );
    }
    if (!batch.errors.empty())
    {
        mErrors.reserve(mErrors.size() + batch.errors.size());
        std::move(batch.errors.begin(), batch.errors.end(), std::back_inserter(mErrors));
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
