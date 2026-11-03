/**
 * @brief Container for parsed rows, their keys and their sources.
 *
 * LogData owns the KeyIndex, the list of LineSources and the rows of one log. Every row resolves its keys through the
 * shared index and its text through the source it came from. Parsers fill a LogData and LogTable consumes it.
 *
 * This is part of the core module.
 */
#pragma once

#include "loglib/core/file_line_source.hpp"
#include "loglib/core/key_index.hpp"
#include "loglib/core/line_source.hpp"
#include "loglib/core/log_line.hpp"
#include "loglib/core/stream_line_source.hpp"

#include <memory>
#include <span>
#include <string>
#include <vector>

namespace loglib
{

/**
 * @brief The rows, keys and sources of one log, loaded from one or more sources.
 *
 * It owns the canonical KeyIndex, and every LogLine resolves its keys through it. The sources are FileLineSources for
 * static files and StreamLineSources for live streams. Use Sources for generic access and the typed accessors when
 * code needs the file or stream behind a row.
 */
class LogData
{
public:
    LogData();

    /** @brief Builds a LogData from one source and its lines, and points every line at keys. */
    LogData(std::unique_ptr<LineSource> source, std::vector<LogLine> lines, KeyIndex keys);

    LogData(const LogData &) = delete;
    LogData &operator=(const LogData &) = delete;

    /** @brief Moves the data and points every line at the new KeyIndex. The sources stay at the same address. */
    LogData(LogData &&other) noexcept;
    LogData &operator=(LogData &&other) noexcept;
    ~LogData() = default;

    /** @brief The sources in load order. The first one is the primary source. */
    [[nodiscard]] const std::vector<std::unique_ptr<LineSource>> &Sources() const noexcept;
    [[nodiscard]] std::vector<std::unique_ptr<LineSource>> &Sources() noexcept;

    /** @brief The first source if it is a FileLineSource, otherwise null. */
    [[nodiscard]] FileLineSource *FrontFileSource() noexcept;
    [[nodiscard]] const FileLineSource *FrontFileSource() const noexcept;

    /** @brief The last FileLineSource, or null. When files load one after another this is the one being filled. */
    [[nodiscard]] FileLineSource *BackFileSource() noexcept;
    [[nodiscard]] const FileLineSource *BackFileSource() const noexcept;

    /** @brief The last StreamLineSource, or null. File history can come before it in Sources. */
    [[nodiscard]] StreamLineSource *BackStreamSource() noexcept;
    [[nodiscard]] const StreamLineSource *BackStreamSource() const noexcept;

    const std::vector<LogLine> &Lines() const;
    std::vector<LogLine> &Lines();

    const KeyIndex &Keys() const;
    KeyIndex &Keys();

    /** @brief The registered keys in sorted order. Builds a new list, so keep it off hot paths. */
    std::vector<std::string> SortedKeys() const;

    /** @brief True when the parser already converted the time columns, so LogTable can skip its own pass. */
    bool TimestampsAlreadyParsed() const;
    void MarkTimestampsParsed();

    /** @brief Moves all rows and sources of other into this one, mapping its keys onto this KeyIndex. */
    void Merge(LogData other);

    /**
     * @brief Appends a parsed batch of rows.
     *
     * For file sources lineOffsets extends the file's line offsets. A stream source keeps its own line storage, so a
     * streaming caller passes an empty vector. The multi-line spans are registered after the offsets.
     */
    void AppendBatch(
        std::vector<LogLine> lines,
        const std::vector<uint64_t> &lineOffsets,
        std::span<const MultiLineRecordSpan> multiLineSpans = {}
    );

private:
    std::vector<std::unique_ptr<LineSource>> mSources;
    std::vector<LogLine> mLines;
    KeyIndex mKeys;
    bool mTimestampsAlreadyParsed = false;
};

} // namespace loglib
