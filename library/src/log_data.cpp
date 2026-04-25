#include "loglib/log_data.hpp"

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

    // Make sure every line points to the canonical KeyIndex we now own. Callers may have
    // built `lines` against a temporary KeyIndex (e.g. inside a parser-local builder) before
    // moving it into us.
    for (auto &line : mLines)
    {
        line.RebindKeys(mKeys);
    }
}

LogData::LogData(LogData &&other) noexcept
    : mFiles(std::move(other.mFiles))
    , mLines(std::move(other.mLines))
    , mKeys(std::move(other.mKeys))
    , mTimestampsAlreadyParsed(other.mTimestampsAlreadyParsed)
{
    // KeyIndex is pImpl, so the underlying storage stays put on the heap, but the wrapper
    // moved to a new address. Re-bind every LogLine's `mKeys` back-pointer so it dereferences
    // through *this rather than the moved-from `other.mKeys` (which Catch may destroy
    // arbitrarily after the move on Windows where NRVO didn't elide the copy).
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

    // Build a remap table: ids in other.mKeys -> ids in mKeys. Walking other.mKeys' high-water
    // slice keeps this O(N) in the merged-in key count rather than O(M*N) per line.
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
        // Rewire each indexed pair's KeyId via the remap table, then re-sort because the new
        // ids may not be in the original order. A single pass over each line keeps the merge
        // bounded by the total number of (line, key) pairs.
        std::vector<std::pair<KeyId, LogValue>> remapped;
        const auto values = line.IndexedValues();
        remapped.reserve(values.size());
        for (const auto &entry : values)
        {
            remapped.emplace_back(remap[entry.first], entry.second);
        }
        std::sort(remapped.begin(), remapped.end(), [](const auto &a, const auto &b) {
            return a.first < b.first;
        });

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
    // Streaming append path used by Stage C of the parsing pipeline (PRD req. 4.1.13a /
    // 4.2.18 Stage C). Lines arrive already bound to the canonical KeyIndex (the parser
    // borrows it via `StreamingLogSink::Keys`), but we re-bind defensively in case a caller
    // builds a batch against a different `KeyIndex` instance and hands it in directly.
    if (!lines.empty())
    {
        mLines.reserve(mLines.size() + lines.size());
        for (auto &line : lines)
        {
            line.RebindKeys(mKeys);
            mLines.push_back(std::move(line));
        }
    }

    if (!mFiles.empty() && !lineOffsets.empty())
    {
        mFiles.front()->AppendLineOffsets(lineOffsets);
    }
}

} // namespace loglib
