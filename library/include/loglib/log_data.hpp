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
/// `KeyIndex`; every owned `LogLine` resolves keys through it.
class LogData
{
public:
    LogData();

    /// Constructs a `LogData` and rebinds each line's `KeyIndex` back-pointer
    /// to @p keys, so lines built against a temporary KeyIndex stay valid.
    LogData(std::unique_ptr<LogFile> file, std::vector<LogLine> lines, KeyIndex keys);

    LogData(const LogData &) = delete;
    LogData &operator=(const LogData &) = delete;

    /// Move ops rebind each line's `KeyIndex` back-pointer to the new owner.
    LogData(LogData &&other) noexcept;
    LogData &operator=(LogData &&other) noexcept;

    const std::vector<std::unique_ptr<LogFile>> &Files() const;
    std::vector<std::unique_ptr<LogFile>> &Files();

    const std::vector<LogLine> &Lines() const;
    std::vector<LogLine> &Lines();

    const KeyIndex &Keys() const;
    KeyIndex &Keys();

    /// Sorted snapshot of the registered keys. Cold path.
    std::vector<std::string> SortedKeys() const;

    /// Whether Stage B already promoted timestamp columns in the parser, so
    /// `LogTable::Update` can skip the whole-data pass.
    bool TimestampsAlreadyParsed() const;
    void MarkTimestampsParsed();

    /// Merges @p other in place, rewiring back-pointers and remapping KeyIds
    /// to this side's canonical `KeyIndex`.
    void Merge(LogData &&other);

    /// Streaming append of a pre-parsed batch.
    void AppendBatch(std::vector<LogLine> lines, std::vector<uint64_t> lineOffsets);

private:
    std::vector<std::unique_ptr<LogFile>> mFiles;
    std::vector<LogLine> mLines;
    KeyIndex mKeys;
    bool mTimestampsAlreadyParsed = false;
};

} // namespace loglib
