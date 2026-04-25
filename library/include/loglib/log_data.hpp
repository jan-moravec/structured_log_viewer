#pragma once

#include "key_index.hpp"
#include "log_file.hpp"
#include "log_line.hpp"

#include <memory>
#include <string>
#include <vector>

namespace loglib
{

/**
 * @brief Collection of log data loaded from one or more log files.
 *
 * Owns the canonical `KeyIndex` for the dataset (the dictionary that maps log
 * field names to dense `KeyId`s) and every `LogLine` resolves keys through it.
 * `Merge` rewires the back-pointers when combining `LogData` instances.
 */
class LogData
{
public:
    LogData();

    /**
     * @brief Constructs a `LogData` instance from a parser's raw output.
     *
     * @param file Owning pointer to the source `LogFile`.
     * @param lines Parsed log lines. Each line's `mKeys` back-pointer is
     *              rebound to the `keys` argument by this constructor, so
     *              callers can pass lines that were temporarily bound to a
     *              builder-local `KeyIndex` before the canonical one was
     *              available.
     * @param keys Canonical KeyIndex to take ownership of. Conceptually `lines`
     *             were parsed against this dictionary (or one whose key set is
     *             a subset of it).
     */
    LogData(std::unique_ptr<LogFile> file, std::vector<LogLine> lines, KeyIndex keys);

    LogData(const LogData &) = delete;
    LogData &operator=(const LogData &) = delete;

    /**
     * @brief Move constructor / assignment.
     *
     * Custom-defined (rather than `= default`) because moving `mKeys`
     * relocates the `KeyIndex` wrapper to a new address. Every `LogLine` we
     * own carries a `const KeyIndex *` back-pointer; after the underlying
     * move we walk the line vector and `RebindKeys` so the back-pointers
     * track the new owner. (PRD req. 4.1.4 — `LogLine::Keys()` must always
     * dereference to the canonical KeyIndex of the enclosing LogData.)
     */
    LogData(LogData &&other) noexcept;
    LogData &operator=(LogData &&other) noexcept;

    /**
     * @brief Read-only access to the source files. Multi-file data is built up
     *        by `Merge`-ing single-file `LogData` instances together.
     */
    const std::vector<std::unique_ptr<LogFile>> &Files() const;
    std::vector<std::unique_ptr<LogFile>> &Files();

    /**
     * @brief Read-only access to the parsed lines.
     */
    const std::vector<LogLine> &Lines() const;
    std::vector<LogLine> &Lines();

    /**
     * @brief Returns the canonical `KeyIndex`.
     */
    const KeyIndex &Keys() const;
    KeyIndex &Keys();

    /**
     * @brief Returns a sorted snapshot of the registered keys.
     *
     * Cold-path convenience for callers (configuration UI, diagnostics, tests)
     * that still want a `std::vector<std::string>`. Re-snapshot on every call,
     * so prefer `Keys()` over this in hot code.
     */
    std::vector<std::string> SortedKeys() const;

    /**
     * @brief Returns true if the streaming pipeline already promoted timestamp
     *        columns during Stage B and `LogTable::Update` should skip the
     *        whole-data `ParseTimestamps` pass.
     *
     * Defaults to false for the legacy single-shot parser path.
     */
    bool TimestampsAlreadyParsed() const;

    /**
     * @brief Marks this `LogData` as having had timestamp promotion done in
     *        the parser (Stage B). Set by the streaming pipeline before
     *        publishing the data.
     */
    void MarkTimestampsParsed();

    /**
     * @brief Merges @p other into this `LogData`, rewiring the merged-in
     *        lines' KeyIndex back-pointers and remapping their KeyIds to the
     *        canonical (this-side) KeyIndex.
     */
    void Merge(LogData &&other);

    /**
     * @brief Streaming append of a pre-parsed batch of lines and their line
     *        offsets.
     *
     * Stub for now; the body lands in task 4.0 once the pipeline is in place.
     * Declared here so downstream code can already #include the signature.
     */
    void AppendBatch(std::vector<LogLine> lines, std::vector<uint64_t> lineOffsets);

private:
    std::vector<std::unique_ptr<LogFile>> mFiles;
    std::vector<LogLine> mLines;
    KeyIndex mKeys;
    bool mTimestampsAlreadyParsed = false;
};

} // namespace loglib
