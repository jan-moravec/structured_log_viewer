#pragma once

#include "file_line_source.hpp"
#include "key_index.hpp"
#include "line_source.hpp"
#include "log_line.hpp"
#include "stream_line_source.hpp"

#include <memory>
#include <string>
#include <vector>

namespace loglib
{

/// Log data loaded from one or more sources. Owns the canonical
/// `KeyIndex`; every `LogLine` resolves keys through it.
///
/// The source list is heterogeneous: `FileLineSource`s for static
/// opens, `StreamLineSource`s for live-tail. Polymorphic access via
/// `Sources()`, typed access via `FrontFileSource()` /
/// `FrontStreamSource()` for code that still needs direct `LogFile`
/// or producer-level control.
class LogData
{
public:
    LogData();

    /// Constructs a `LogData` from a single source and rebinds each
    /// line's `KeyIndex` back-pointer to @p keys.
    LogData(std::unique_ptr<LineSource> source, std::vector<LogLine> lines, KeyIndex keys);

    LogData(const LogData &) = delete;
    LogData &operator=(const LogData &) = delete;

    /// Move ops rebind each line's `KeyIndex` back-pointer.
    /// `LogLine::mSource` survives the move because the underlying
    /// `LineSource` heap object stays put.
    LogData(LogData &&other) noexcept;
    LogData &operator=(LogData &&other) noexcept;

    /// Polymorphic source list. The front entry is the primary source.
    [[nodiscard]] const std::vector<std::unique_ptr<LineSource>> &Sources() const noexcept;
    [[nodiscard]] std::vector<std::unique_ptr<LineSource>> &Sources() noexcept;

    /// First source iff it is a `FileLineSource`, else nullptr. Used
    /// by static-file branches (e.g. `LogTable::ReserveLineOffsets`)
    /// that target the initial source.
    [[nodiscard]] FileLineSource *FrontFileSource() noexcept;
    [[nodiscard]] const FileLineSource *FrontFileSource() const noexcept;

    /// Last `FileLineSource` in `Sources()`, or nullptr. Tracks the
    /// in-flight file for sequential multi-file streaming so
    /// `AppendBatch` routes line offsets to the right source.
    [[nodiscard]] FileLineSource *BackFileSource() noexcept;
    [[nodiscard]] const FileLineSource *BackFileSource() const noexcept;

    /// First `StreamLineSource` (live-tail mirror of `FrontFileSource`),
    /// or nullptr. Used by `LogModel` to reach the byte producer.
    [[nodiscard]] StreamLineSource *FrontStreamSource() noexcept;
    [[nodiscard]] const StreamLineSource *FrontStreamSource() const noexcept;

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
    void Merge(LogData other);

    /// Append a parsed batch. `lineOffsets` populates
    /// `LogFile::mLineOffsets` for file sources; the live-tail path
    /// passes an empty vector (the source owns its per-line storage).
    void AppendBatch(std::vector<LogLine> lines, const std::vector<uint64_t> &lineOffsets);

private:
    std::vector<std::unique_ptr<LineSource>> mSources;
    std::vector<LogLine> mLines;
    KeyIndex mKeys;
    bool mTimestampsAlreadyParsed = false;
};

} // namespace loglib
