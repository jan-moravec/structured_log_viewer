#include "cli_filter.hpp"

#include <loglib/log_configuration.hpp>
#include <loglib/log_level.hpp>
#include <loglib/query_parser.hpp>
#include <loglib/user_timestamp.hpp>

#include <cctype>
#include <chrono>
#include <format>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

namespace slp
{

namespace
{

[[nodiscard]] CommandOutput FilterFail(
    std::string_view command, ExitCode code, std::string_view sentence, std::string_view errExtra = {}
)
{
    CommandOutput out;
    out.exitCode = static_cast<int>(code);
    out.err = FormatErrorLine(command, sentence);
    out.err.push_back('\n');
    out.err += errExtra;
    return out;
}

} // namespace

std::string AvailableColumnList(const std::vector<loglib::Column> &columns)
{
    if (columns.empty())
    {
        return "(none)";
    }
    std::ostringstream out;
    for (std::size_t i = 0; i < columns.size(); ++i)
    {
        if (i != 0)
        {
            out << ", ";
        }
        out << columns[i].header;
    }
    return out.str();
}

int FindColumnByName(const std::vector<loglib::Column> &columns, std::string_view name)
{
    for (std::size_t i = 0; i < columns.size(); ++i)
    {
        if (columns[i].header == name)
        {
            return static_cast<int>(i);
        }
    }
    for (std::size_t i = 0; i < columns.size(); ++i)
    {
        for (const std::string &key : columns[i].keys)
        {
            if (key == name)
            {
                return static_cast<int>(i);
            }
        }
    }
    for (std::size_t i = 0; i < columns.size(); ++i)
    {
        if (columns[i].header.size() == name.size())
        {
            bool match = true;
            for (std::size_t j = 0; j < name.size(); ++j)
            {
                if (std::tolower(static_cast<unsigned char>(columns[i].header[j])) !=
                    std::tolower(static_cast<unsigned char>(name[j])))
                {
                    match = false;
                    break;
                }
            }
            if (match)
            {
                return static_cast<int>(i);
            }
        }
        for (const std::string &key : columns[i].keys)
        {
            if (key.size() != name.size())
            {
                continue;
            }
            bool match = true;
            for (std::size_t j = 0; j < name.size(); ++j)
            {
                if (std::tolower(static_cast<unsigned char>(key[j])) !=
                    std::tolower(static_cast<unsigned char>(name[j])))
                {
                    match = false;
                    break;
                }
            }
            if (match)
            {
                return static_cast<int>(i);
            }
        }
    }
    return -1;
}

int FirstColumnOfType(const std::vector<loglib::Column> &columns, loglib::ColumnType type)
{
    for (std::size_t i = 0; i < columns.size(); ++i)
    {
        if (columns[i].type == type)
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool HasActiveRowFilter(const Options &options) noexcept
{
    return !options.filters.empty() || options.since.has_value() || options.until.has_value() ||
           options.minLevel.has_value() || options.invert;
}

std::expected<loglib::FilterExpression, CommandOutput> BuildRowFilter(
    const Options &options, const loglib::LogTable &table, const RuntimeContext &runtime, std::string_view command
)
{
    const auto &columns = table.Configuration().Configuration().columns;
    loglib::FilterExpression::And combined;

    for (const std::string &query : options.filters)
    {
        auto parsed = loglib::ParseQuery(query);
        if (!parsed)
        {
            CommandOutput out;
            out.exitCode = static_cast<int>(ExitCode::Usage);
            out.err = FormatFilterParseDiagnostic(query, parsed.error().offset, parsed.error().message);
            out.err.push_back('\n');
            return std::unexpected(std::move(out));
        }
        combined.children.push_back(std::move(*parsed));
    }

    if (options.since || options.until)
    {
        const int timeIndex = FirstColumnOfType(columns, loglib::ColumnType::Time);
        if (timeIndex < 0)
        {
            return std::unexpected(FilterFail(
                command,
                ExitCode::Usage,
                std::format(
                    "'--since' and '--until' need a time column; available columns are {}.",
                    AvailableColumnList(columns)
                )
            ));
        }
        const loglib::Column &timeColumn = columns[static_cast<std::size_t>(timeIndex)];
        const auto now = std::chrono::system_clock::now();
        std::optional<std::int64_t> begin;
        std::optional<std::int64_t> end;
        auto parseBound = [&](std::string_view text,
                              std::string_view flag) -> std::expected<std::int64_t, CommandOutput> {
            const auto parsed = loglib::ParseUserTimestamp(text, timeColumn.parseFormats, now);
            if (!parsed)
            {
                return std::unexpected(FilterFail(
                    command, ExitCode::Usage, std::format("'{}' could not parse timestamp '{}'.", flag, text)
                ));
            }
            std::int64_t micros = parsed->micros;
            if (parsed->isNaive)
            {
                micros = runtime.timeZone.LocalMicrosecondsToUtc(micros);
            }
            return micros;
        };
        if (options.since)
        {
            auto value = parseBound(*options.since, "--since");
            if (!value)
            {
                return std::unexpected(std::move(value.error()));
            }
            begin = *value;
        }
        if (options.until)
        {
            auto value = parseBound(*options.until, "--until");
            if (!value)
            {
                return std::unexpected(std::move(value.error()));
            }
            end = *value;
        }
        if (begin && end && *end < *begin)
        {
            return std::unexpected(
                FilterFail(command, ExitCode::Usage, "'--until' must not be earlier than '--since'.")
            );
        }
        loglib::LeafRule rule;
        rule.type = loglib::LeafRule::Type::Time;
        rule.columnKeys = timeColumn.keys;
        rule.filterBegin = begin;
        rule.filterEnd = end;
        combined.children.push_back(loglib::FilterExpression{loglib::FilterExpression::Leaf{std::move(rule)}});
    }

    if (options.minLevel)
    {
        const int levelIndex = FirstColumnOfType(columns, loglib::ColumnType::Level);
        if (levelIndex < 0)
        {
            return std::unexpected(FilterFail(
                command,
                ExitCode::Usage,
                std::format(
                    "'--min-level' needs a level column; available columns are {}.", AvailableColumnList(columns)
                )
            ));
        }
        const auto level = loglib::ParseLevelName(*options.minLevel);
        if (!level || *level == loglib::LogLevel::Unknown)
        {
            return std::unexpected(FilterFail(
                command,
                ExitCode::Usage,
                std::format(
                    "'--min-level' expects trace, debug, info, warn, error, or fatal, not '{}'.", *options.minLevel
                )
            ));
        }
        loglib::LeafRule rule;
        rule.type = loglib::LeafRule::Type::Enumeration;
        rule.columnKeys = columns[static_cast<std::size_t>(levelIndex)].keys;
        for (int value = static_cast<int>(*level); value <= static_cast<int>(loglib::LogLevel::Fatal); ++value)
        {
            rule.filterValues.emplace_back(loglib::CanonicalLevelName(static_cast<loglib::LogLevel>(value)));
        }
        combined.children.push_back(loglib::FilterExpression{loglib::FilterExpression::Leaf{std::move(rule)}});
    }

    loglib::FilterExpression expression{std::move(combined)};
    if (options.invert)
    {
        expression = loglib::FilterExpression{loglib::FilterExpression::Not{std::move(expression)}};
    }
    return expression;
}

} // namespace slp
