#pragma once

#include "key_index.hpp"
#include "log_file.hpp"
#include "log_line.hpp"
#include "stream_log_line.hpp"

#include <memory>
#include <string>
#include <vector>

namespace loglib
{

/// Collection of log data loaded from one or more log files. Owns the canonical
/// `KeyIndex`; every owned `LogLine` / `StreamLogLine` resolves keys through it.
///
/// Two parallel row vectors are maintained: `mLines` holds memory-mapped-backed
/// `LogLine`s produced by the static-file path, while `mStreamLines` holds
/// `StreamLogLine`s produced by `LogSource`-driven streaming sources
/// (`TailingFileSource`, future stdin / TCP / UDP). The two never share rows;
/// a session is in one mode or the other in practice. The implementer's
/// design judgement (PRD 4.9.7) favoured separate vectors over a per-row
/// `std::variant<LogLine, StreamLogLine>` to keep the existing static-path
/// hot loops (`Lines()` accessor, `BackfillTimestampColumn` over a span)
/// untouched and to skip the variant-tag dispatch on every row access.
/// `LogTable` exposes the two as a single logical row range, with file
/// rows first and stream rows after them.
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

    /// Streaming sibling of `Lines()`. Used by `LogTable` when dispatching
    /// row queries that fall past the file-rows count.
    const std::vector<StreamLogLine> &StreamLines() const;
    std::vector<StreamLogLine> &StreamLines();

    const KeyIndex &Keys() const;
    KeyIndex &Keys();

    /// Sorted snapshot of the registered keys. Cold path.
    std::vector<std::string> SortedKeys() const;

    /// Whether Stage B already promoted timestamp columns in the parser, so
    /// `LogTable::Update` can skip the whole-data pass.
    bool TimestampsAlreadyParsed() const;
    void MarkTimestampsParsed();

    /// Merges @p other in place, rewiring back-pointers and remapping KeyIds
    /// to this side's canonical `KeyIndex`. Only `mLines` is merged; the
    /// streaming path uses `AppendBatch(StreamLogLine, ...)` instead.
    void Merge(LogData &&other);

    /// Streaming append of a pre-parsed `LogLine` batch (mmap-backed sources).
    /// `lineOffsets` is the per-line offset table for `LogFile::GetLine`;
    /// gated on `mFiles.size() == 1` (PRD 4.9.7 last paragraph).
    void AppendBatch(std::vector<LogLine> lines, std::vector<uint64_t> lineOffsets);

    /// Streaming append of a pre-parsed `StreamLogLine` batch (non-mmap
    /// sources such as `TailingFileSource`). The `mFiles` invariant is
    /// bypassed entirely because stream lines are self-owning and have no
    /// associated `LogFile` (PRD 4.9.7 last paragraph).
    void AppendBatch(std::vector<StreamLogLine> lines);

private:
    std::vector<std::unique_ptr<LogFile>> mFiles;
    std::vector<LogLine> mLines;
    std::vector<StreamLogLine> mStreamLines;
    KeyIndex mKeys;
    bool mTimestampsAlreadyParsed = false;
};

} // namespace loglib
