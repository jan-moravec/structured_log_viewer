#include "loglib/log_data.hpp"

#include "loglib/internal/compact_log_value.hpp"

#include <algorithm>
#include <cassert>
#include <iterator>
#include <utility>
#include <vector>

namespace loglib
{

LogData::LogData() = default;

LogData::LogData(std::unique_ptr<LogFile> file, std::vector<LogLine> lines, KeyIndex keys)
    : mLines(std::move(lines)), mKeys(std::move(keys))
{
    mFiles.push_back(std::move(file));

    // Rebind to the canonical KeyIndex we now own.
    for (auto &line : mLines)
    {
        line.RebindKeys(mKeys);
    }
}

LogData::LogData(LogData &&other) noexcept
    : mFiles(std::move(other.mFiles)), mLines(std::move(other.mLines)), mKeys(std::move(other.mKeys)),
      mTimestampsAlreadyParsed(other.mTimestampsAlreadyParsed)
{
    // The KeyIndex wrapper moved address; rebind line back-pointers to *this.
    for (auto &line : mLines)
    {
        line.RebindKeys(mKeys);
    }
}

LogData &LogData::operator=(LogData &&other) noexcept
{
    if (this != &other)
    {
        mFiles = std::move(other.mFiles);
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

const std::vector<std::unique_ptr<LogFile>> &LogData::Files() const
{
    return mFiles;
}

std::vector<std::unique_ptr<LogFile>> &LogData::Files()
{
    return mFiles;
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
    mFiles.reserve(mFiles.size() + other.mFiles.size());
    std::move(
        std::make_move_iterator(other.mFiles.begin()),
        std::make_move_iterator(other.mFiles.end()),
        std::back_inserter(mFiles)
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
        // Rewire each pair's KeyId via the compact span (no `LogValue`
        // materialisation), then re-sort since the new ids may differ in
        // order. `OwnedString` offsets stay relative to the merged-in
        // `LogFile`, which moves into `mFiles` above with its arena bytes
        // intact (`std::string` move swaps heap pointers, no realloc), so
        // no rebasing is needed here.
        const auto values = line.CompactValues();
        std::vector<std::pair<KeyId, detail::CompactLogValue>> remapped;
        remapped.reserve(values.size());
        for (const auto &entry : values)
        {
            remapped.emplace_back(remap[entry.first], entry.second);
        }
        std::sort(remapped.begin(), remapped.end(), [](const auto &a, const auto &b) { return a.first < b.first; });

        LogLine rebuilt(std::move(remapped), mKeys, line.FileReference());
        mLines.push_back(std::move(rebuilt));
    }

    if (other.mTimestampsAlreadyParsed)
    {
        mTimestampsAlreadyParsed = true;
    }
}

void LogData::AppendBatch(std::vector<LogLine> lines, std::vector<uint64_t> lineOffsets)
{
    // No per-batch `reserve` (STL grows to exactly n → O(N^2/B)); rely on
    // geometric `push_back` growth.
    if (!lines.empty())
    {
        for (auto &line : lines)
        {
            line.RebindKeys(mKeys);
            mLines.push_back(std::move(line));
        }
    }

    if (!mFiles.empty() && !lineOffsets.empty())
    {
        // Streaming installs exactly one file; multi-file goes through `Merge`.
        assert(mFiles.size() == 1);
        mFiles.front()->AppendLineOffsets(lineOffsets);
    }
}

} // namespace loglib
