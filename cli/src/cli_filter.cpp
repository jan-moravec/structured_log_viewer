#include "cli_filter.hpp"

#include "text_util.hpp"

#include <loglib/log_configuration.hpp>
#include <loglib/log_level.hpp>
#include <loglib/query_parser.hpp>
#include <loglib/user_timestamp.hpp>

#include <algorithm>
#include <chrono>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace slp
{

namespace
{

[[nodiscard]] Failure FilterFail(std::string_view command, std::string_view sentence)
{
    return MakeFailure(ExitCode::Usage, command, sentence);
}

[[nodiscard]] Failure FilterSyntaxFailure(std::string_view query, const loglib::QueryParseError &error)
{
    Failure failure{.code = ExitCode::Usage, .text = FormatFilterParseDiagnostic(query, error.offset, error.message)};
    failure.text.push_back('\n');
    return failure;
}

} // namespace

std::string AvailableColumnList(const std::vector<loglib::Column> &columns)
{
    if (columns.empty())
    {
        return "(none)";
    }
    std::vector<std::string> headers;
    headers.reserve(columns.size());
    for (const loglib::Column &column : columns)
    {
        headers.push_back(column.header);
    }
    return JoinComma(headers);
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
        if (std::ranges::find(columns[i].keys, name) != columns[i].keys.end())
        {
            return static_cast<int>(i);
        }
    }
    for (std::size_t i = 0; i < columns.size(); ++i)
    {
        const bool match = EqualsIgnoreCase(columns[i].header, name) ||
                           std::ranges::any_of(columns[i].keys, [name](const std::string &key) {
                               return EqualsIgnoreCase(key, name);
                           });
        if (match)
        {
            return static_cast<int>(i);
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

bool IsPresent(const loglib::LogValue &value) noexcept
{
    return !std::holds_alternative<std::monostate>(value);
}

std::optional<Failure> CheckFilterSyntax(const FilterOptions &filter)
{
    for (const std::string &query : filter.filters)
    {
        if (auto parsed = loglib::ParseQuery(query); !parsed)
        {
            return FilterSyntaxFailure(query, parsed.error());
        }
    }
    return std::nullopt;
}

std::expected<loglib::FilterExpression, Failure> BuildRowFilter(
    const FilterOptions &filter, const loglib::LogTable &table, const RuntimeContext &runtime, std::string_view command
)
{
    const auto &columns = table.Configuration().Configuration().columns;
    loglib::FilterExpression::And combined;

    for (const std::string &query : filter.filters)
    {
        auto parsed = loglib::ParseQuery(query);
        if (!parsed)
        {
            return std::unexpected(FilterSyntaxFailure(query, parsed.error()));
        }
        combined.children.push_back(std::move(*parsed));
    }

    if (filter.since || filter.until)
    {
        const int timeIndex = FirstColumnOfType(columns, loglib::ColumnType::Time);
        if (timeIndex < 0)
        {
            return std::unexpected(FilterFail(
                command,
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
        auto parseBound = [&](std::string_view text, std::string_view flag) -> std::expected<std::int64_t, Failure> {
            const auto parsed = loglib::ParseUserTimestamp(text, timeColumn.parseFormats, now);
            if (!parsed)
            {
                return std::unexpected(
                    FilterFail(command, std::format("'{}' could not parse timestamp '{}'.", flag, text))
                );
            }
            std::int64_t micros = parsed->micros;
            if (parsed->isNaive)
            {
                micros = runtime.timeZone.LocalMicrosecondsToUtc(micros);
            }
            return micros;
        };
        if (filter.since)
        {
            auto value = parseBound(*filter.since, "--since");
            if (!value)
            {
                return std::unexpected(std::move(value.error()));
            }
            begin = *value;
        }
        if (filter.until)
        {
            auto value = parseBound(*filter.until, "--until");
            if (!value)
            {
                return std::unexpected(std::move(value.error()));
            }
            end = *value;
        }
        if (begin && end && *end < *begin)
        {
            return std::unexpected(FilterFail(command, "'--until' must not be earlier than '--since'."));
        }
        loglib::LeafRule rule;
        rule.type = loglib::LeafRule::Type::Time;
        rule.columnKeys = timeColumn.keys;
        rule.filterBegin = begin;
        rule.filterEnd = end;
        combined.children.push_back(loglib::FilterExpression{loglib::FilterExpression::Leaf{std::move(rule)}});
    }

    if (filter.minLevel)
    {
        const int levelIndex = FirstColumnOfType(columns, loglib::ColumnType::Level);
        if (levelIndex < 0)
        {
            return std::unexpected(FilterFail(
                command,
                std::format(
                    "'--min-level' needs a level column; available columns are {}.", AvailableColumnList(columns)
                )
            ));
        }
        const auto level = loglib::ParseLevelName(*filter.minLevel);
        if (!level || *level == loglib::LogLevel::Unknown)
        {
            return std::unexpected(FilterFail(
                command,
                std::format(
                    "'--min-level' expects trace, debug, info, warn, error, or fatal, not '{}'.", *filter.minLevel
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
    if (filter.invert)
    {
        expression = loglib::FilterExpression{loglib::FilterExpression::Not{std::move(expression)}};
    }
    return expression;
}

} // namespace slp
