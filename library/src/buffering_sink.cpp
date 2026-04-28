#include "loglib/internal/buffering_sink.hpp"

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
    // Splice straight into mLines. Don't `reserve(mLines.size() + n)` per
    // batch — MSVC/libstdc++ implement reserve as "grow to exactly n", which
    // would turn the buffered path into O(N²/B). insert() grows geometrically.
    if (!batch.lines.empty())
    {
        mLines.insert(
            mLines.end(), std::make_move_iterator(batch.lines.begin()), std::make_move_iterator(batch.lines.end())
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
            mErrors.end(), std::make_move_iterator(batch.errors.begin()), std::make_move_iterator(batch.errors.end())
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
    // Push line offsets into the LogFile before it moves into LogData,
    // so GetLine(i) keeps working on the returned data.
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
