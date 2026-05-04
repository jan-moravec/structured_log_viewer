#include "loglib/internal/buffering_sink.hpp"

#include "loglib/log_file.hpp"

#include <iterator>
#include <utility>

namespace loglib::internal
{

BufferingSink::BufferingSink(std::unique_ptr<FileLineSource> source)
    : mSource(std::move(source))
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
    // Avoid `reserve(size + n)`: some STL impls take it as exact and
    // make this O(N^2/B). `insert` grows geometrically.
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
    // Flush line offsets into the LogFile before moving the source
    // into LogData so `GetLine(i)` works on the returned data.
    if (mSource && !mLineOffsets.empty())
    {
        mSource->File().AppendLineOffsets(mLineOffsets);
        mLineOffsets.clear();
    }
    return LogData(std::move(mSource), std::move(mLines), std::move(mKeys));
}

std::vector<std::string> BufferingSink::TakeErrors()
{
    return std::move(mErrors);
}

} // namespace loglib::internal
