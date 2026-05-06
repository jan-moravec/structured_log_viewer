#include "loglib/log_data.hpp"

#include "loglib/file_line_source.hpp"
#include "loglib/internal/compact_log_value.hpp"
#include "loglib/log_file.hpp"

#include <algorithm>
#include <cassert>
#include <iterator>
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

    // Rebind to the canonical KeyIndex we now own.
    for (auto &line : mLines)
    {
        line.RebindKeys(mKeys);
    }
}

LogData::LogData(LogData &&other) noexcept
    : mSources(std::move(other.mSources)),
      mLines(std::move(other.mLines)),
      mKeys(std::move(other.mKeys)),
      mTimestampsAlreadyParsed(other.mTimestampsAlreadyParsed)
{
    // KeyIndex moved address — rebind. `LineSource` heap addresses are
    // stable across the move, so `mSource` pointers stay valid.
    for (auto &line : mLines)
    {
        line.RebindKeys(mKeys);
    }
}

LogData &LogData::operator=(LogData &&other) noexcept
{
    if (this != &other)
    {
        mSources = std::move(other.mSources);
        mLines = std::move(other.mLines);
        mKeys = std::move(other.mKeys);
        mTimestampsAlreadyParsed = other.mTimestampsAlreadyParsed;
        for (auto &line : mLines)
        {
            line.RebindKeys(mKeys);
        }
    }
    return *this;
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
    for (auto it = mSources.rbegin(); it != mSources.rend(); ++it)
    {
        if (auto *fs = dynamic_cast<FileLineSource *>(it->get()); fs != nullptr)
        {
            return fs;
        }
    }
    return nullptr;
}

const FileLineSource *LogData::BackFileSource() const noexcept
{
    for (auto it = mSources.rbegin(); it != mSources.rend(); ++it)
    {
        if (const auto *fs = dynamic_cast<const FileLineSource *>(it->get()); fs != nullptr)
        {
            return fs;
        }
    }
    return nullptr;
}

StreamLineSource *LogData::FrontStreamSource() noexcept
{
    if (mSources.empty())
    {
        return nullptr;
    }
    return dynamic_cast<StreamLineSource *>(mSources.front().get());
}

const StreamLineSource *LogData::FrontStreamSource() const noexcept
{
    if (mSources.empty())
    {
        return nullptr;
    }
    return dynamic_cast<const StreamLineSource *>(mSources.front().get());
}

const std::vector<LogLine> &LogData::Lines() const
{
    return mLines;
}

std::vector<LogLine> &LogData::Lines()
{
    return mLines;
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

bool LogData::TimestampsAlreadyParsed() const
{
    return mTimestampsAlreadyParsed;
}

void LogData::MarkTimestampsParsed()
{
    mTimestampsAlreadyParsed = true;
}

void LogData::Merge(LogData &&other)
{
    // Splice sources first: each `LogLine` already points at a heap
    // `LineSource` inside `other.mSources`; moving the `unique_ptr`s
    // keeps the addresses stable.
    mSources.reserve(mSources.size() + other.mSources.size());
    std::move(
        std::make_move_iterator(other.mSources.begin()),
        std::make_move_iterator(other.mSources.end()),
        std::back_inserter(mSources)
    );

    // Remap table: ids in other.mKeys -> ids in mKeys. O(N) in the merged-in key count.
    const size_t otherKeyCount = other.mKeys.Size();
    std::vector<KeyId> remap(otherKeyCount);
    for (size_t i = 0; i < otherKeyCount; ++i)
    {
        const auto otherId = static_cast<KeyId>(i);
        remap[i] = mKeys.GetOrInsert(other.mKeys.KeyOf(otherId));
    }

    mLines.reserve(mLines.size() + other.mLines.size());
    for (auto &line : other.mLines)
    {
        // Rewire each pair's KeyId in place (no `LogValue`
        // materialisation), then re-sort. `OwnedString` offsets stay
        // valid: the source's arena moved with its bytes.
        const auto values = line.CompactValues();
        std::vector<std::pair<KeyId, internal::CompactLogValue>> remapped;
        remapped.reserve(values.size());
        for (const auto &entry : values)
        {
            remapped.emplace_back(remap[entry.first], entry.second);
        }
        std::ranges::sort(remapped, [](const auto &a, const auto &b) { return a.first < b.first; });

        assert(line.Source() != nullptr);
        LogLine rebuilt(std::move(remapped), mKeys, *line.Source(), line.LineId());
        mLines.push_back(std::move(rebuilt));
    }

    if (other.mTimestampsAlreadyParsed)
    {
        mTimestampsAlreadyParsed = true;
    }
}

void LogData::AppendBatch(std::vector<LogLine> lines, const std::vector<uint64_t> &lineOffsets)
{
    // No per-batch `reserve` — rely on geometric push_back growth
    // (some STL impls take `reserve(size+n)` as exact = O(N^2/B)).
    if (!lines.empty())
    {
        for (auto &line : lines)
        {
            line.RebindKeys(mKeys);
            mLines.push_back(std::move(line));
        }
    }

    if (!lineOffsets.empty())
    {
        // Route offsets to the most-recently-appended `FileLineSource`
        // — the file currently being streamed. Live-tail passes empty
        // `lineOffsets` and skips this branch.
        FileLineSource *fileSource = BackFileSource();
        assert(fileSource != nullptr);
        if (fileSource != nullptr)
        {
            fileSource->File().AppendLineOffsets(lineOffsets);
        }
    }
}

} // namespace loglib
