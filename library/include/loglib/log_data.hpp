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

/// Collection of log data loaded from one or more log sources. Owns the
/// canonical `KeyIndex`; every owned `LogLine` resolves keys through it.
///
/// The session's source list is stored as
/// `vector<unique_ptr<LineSource>>` — heterogeneous on purpose:
/// `FileLineSource`s for static `File → Open…` flows,
/// `StreamLineSource`s for live-tail / non-mmap streaming sources.
/// `LogData::Sources()` exposes the polymorphic container;
/// `FrontFileSource()` / `FrontStreamSource()` are typed accessors for
/// branches that still need direct `LogFile` access or producer-level
/// control (`Stop()`, retention).
class LogData
{
public:
    LogData();

    /// Constructs a `LogData` from a single source and rebinds each
    /// line's `KeyIndex` back-pointer to @p keys, so lines built against
    /// a temporary `KeyIndex` stay valid.
    LogData(std::unique_ptr<LineSource> source, std::vector<LogLine> lines, KeyIndex keys);

    LogData(const LogData &) = delete;
    LogData &operator=(const LogData &) = delete;

    /// Move ops rebind each line's `KeyIndex` back-pointer to the new owner.
    /// `LogLine::mSource` survives the move automatically because the
    /// underlying `LineSource` heap object stays at its address (only
    /// the `unique_ptr` wrappers move).
    LogData(LogData &&other) noexcept;
    LogData &operator=(LogData &&other) noexcept;

    /// Polymorphic source list. The first entry, when present, is the
    /// canonical "primary" source; multi-source loads add more (the
    /// `Merge` path concatenates them here).
    [[nodiscard]] const std::vector<std::unique_ptr<LineSource>> &Sources() const noexcept;
    [[nodiscard]] std::vector<std::unique_ptr<LineSource>> &Sources() noexcept;

    /// Typed accessor for the front source iff it is a `FileLineSource`.
    /// Returns `nullptr` otherwise (empty source list, or front source
    /// is a `StreamLineSource`). Used by the static-file branches that
    /// still need direct `LogFile` access — `LogTable::ReserveLineOffsets`,
    /// `LogData::AppendBatch(lines, lineOffsets)`. Multi-file static
    /// loads have not landed yet, so single-source is the common shape.
    [[nodiscard]] FileLineSource *FrontFileSource() noexcept;
    [[nodiscard]] const FileLineSource *FrontFileSource() const noexcept;

    /// Mirror of `FrontFileSource` for the live-tail path. Returns the
    /// first installed `StreamLineSource` (the streaming pipeline only
    /// installs one — multi-stream sessions are not in scope) or
    /// nullptr when the table is empty / file-only. Used by
    /// `LogModel`'s stop-on-clear path to access the byte producer
    /// owned by the source (PRD 4.10.4).
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
    void Merge(LogData &&other);

    /// Streaming append of a pre-parsed `LogLine` batch. `lineOffsets`
    /// is the per-line offset table for `LogFile::GetLine`; only used
    /// (and gated) when `FrontFileSource() != nullptr`. The live-tail
    /// path passes an empty vector — `StreamLineSource` owns its own
    /// per-line storage.
    void AppendBatch(std::vector<LogLine> lines, std::vector<uint64_t> lineOffsets);

private:
    std::vector<std::unique_ptr<LineSource>> mSources;
    std::vector<LogLine> mLines;
    KeyIndex mKeys;
    bool mTimestampsAlreadyParsed = false;
};

} // namespace loglib
