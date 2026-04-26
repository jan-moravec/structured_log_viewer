#pragma once

#include "log_data.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace loglib
{

/**
 * @brief Represents the configuration for log data, including columns and filters.
 */
struct LogConfiguration
{
    /**
     * @brief Enum to specify the type of data in a column.
     */
    enum class Type
    {
        any, ///< Any type of data.
        time ///< Time-based data.
    };

    /**
     * @brief Represents a column in the log configuration.
     */
    struct Column
    {
        std::string header;                    ///< The header name of the column.
        std::vector<std::string> keys;         ///< Keys associated with the column.
        std::string printFormat;               ///< Format string for printing the column data.
        Type type = Type::any;                 ///< Type of the column data.
        std::vector<std::string> parseFormats; ///< Formats for parsing the column data.
    };

    /**
     * @brief Represents a filter for log data.
     */
    struct LogFilter
    {
        /**
         * @brief Enum to specify the type of filter.
         */
        enum class Type
        {
            string, ///< String-based filter.
            time    ///< Time-based filter.
        };

        /**
         * @brief Enum to specify the matching criteria for the filter.
         */
        enum class Match
        {
            exactly,           ///< Exact match.
            contains,          ///< Substring match.
            regularExpression, ///< Regular expression match.
            wildcard           ///< Wildcard match.
        };

        Type type;                               ///< Type of the filter.
        int row;                                 ///< Row index to apply the filter.
        std::optional<std::string> filterString; ///< String value for the filter.
        std::optional<Match> matchType;          ///< Matching criteria for the filter.
        std::optional<int64_t> filterBegin;      ///< Start range for time-based filters.
        std::optional<int64_t> filterEnd;        ///< End range for time-based filters.
    };

    std::vector<Column> columns;    ///< List of columns in the configuration.
    std::vector<LogFilter> filters; ///< List of filters in the configuration.
};

/**
 * @brief Manages the log configuration, including loading, saving, and updating it.
 */
class LogConfigurationManager
{
public:
    LogConfigurationManager() = default;

    /**
     * @brief Loads the log configuration from a file.
     * @param path The path to the configuration file.
     * @throws std::runtime_error if the file cannot be opened.
     */
    void Load(const std::filesystem::path &path);

    /**
     * @brief Saves the log configuration to a file.
     * @param path The path to the configuration file.
     * @throws std::runtime_error if the file cannot be opened.
     */
    void Save(const std::filesystem::path &path) const;

    /**
     * @brief Updates the log configuration based on the provided log data.
     * @param logData The log data used to update the configuration.
     */
    void Update(const LogData &logData);

    /**
     * @brief Append-only configuration extension used by the streaming path.
     *
     * Walks @p newKeys (typically `StreamedBatch::newKeys`) and appends a
     * column at the *end* of `mConfiguration.columns` for any key that is
     * not already configured. Auto-promotes timestamp-named keys to
     * `Type::time` columns (matching the heuristic in `Update`) but, unlike
     * `Update`, never reorders existing columns — the streaming UI contract
     * (PRD req. 4.1.13) requires that any column index that has already been
     * observed stays put for the life of the parse so Qt's
     * `beginInsertColumns` works correctly.
     *
     * @param newKeys Keys observed for the first time in the most recent
     *                streaming batch.
     */
    void AppendKeys(const std::vector<std::string> &newKeys);

    /**
     * @brief Retrieves the current log configuration.
     * @return A constant reference to the log configuration.
     */
    const LogConfiguration &Configuration() const;

private:
    /**
     * @brief Ensures `mKeysInColumns` mirrors the current `mConfiguration.columns`.
     *
     * Lazily rebuilt on the next query after any mutating path
     * (`Load` / `Update` / `AppendKeys`) flips `mCacheStale`. Mutating paths
     * also keep the cache fresh as they append new columns so subsequent
     * iterations of the same loop see the freshly-added keys without needing
     * a full rebuild (PRD §4.7.6 / parser-perf task 8.6).
     */
    void EnsureKeyCacheBuilt() const;

    /**
     * @brief Cached `O(1)` membership test for "is @p key listed under any
     *        column's `keys` vector".
     *
     * Replaces the O(M·K) free-function `IsKeyInAnyColumn` that walked every
     * column × every key per query. For a 30-column configuration with ~30
     * keys/column this drops ~900 string compares per inserted key on the
     * GUI thread to a single hash lookup. Critical prerequisite for the
     * `[wide]` benchmark to be meaningful (PRD §4.7.6 / Q7).
     */
    bool IsKeyInAnyColumnCached(const std::string &key) const;

    LogConfiguration mConfiguration;

    /// Cache of every key that appears under any column's `keys` vector. See
    /// PRD §4.7.6 / Q7 for the invalidation contract: every mutating path
    /// (`Load`, `Update`, `AppendKeys`) flips `mCacheStale` so the next
    /// query rebuilds it. NOTE for the next mutator-adder: any new column-
    /// mutating path (e.g. a future `RemoveColumn`) MUST flip this flag too.
    /// `Save` and `Configuration` are read-only and leave the flag alone.
    mutable std::unordered_set<std::string> mKeysInColumns;
    mutable bool mCacheStale = true;
};

} // namespace loglib
