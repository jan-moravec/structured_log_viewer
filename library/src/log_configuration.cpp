#include "loglib/log_configuration.hpp"

#include "loglib/internal/ascii_case.hpp"
#include "loglib/internal/log_configuration_glaze_meta.hpp"
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
    // Common JSON / structured-log log-level field names. Matched
    // case-insensitively. Detection is purely advisory: a column needs
    // to enum-shape (auto-promotion via `RunEnumPassForAppendBatch`)
    // before `LogTable::MaybePromoteToLevel` will flip the type from
    // `Type::Enumeration` to `Type::Level`. Allocation-free comparison
    // because this fires once per column per batch in the enum pass.
    //
    // Buckets:
    //   - Long-form / classic: `level`, `severity`, `loglevel`,
    //     `log.level`, `log_level`, `lvl`, `levelname`, `priority`.
    //   - Short forms used by compact loggers / embedded targets:
    //     `l`, `lv`, `lev`, `sev`, `s`, `loglvl`. These have a real
    //     false-positive risk (a `length` / `size` column could match
    //     by name alone) -- the dictionary-content check inside
    //     `MaybePromoteToLevel` is the safety net.
    //   - OpenTelemetry / ECS / GCP: `severity_text`, `severity.text`,
    //     `severitytext`, `log_severity`, `log.severity`, `logseverity`.
    //   - Separator variants of `levelname`: `level_name`, `level.name`.
    //   - Structured-JSON conventions (Serilog `@l`, Datadog @-fields,
    //     etc.): `@level`.
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
        // Short forms used by compact loggers / embedded targets.
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
        // Structured-JSON conventions (Serilog `@l`, Datadog @-fields, ...).
        "@level",
    };
    const std::string_view keyView(key);
    return std::ranges::any_of(LEVEL_KEYS, [keyView](std::string_view value) {
        return internal::EqualsIgnoreCaseAscii(keyView, value);
    });
}

} // namespace loglib

namespace
{

// Glaze 7.x: indentation_width is an inheritable option.
struct PrettyOpts : glz::opts
{
    uint8_t indentation_width = 4;
};
constexpr PrettyOpts PRETTIFY_OPTS{{.prettify = true}};

} // namespace

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
    const std::string content = buffer.str();

    // Parse into a temporary first: Glaze writes member-by-member,
    // so reading directly into `mConfiguration` would leave it
    // half-populated if a parse error throws mid-file.
    LogConfiguration parsed;
    const auto error = glz::read_json(parsed, content);
    if (error)
    {
        throw std::runtime_error("Failed to parse configuration file: " + glz::format_error(error, content));
    }
    mConfiguration = std::move(parsed);
    mCacheStale = true;
}

void LogConfigurationManager::Save(const std::filesystem::path &path) const
{
    std::string json;
    const auto error = glz::write<PRETTIFY_OPTS>(mConfiguration, json);
    if (error)
    {
        throw std::runtime_error("Failed to serialize configuration: " + glz::format_error(error));
    }

    std::ofstream file(path);
    if (file.is_open())
    {
        file << json;
    }
    else
    {
        throw std::runtime_error("Failed to open file '" + path.string() + "'.");
    }
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
                mConfiguration.columns.push_back(LogConfiguration::Column{
                    .header = key,
                    .keys = {key},
                    .printFormat = "%F %H:%M:%S",
                    .type = LogConfiguration::Type::Time,
                    .parseFormats = {"%FT%T%Ez", "%F %T%Ez", "%FT%T", "%F %T"}
                });
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
                mConfiguration.columns.push_back(LogConfiguration::Column{
                    .header = key,
                    .keys = {key},
                    .printFormat = "{}",
                    .type = LogConfiguration::Type::Unknown,
                    .parseFormats = {}
                });
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
            mConfiguration.columns.push_back(LogConfiguration::Column{
                .header = key,
                .keys = {key},
                .printFormat = "%F %H:%M:%S",
                .type = LogConfiguration::Type::Time,
                .parseFormats = {"%FT%T%Ez", "%F %T%Ez", "%FT%T", "%F %T"}
            });
        }
        else
        {
            mConfiguration.columns.push_back(LogConfiguration::Column{
                .header = key,
                .keys = {key},
                .printFormat = "{}",
                .type = LogConfiguration::Type::Unknown,
                .parseFormats = {}
            });
        }
        mKeysInColumns.insert(key);
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
    // Run every persisted `LogFilter::row` through the same
    // permutation so each filter follows its column.
    const int src = static_cast<int>(srcIndex);
    const int dest = static_cast<int>(destIndex);
    for (LogConfiguration::LogFilter &filter : mConfiguration.filters)
    {
        filter.row = LogConfigurationManager::RemapColumnIndexAfterMove(filter.row, src, dest);
    }
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

void LogConfigurationManager::SetColumnVisible(size_t columnIndex, bool visible)
{
    if (columnIndex >= mConfiguration.columns.size())
    {
        return;
    }
    mConfiguration.columns[columnIndex].visible = visible;
}

void LogConfigurationManager::SetFilters(std::vector<LogConfiguration::LogFilter> filters)
{
    mConfiguration.filters = std::move(filters);
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
