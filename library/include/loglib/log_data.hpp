#pragma once

#include "key_index.hpp"
#include "log_file.hpp"
#include "log_line.hpp"

#include <memory>
#include <string>
#include <vector>

namespace loglib
{

/// Collection of log data loaded from one or more log files. Owns the canonical
/// `KeyIndex` for the dataset; every owned `LogLine` resolves keys through it.
class LogData
{
public:
    LogData();

    /// @param file  Owning pointer to the source `LogFile`.
    /// @param lines Parsed log lines. Each line's KeyIndex back-pointer is
    ///              rebound to @p keys by this constructor, so callers may
    ///              pass lines that were temporarily bound to a builder-local
    ///              KeyIndex.
    /// @param keys  Canonical KeyIndex to take ownership of.
    LogData(std::unique_ptr<LogFile> file, std::vector<LogLine> lines, KeyIndex keys);

    LogData(const LogData &) = delete;
    LogData &operator=(const LogData &) = delete;

    /// Move ops walk the line vector and rebind each line's KeyIndex
    /// back-pointer to the new owner — `LogLine::Keys()` must always
    /// dereference the canonical KeyIndex of the enclosing LogData.
    LogData(LogData &&other) noexcept;
    LogData &operator=(LogData &&other) noexcept;

    const std::vector<std::unique_ptr<LogFile>> &Files() const;
    std::vector<std::unique_ptr<LogFile>> &Files();

    const std::vector<LogLine> &Lines() const;
    std::vector<LogLine> &Lines();

    const KeyIndex &Keys() const;
    KeyIndex &Keys();

    /// Sorted snapshot of the registered keys. Re-snapshots on every call;
    /// prefer `Keys()` in hot code.
    std::vector<std::string> SortedKeys() const;

    /// Returns true if the streaming pipeline already promoted timestamp
    /// columns during Stage B, so `LogTable::Update` can skip the whole-data
    /// timestamp pass.
    bool TimestampsAlreadyParsed() const;

    /// Marks this `LogData` as having had timestamp promotion done in the
    /// parser. Set by the streaming pipeline before publishing the data.
    void MarkTimestampsParsed();

    /// Merges @p other into this `LogData`, rewiring back-pointers and
    /// remapping KeyIds to the canonical (this-side) KeyIndex.
    void Merge(LogData &&other);

    /// Streaming append of a pre-parsed batch of lines and their file offsets.
    void AppendBatch(std::vector<LogLine> lines, std::vector<uint64_t> lineOffsets);

private:
    std::vector<std::unique_ptr<LogFile>> mFiles;
    std::vector<LogLine> mLines;
    KeyIndex mKeys;
    bool mTimestampsAlreadyParsed = false;
};

} // namespace loglib
