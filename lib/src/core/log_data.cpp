#include "loglib/core/log_data.hpp"

#include "loglib/core/compact_log_value.hpp"
#include "loglib/core/file_line_source.hpp"

#include <algorithm>
#include <cassert>
#include <iterator>
#include <ranges>
#include <utility>
#include <vector>

namespace loglib
{

LogData::LogData() = default;

LogData::LogData(std::unique_ptr<LineSource> source, std::vector<LogLine> lines, KeyIndex keys)
    : mLines(std::move(lines)), mKeys(std::move(keys))
{
    if (source != nullptr)
    {
        mSources.push_back(std::move(source));
    }
}

const std::vector<std::unique_ptr<LineSource>> &LogData::Sources() const noexcept
{
    return mSources;
}

std::vector<std::unique_ptr<LineSource>> &LogData::Sources() noexcept
{
    return mSources;
}

FileLineSource *LogData::FrontFileSource() noexcept
{
    if (mSources.empty())
    {
        return nullptr;
    }
    return dynamic_cast<FileLineSource *>(mSources.front().get());
}

const FileLineSource *LogData::FrontFileSource() const noexcept
{
    if (mSources.empty())
    {
        return nullptr;
    }
    return dynamic_cast<const FileLineSource *>(mSources.front().get());
}

FileLineSource *LogData::BackFileSource() noexcept
{
    for (auto &source : std::views::reverse(mSources))
    {
        if (auto *fs = dynamic_cast<FileLineSource *>(source.get()); fs != nullptr)
        {
            return fs;
        }
    }
    return nullptr;
}

const FileLineSource *LogData::BackFileSource() const noexcept
{
    for (const auto &source : std::views::reverse(mSources))
    {
        if (const auto *fs = dynamic_cast<const FileLineSource *>(source.get()); fs != nullptr)
        {
            return fs;
        }
    }
    return nullptr;
}

StreamLineSource *LogData::BackStreamSource() noexcept
{
    for (auto &source : std::views::reverse(mSources))
    {
        if (auto *ss = dynamic_cast<StreamLineSource *>(source.get()); ss != nullptr)
        {
            return ss;
        }
    }
    return nullptr;
}

const StreamLineSource *LogData::BackStreamSource() const noexcept
{
    for (const auto &source : std::views::reverse(mSources))
    {
        if (const auto *ss = dynamic_cast<const StreamLineSource *>(source.get()); ss != nullptr)
        {
            return ss;
        }
    }
    return nullptr;
}

void LogData::EvictPrefix(size_t count)
{
    count = std::min(count, mLines.size() - mHead);
    for (size_t row = mHead; row < mHead + count; ++row)
    {
        mLines[row].ReleaseFields();
    }
    mHead += count;
    // Moving the live rows costs as much as they are many, so wait until the dead rows are as many as the live ones.
    if (mHead != 0 && mHead >= mLines.size() - mHead)
    {
        mLines.erase(mLines.begin(), mLines.begin() + static_cast<std::ptrdiff_t>(mHead));
        mHead = 0;
    }
}

const KeyIndex &LogData::Keys() const
{
    return mKeys;
}

KeyIndex &LogData::Keys()
{
    return mKeys;
}

std::vector<std::string> LogData::SortedKeys() const
{
    return mKeys.SortedKeys();
}

void LogData::AppendBatch(
    std::vector<LogLine> lines,
    const std::vector<uint64_t> &lineOffsets,
    std::span<const MultiLineRecordSpan> multiLineSpans
)
{
    // No per-batch reserve — rely on geometric push_back growth
    // (some STL impls take reserve(size+n) as exact = O(N^2/B)).
    if (!lines.empty())
    {
        for (auto &line : lines)
        {
            mLines.push_back(std::move(line));
        }
    }

    if (!lineOffsets.empty() || !multiLineSpans.empty())
    {
        // File batches belong to the most recently appended file source.
        FileLineSource *fileSource = BackFileSource();
        assert(fileSource != nullptr);
        if (fileSource != nullptr)
        {
            if (!lineOffsets.empty())
            {
                fileSource->AppendLineOffsets(lineOffsets);
            }
            // Spans depend on offsets from the same batch.
            for (const auto &span : multiLineSpans)
            {
                fileSource->RegisterMultiLineRecord(span.headerLineId, span.lastLineId);
            }
        }
    }
}

} // namespace loglib
