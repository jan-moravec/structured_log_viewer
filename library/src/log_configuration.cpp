#include "loglib/log_configuration.hpp"

#include "loglib/internal/ascii_case.hpp"
#include "loglib/internal/log_configuration_glaze_meta.hpp"
#include "loglib/internal/log_configuration_glaze_opts.hpp"
#include "loglib/log_data.hpp"

#include <glaze/glaze.hpp>

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string_view>

namespace
{

std::string ToLower(const std::string &str)
{
    std::string lower = str;
    std::ranges::transform(lower, lower.begin(), [](auto c) { return static_cast<unsigned char>(std::tolower(c)); });
    return lower;
}

bool IsTimestampKey(const std::string &key)
{
    // Common JSON / structured-log timestamp field names; bare "t" is
    // excluded to avoid false-positives.
    static const std::vector<std::string> TIMESTAMP_KEYS = {
        "timestamp", "time", "ts", "@timestamp", "datetime", "created_at"
    };
    return std::ranges::any_of(TIMESTAMP_KEYS, [lowerKey = ToLower(key)](const std::string &value) {
        return (lowerKey == value);
    });
}

} // namespace

namespace loglib
{

bool IsLogLevelKey(const std::string &key)
{
    // Known log-level field names. Matched case-insensitively and
    // allocation-free because this fires once per column per batch.
    // Advisory only -- false positives (e.g. a `length` column matching
    // `l`) are caught by the dictionary-content check in
    // `LogTable::MaybePromoteToLevel`.
    static constexpr std::array<std::string_view, 23> LEVEL_KEYS = {
        // Long-form / classic.
        "level",
        "severity",
        "loglevel",
        "log.level",
        "log_level",
        "lvl",
        "levelname",
        "priority",
        // Short forms (compact / embedded loggers).
        "l",
        "lv",
        "lev",
        "sev",
        "s",
        "loglvl",
        // OpenTelemetry / ECS / GCP.
        "severity_text",
        "severity.text",
        "severitytext",
        "log_severity",
        "log.severity",
        "logseverity",
        // Separator variants of `levelname`.
        "level_name",
        "level.name",
        // Serilog `@l`, Datadog @-fields, ...
        "@level",
    };
    const std::string_view keyView(key);
    return std::ranges::any_of(LEVEL_KEYS, [keyView](std::string_view value) {
        return internal::EqualsIgnoreCaseAscii(keyView, value);
    });
}

int FirstTimeColumnIndex(const LogConfiguration &configuration)
{
    for (size_t i = 0; i < configuration.columns.size(); ++i)
    {
        if (configuration.columns[i].type == LogConfiguration::Type::Time)
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}

} // namespace loglib

namespace
{

// See `log_configuration_glaze_opts.hpp` for the pinned options.
constexpr auto LOG_CONFIG_OPTS = loglib::internal::LOG_CONFIG_OPTS;

/// Wire-format shim for `SaveScope::ColumnsOnly`: emits only the
/// `columns` array, skipping the default `filters` / `sort` blocks a
/// transient `LogConfiguration` would still serialise. Files written
/// from this shim still parse cleanly through
/// `glz::read_json<LogConfiguration>` -- missing members default.
struct ColumnsOnlyDocument
{
    // Reference (not pointer) so the type is never default-
    // constructible and the glaze accessor can't deref null. `Save`
    // is the only construction site and holds the source vector for
    // the synchronous `glz::write` call.
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
    const std::vector<loglib::LogConfiguration::Column> &columns;
};

} // namespace

// `value` is glaze's required slot name (same exemption as the
// canonical meta block in `log_configuration_glaze_meta.hpp`).
// NOLINTBEGIN(readability-identifier-naming)
template <> struct glz::meta<ColumnsOnlyDocument>
{
    using T = ColumnsOnlyDocument;
    static constexpr auto value = object("columns", [](auto &self) -> const auto & { return self.columns; });
};
// NOLINTEND(readability-identifier-naming)

namespace loglib
{

void LogConfigurationManager::Load(const std::filesystem::path &path)
{
    const std::ifstream file(path);
    if (!file.is_open())
    {
        throw std::runtime_error("Failed to open file '" + path.string() + "'.");
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    LoadFromString(buffer.str());
}

void LogConfigurationManager::LoadFromString(std::string_view content)
{
    // Parse into a temporary first: Glaze writes member-by-member,
    // so reading directly into `mConfiguration` could leave it
    // half-populated on a mid-file parse error.
    LogConfiguration parsed;
    // `LOG_CONFIG_OPTS` (not `glz::read_json`) so
    // `error_on_unknown_keys=false` lets legacy / forward-compat
    // schemas load. See the header for the rationale.
    const auto error = glz::read<LOG_CONFIG_OPTS>(parsed, content);
    if (error)
    {
        throw std::runtime_error(
            "Failed to parse configuration file: " + glz::format_error(error, std::string(content))
        );
    }
    mConfiguration = std::move(parsed);
    mCacheStale = true;
}

void LogConfigurationManager::Save(const std::filesystem::path &path) const
{
    Save(path, SaveScope::Full);
}

void LogConfigurationManager::Save(const std::filesystem::path &path, SaveScope scope) const
{
    Save(mConfiguration, path, scope);
}

void LogConfigurationManager::Save(
    const LogConfiguration &configuration, const std::filesystem::path &path, SaveScope scope
)
{
    std::string json;
    if (scope == SaveScope::ColumnsOnly)
    {
        // Use the glaze shim so the written JSON has only `columns`,
        // not the default `filters` / `sort` blocks the full struct
        // would emit.
        const ColumnsOnlyDocument document{.columns = configuration.columns};
        const auto error = glz::write<LOG_CONFIG_OPTS>(document, json);
        if (error)
        {
            throw std::runtime_error("Failed to serialize configuration: " + glz::format_error(error));
        }
    }
    else
    {
        const auto error = glz::write<LOG_CONFIG_OPTS>(configuration, json);
        if (error)
        {
            throw std::runtime_error("Failed to serialize configuration: " + glz::format_error(error));
        }
    }

    // Atomic write: stream to `<path>.tmp`, flush + check stream
    // state, then rename. Prevents a crash / ENOSPC mid-write from
    // leaving a truncated `<uuid>.json` the recents index points at.
    // Stale `.tmp` files are swept by `CleanupOrphanFiles`.
    const std::filesystem::path tempPath = path.string() + ".tmp";
    {
        std::ofstream file(tempPath);
        if (!file.is_open())
        {
            throw std::runtime_error("Failed to open file '" + tempPath.string() + "'.");
        }
        file << json;
        file.flush();
        if (!file.good())
        {
            std::error_code cleanupEc;
            std::filesystem::remove(tempPath, cleanupEc);
            throw std::runtime_error("Failed to write file '" + tempPath.string() + "'.");
        }
        // Explicit close + good() check so close-time failures
        // (network shares, deferred-write filesystems) are observable
        // -- the destructor would swallow them.
        file.close();
        if (!file.good())
        {
            std::error_code cleanupEc;
            std::filesystem::remove(tempPath, cleanupEc);
            throw std::runtime_error("Failed to close file '" + tempPath.string() + "'.");
        }
    }
    std::error_code ec;
    std::filesystem::rename(tempPath, path, ec);
    if (ec)
    {
        std::error_code cleanupEc;
        std::filesystem::remove(tempPath, cleanupEc);
        throw std::runtime_error(
            "Failed to rename '" + tempPath.string() + "' to '" + path.string() + "': " + ec.message()
        );
    }
}

void LogConfigurationManager::Reset()
{
    // Default-construct so all fields return to factory state in
    // one assignment.
    mConfiguration = LogConfiguration{};
    mCacheStale = true;
}

void LogConfigurationManager::SetConfiguration(LogConfiguration configuration)
{
    mConfiguration = std::move(configuration);
    mCacheStale = true;
}

void LogConfigurationManager::Update(const LogData &logData)
{
    EnsureKeyCacheBuilt();
    for (const std::string &key : logData.SortedKeys())
    {
        if (!IsKeyInAnyColumnCached(key))
        {
            if (IsTimestampKey(key))
            {
                mConfiguration.columns.push_back(
                    LogConfiguration::Column{
                        .header = key,
                        .keys = {key},
                        .printFormat = "%F %H:%M:%S",
                        .type = LogConfiguration::Type::Time,
                        .parseFormats = {"%FT%T%Ez", "%F %T%Ez", "%FT%T", "%F %T"}
                    }
                );
                // Bubble the freshly-appended timestamp column to
                // position 0. `MoveColumn` (not an inline swap chain)
                // so persisted `filters[*].row` is remapped along
                // with the rotation. No-op when this is the only
                // column.
                if (mConfiguration.columns.size() > 1)
                {
                    MoveColumn(mConfiguration.columns.size() - 1, 0);
                }
            }
            else
            {
                mConfiguration.columns.push_back(
                    LogConfiguration::Column{
                        .header = key,
                        .keys = {key},
                        .printFormat = "{}",
                        .type = LogConfiguration::Type::Any,
                        .parseFormats = {}
                    }
                );
            }
            mKeysInColumns.insert(key);
        }
    }
}

void LogConfigurationManager::AppendKeys(const std::vector<std::string> &newKeys)
{
    // Append-only; Qt's `beginInsertColumns` expects appends.
    EnsureKeyCacheBuilt();
    for (const std::string &key : newKeys)
    {
        if (IsKeyInAnyColumnCached(key))
        {
            continue;
        }
        if (IsTimestampKey(key))
        {
            mConfiguration.columns.push_back(
                LogConfiguration::Column{
                    .header = key,
                    .keys = {key},
                    .printFormat = "%F %H:%M:%S",
                    .type = LogConfiguration::Type::Time,
                    .parseFormats = {"%FT%T%Ez", "%F %T%Ez", "%FT%T", "%F %T"}
                }
            );
        }
        else
        {
            mConfiguration.columns.push_back(
                LogConfiguration::Column{
                    .header = key,
                    .keys = {key},
                    .printFormat = "{}",
                    .type = LogConfiguration::Type::Any,
                    .parseFormats = {}
                }
            );
        }
        mKeysInColumns.insert(key);
    }
}

bool ShouldBubbleLevelColumn(const LogConfiguration &config, size_t columnIndex) noexcept
{
    // Same no-op conditions as the Time bubble in
    // `LogConfigurationManager::Update`.
    if (columnIndex >= config.columns.size())
    {
        return false;
    }
    if (config.columns.size() < 2)
    {
        return false;
    }
    if (columnIndex == CANONICAL_LEVEL_COLUMN_INDEX)
    {
        return false;
    }
    // Multi-Level guard: "first promoted wins" the canonical slot
    // when a payload carries more than one Level-typed key (e.g.
    // `level` and `severity`). Without this, draining multiple
    // bubbles would shuffle the slot's occupant.
    if (CANONICAL_LEVEL_COLUMN_INDEX < config.columns.size() &&
        config.columns[CANONICAL_LEVEL_COLUMN_INDEX].type == LogConfiguration::Type::Level)
    {
        return false;
    }
    return true;
}

void BubbleLevelColumnToCanonicalPosition(LogConfigurationManager &mgr, size_t columnIndex)
{
    if (ShouldBubbleLevelColumn(mgr.Configuration(), columnIndex))
    {
        mgr.MoveColumn(columnIndex, CANONICAL_LEVEL_COLUMN_INDEX);
    }
}

void LogConfigurationManager::MoveColumn(size_t srcIndex, size_t destIndex)
{
    if (srcIndex == destIndex || srcIndex >= mConfiguration.columns.size() ||
        destIndex >= mConfiguration.columns.size())
    {
        return;
    }
    using Diff = std::vector<LogConfiguration::Column>::difference_type;
    auto begin = mConfiguration.columns.begin();
    if (srcIndex > destIndex)
    {
        std::rotate(
            std::next(begin, static_cast<Diff>(destIndex)),
            std::next(begin, static_cast<Diff>(srcIndex)),
            std::next(begin, static_cast<Diff>(srcIndex + 1))
        );
    }
    else
    {
        std::rotate(
            std::next(begin, static_cast<Diff>(srcIndex)),
            std::next(begin, static_cast<Diff>(srcIndex + 1)),
            std::next(begin, static_cast<Diff>(destIndex + 1))
        );
    }
    // Run every persisted `LogFilter::row` and `sort.columnIndex`
    // through the same permutation so they follow their column even
    // when callers read `Configuration()` directly without first
    // re-mirroring from a runtime proxy.
    const int src = static_cast<int>(srcIndex);
    const int dest = static_cast<int>(destIndex);
    for (LogConfiguration::LogFilter &filter : mConfiguration.filters)
    {
        filter.row = LogConfigurationManager::RemapColumnIndexAfterMove(filter.row, src, dest);
    }
    mConfiguration.sort.columnIndex =
        LogConfigurationManager::RemapColumnIndexAfterMove(mConfiguration.sort.columnIndex, src, dest);
    // Pure reorder; key cache is unchanged.
}

int LogConfigurationManager::RemapColumnIndexAfterMove(int columnIndex, int srcIndex, int destIndex)
{
    if (srcIndex == destIndex)
    {
        return columnIndex;
    }
    if (columnIndex == srcIndex)
    {
        return destIndex;
    }
    if (srcIndex < destIndex)
    {
        // Columns in (srcIndex, destIndex] shift one slot left.
        if (columnIndex > srcIndex && columnIndex <= destIndex)
        {
            return columnIndex - 1;
        }
    }
    else
    {
        // Columns in [destIndex, srcIndex) shift one slot right.
        if (columnIndex >= destIndex && columnIndex < srcIndex)
        {
            return columnIndex + 1;
        }
    }
    return columnIndex;
}

void LogConfigurationManager::SetColumnType(size_t columnIndex, LogConfiguration::Type type)
{
    if (columnIndex >= mConfiguration.columns.size())
    {
        return;
    }
    mConfiguration.columns[columnIndex].type = type;
}

void LogConfigurationManager::SetColumnAutoDetect(size_t columnIndex, bool autoDetect)
{
    if (columnIndex >= mConfiguration.columns.size())
    {
        return;
    }
    mConfiguration.columns[columnIndex].autoDetect = autoDetect;
}

void LogConfigurationManager::SetColumnTypePair(size_t columnIndex, LogConfiguration::Type type, bool autoDetect)
{
    if (columnIndex >= mConfiguration.columns.size())
    {
        return;
    }
    auto &column = mConfiguration.columns[columnIndex];
    column.type = type;
    column.autoDetect = autoDetect;
}

void LogConfigurationManager::SetColumnVisible(size_t columnIndex, bool visible)
{
    if (columnIndex >= mConfiguration.columns.size())
    {
        return;
    }
    mConfiguration.columns[columnIndex].visible = visible;
}

void LogConfigurationManager::SetColumnHeader(size_t columnIndex, std::string header)
{
    if (columnIndex >= mConfiguration.columns.size())
    {
        return;
    }
    mConfiguration.columns[columnIndex].header = std::move(header);
}

void LogConfigurationManager::SetColumnPrintFormat(size_t columnIndex, std::string printFormat)
{
    if (columnIndex >= mConfiguration.columns.size())
    {
        return;
    }
    mConfiguration.columns[columnIndex].printFormat = std::move(printFormat);
}

void LogConfigurationManager::SetColumnParseFormats(size_t columnIndex, std::vector<std::string> parseFormats)
{
    if (columnIndex >= mConfiguration.columns.size())
    {
        return;
    }
    mConfiguration.columns[columnIndex].parseFormats = std::move(parseFormats);
}

void LogConfigurationManager::SetFilters(std::vector<LogConfiguration::LogFilter> filters)
{
    mConfiguration.filters = std::move(filters);
}

void LogConfigurationManager::SetSort(LogConfiguration::Sort sort)
{
    mConfiguration.sort = sort;
}

void LogConfigurationManager::SetSource(std::optional<LogConfiguration::Source> source)
{
    mConfiguration.source = std::move(source);
}

void LogConfigurationManager::SetAnchors(std::vector<LogConfiguration::AnchorEntry> anchors)
{
    mConfiguration.anchors = std::move(anchors);
}

size_t LogConfigurationManager::CountAppendableKeys(const std::vector<std::string> &newKeys) const
{
    if (newKeys.empty())
    {
        return 0;
    }
    EnsureKeyCacheBuilt();
    // Mirrors `AppendKeys`'s skip predicate plus an in-input de-dupe.
    std::unordered_set<std::string_view> alreadyCounted;
    alreadyCounted.reserve(newKeys.size());
    size_t count = 0;
    for (const std::string &key : newKeys)
    {
        if (IsKeyInAnyColumnCached(key))
        {
            continue;
        }
        if (alreadyCounted.insert(std::string_view(key)).second)
        {
            ++count;
        }
    }
    return count;
}

const LogConfiguration &LogConfigurationManager::Configuration() const
{
    return mConfiguration;
}

void LogConfigurationManager::EnsureKeyCacheBuilt() const
{
    if (!mCacheStale)
    {
        return;
    }
    mKeysInColumns.clear();
    mKeysInColumns.reserve(mConfiguration.columns.size() * 4);
    for (const LogConfiguration::Column &column : mConfiguration.columns)
    {
        for (const std::string &key : column.keys)
        {
            mKeysInColumns.insert(key);
        }
    }
    mCacheStale = false;
}

bool LogConfigurationManager::IsKeyInAnyColumnCached(const std::string &key) const
{
    EnsureKeyCacheBuilt();
    return mKeysInColumns.contains(key);
}

} // namespace loglib
