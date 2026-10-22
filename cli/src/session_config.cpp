#include "session_config.hpp"

#include "cli_filter.hpp"
#include "diagnostics.hpp"

#include <loglib/column_projection.hpp>
#include <loglib/filter_expression.hpp>

#include <algorithm>
#include <cctype>
#include <exception>
#include <format>
#include <string>
#include <utility>

namespace slp
{

namespace
{

[[nodiscard]] CommandOutput SessionFail(std::string_view command, ExitCode code, std::string_view sentence)
{
    CommandOutput out;
    out.exitCode = static_cast<int>(code);
    out.err = FormatErrorLine(command, sentence);
    out.err.push_back('\n');
    return out;
}

[[nodiscard]] bool EqualsIgnoreCase(std::string_view lhs, std::string_view rhs) noexcept
{
    return std::ranges::equal(lhs, rhs, [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    });
}

[[nodiscard]] std::vector<std::string> SplitCsv(std::string_view text)
{
    std::vector<std::string> parts;
    std::size_t begin = 0;
    while (begin <= text.size())
    {
        const std::size_t comma = text.find(',', begin);
        const std::string_view part =
            comma == std::string_view::npos ? text.substr(begin) : text.substr(begin, comma - begin);
        std::size_t start = 0;
        std::size_t end = part.size();
        while (start < end && std::isspace(static_cast<unsigned char>(part[start])) != 0)
        {
            ++start;
        }
        while (end > start && std::isspace(static_cast<unsigned char>(part[end - 1])) != 0)
        {
            --end;
        }
        if (start < end)
        {
            parts.emplace_back(part.substr(start, end - start));
        }
        if (comma == std::string_view::npos)
        {
            break;
        }
        begin = comma + 1;
    }
    return parts;
}

/** @brief First line of @p text with trailing sentence punctuation removed. */
[[nodiscard]] std::string FirstSentence(std::string_view text)
{
    const std::size_t newline = text.find('\n');
    if (newline != std::string_view::npos)
    {
        text = text.substr(0, newline);
    }
    while (!text.empty() && (text.back() == '.' || std::isspace(static_cast<unsigned char>(text.back())) != 0))
    {
        text.remove_suffix(1);
    }
    return std::string(text);
}

[[nodiscard]] bool OnlyInvertRequested(const Options &options) noexcept
{
    return options.invert && options.filters.empty() && !options.since && !options.until && !options.minLevel;
}

/** @brief Parses `--sort` as `column[:asc|:desc]` against @p columns. */
[[nodiscard]] std::expected<SortChoice, CommandOutput> ParseSortFlag(
    std::string_view spec, const std::vector<loglib::Column> &columns, std::string_view command
)
{
    SortChoice choice;
    const std::size_t colon = spec.rfind(':');
    if (colon != std::string_view::npos)
    {
        const std::string_view direction = spec.substr(colon + 1);
        if (EqualsIgnoreCase(direction, "asc"))
        {
            spec = spec.substr(0, colon);
        }
        else if (EqualsIgnoreCase(direction, "desc"))
        {
            choice.direction = loglib::SortDirection::Descending;
            spec = spec.substr(0, colon);
        }
    }
    const int index = FindColumnByName(columns, spec);
    if (index < 0)
    {
        return std::unexpected(SessionFail(
            command,
            ExitCode::Usage,
            std::format("unresolvable sort column '{}'; available columns are {}.", spec, AvailableColumnList(columns))
        ));
    }
    choice.columnIndex = static_cast<std::size_t>(index);
    return choice;
}

/** @brief Resolves `--columns` names to unique table indices in the order given. */
[[nodiscard]] std::expected<std::vector<std::size_t>, CommandOutput> ParseColumnsFlag(
    std::string_view spec, const std::vector<loglib::Column> &columns, std::string_view command
)
{
    std::vector<std::size_t> indices;
    for (const std::string &name : SplitCsv(spec))
    {
        const int index = FindColumnByName(columns, name);
        if (index < 0)
        {
            return std::unexpected(SessionFail(
                command,
                ExitCode::Usage,
                std::format("unresolvable column '{}'; available columns are {}.", name, AvailableColumnList(columns))
            ));
        }
        indices.push_back(static_cast<std::size_t>(index));
    }
    return indices;
}

[[nodiscard]] std::string LowerIfCaseInsensitiveFs(std::string text)
{
#ifdef _WIN32
    for (char &ch : text)
    {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
#endif
    return text;
}

[[nodiscard]] std::string AbsoluteGenericPath(const std::filesystem::path &path)
{
    std::error_code ec;
    std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    if (ec)
    {
        absolute = path;
    }
    return absolute.lexically_normal().generic_string();
}

[[nodiscard]] std::optional<loglib::Source::Format> FormatFromName(std::string_view name) noexcept
{
    if (name == "json")
    {
        return loglib::Source::Format::Json;
    }
    if (name == "logfmt")
    {
        return loglib::Source::Format::Logfmt;
    }
    if (name == "csv")
    {
        return loglib::Source::Format::Csv;
    }
    if (name == "regex")
    {
        return loglib::Source::Format::Regex;
    }
    return std::nullopt;
}

/**
 * @brief Builds the persisted source from the sources the user typed.
 *
 * Paths are made absolute with forward slashes, matching the GUI's
 * locator form. Stdin is skipped.
 */
[[nodiscard]] std::optional<loglib::Source> BuildSavedSource(const Options &options, const LoadedTable &loaded)
{
    loglib::Source source;
    for (const SourceSpec &spec : options.sources)
    {
        if (spec.kind != SourceKind::Path && spec.kind != SourceKind::Bundle)
        {
            continue;
        }
        std::string display = AbsoluteGenericPath(spec.path);
        std::string dedup = LowerIfCaseInsensitiveFs(display);
        loglib::AppendLocator(source, std::move(display), std::move(dedup));
    }
    if (source.locators.empty())
    {
        return std::nullopt;
    }
    source.kind = loglib::Source::Kind::File;
    source.format = FormatFromName(loaded.detectedFormat).value_or(loglib::Source::Format::Json);
    if (source.format == loglib::Source::Format::Regex)
    {
        source.regexPattern = loaded.detectedRegexPattern;
    }
    source.followRotationSiblings = !options.noRotationHistory;
    return source;
}

[[nodiscard]] std::optional<CommandOutput> WriteOne(
    const loglib::LogConfiguration &configuration, const std::filesystem::path &path, loglib::SaveScope scope
)
{
    try
    {
        loglib::LogConfigurationManager::Save(configuration, path, scope);
    }
    catch (const std::exception &ex)
    {
        CommandOutput out;
        out.exitCode = static_cast<int>(ExitCode::Output);
        out.err = FormatErrorLine(
            {}, std::format("could not write configuration '{}': {}.", path.string(), FirstSentence(ex.what()))
        );
        out.err.push_back('\n');
        return out;
    }
    return std::nullopt;
}

} // namespace

std::string CanonicalSourceLocator(const std::filesystem::path &path)
{
    return LowerIfCaseInsensitiveFs(AbsoluteGenericPath(path));
}

std::expected<loglib::LogConfiguration, std::string> LoadConfigFile(const std::filesystem::path &path)
{
    loglib::LogConfigurationManager manager;
    try
    {
        manager.Load(path);
    }
    catch (const std::exception &ex)
    {
        return std::unexpected(
            std::format("could not read configuration '{}': {}.", path.string(), FirstSentence(ex.what()))
        );
    }
    loglib::LogConfiguration configuration = manager.Configuration();
    configuration.source.reset();
    return configuration;
}

std::expected<EffectiveSession, CommandOutput> ApplySessionConfiguration(
    const Options &options, const loglib::LogTable &table, const RuntimeContext &runtime, std::string_view command
)
{
    EffectiveSession session;
    session.configuration = table.Configuration().Configuration();
    session.configuration.source.reset();
    auto &columns = session.configuration.columns;

    // Row selection: flags replace the configuration's expression.
    if (!HasActiveRowFilter(options))
    {
        session.expression = session.configuration.expression;
    }
    else if (OnlyInvertRequested(options))
    {
        session.expression = loglib::FilterExpression{loglib::FilterExpression::Not{session.configuration.expression}};
    }
    else
    {
        auto built = BuildRowFilter(options, table, runtime, command);
        if (!built)
        {
            return std::unexpected(std::move(built.error()));
        }
        session.expression = std::move(*built);
    }

    // Sort: `--sort` replaces the configuration's sort.
    if (options.sort)
    {
        auto parsed = ParseSortFlag(*options.sort, columns, command);
        if (!parsed)
        {
            return std::unexpected(std::move(parsed.error()));
        }
        session.sort = *parsed;
    }
    else if (
        const auto &persisted = session.configuration.sort;
        persisted.columnIndex >= 0 && static_cast<std::size_t>(persisted.columnIndex) < columns.size()
    )
    {
        session.sort = SortChoice{
            static_cast<std::size_t>(persisted.columnIndex),
            persisted.descending ? loglib::SortDirection::Descending : loglib::SortDirection::Ascending
        };
    }

    // Columns: `--all-columns` beats `--columns` beats the configuration.
    std::vector<std::size_t> order; // new position -> table index; empty keeps the table order.
    if (options.allColumns)
    {
        for (loglib::Column &column : columns)
        {
            column.visible = true;
        }
        session.visibleColumns.resize(columns.size());
        for (std::size_t i = 0; i < columns.size(); ++i)
        {
            session.visibleColumns[i] = i;
        }
    }
    else if (options.columns)
    {
        auto selected = ParseColumnsFlag(*options.columns, columns, command);
        if (!selected)
        {
            return std::unexpected(std::move(selected.error()));
        }
        session.visibleColumns = *selected;
        std::vector<bool> taken(columns.size(), false);
        std::vector<std::size_t> unique;
        for (const std::size_t index : *selected)
        {
            if (!taken[index])
            {
                taken[index] = true;
                unique.push_back(index);
            }
        }
        order = unique;
        for (std::size_t i = 0; i < columns.size(); ++i)
        {
            if (!taken[i])
            {
                order.push_back(i);
            }
        }
        std::vector<loglib::Column> reordered;
        reordered.reserve(columns.size());
        for (std::size_t position = 0; position < order.size(); ++position)
        {
            loglib::Column column = std::move(columns[order[position]]);
            column.visible = position < unique.size();
            reordered.push_back(std::move(column));
        }
        columns = std::move(reordered);
    }
    else
    {
        session.visibleColumns = loglib::ColumnProjection(columns).Indices();
    }

    // Persist the sort in the written column order.
    session.configuration.expression = session.expression;
    session.configuration.sort = loglib::Sort{};
    if (session.sort)
    {
        std::size_t written = session.sort->columnIndex;
        if (!order.empty())
        {
            written = static_cast<std::size_t>(std::ranges::find(order, written) - order.begin());
        }
        session.configuration.sort.columnIndex = static_cast<int>(written);
        session.configuration.sort.descending = session.sort->direction == loglib::SortDirection::Descending;
    }
    return session;
}

std::optional<CommandOutput> SaveSessionFiles(
    const Options &options, const LoadedTable &loaded, const EffectiveSession &session
)
{
    if (options.saveSession)
    {
        loglib::LogConfiguration configuration = session.configuration;
        configuration.source = BuildSavedSource(options, loaded);
        if (auto failure = WriteOne(configuration, *options.saveSession, loglib::SaveScope::Full))
        {
            return failure;
        }
    }
    if (options.saveColumns)
    {
        if (auto failure = WriteOne(session.configuration, *options.saveColumns, loglib::SaveScope::ColumnsOnly))
        {
            return failure;
        }
    }
    return std::nullopt;
}

} // namespace slp
