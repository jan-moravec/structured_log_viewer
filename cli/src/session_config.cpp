#include "session_config.hpp"

#include "cli_filter.hpp"
#include "diagnostics.hpp"
#include "text_util.hpp"

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

[[nodiscard]] Failure SessionFail(std::string_view command, std::string_view sentence)
{
    return MakeFailure(ExitCode::Usage, command, sentence);
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

/** @brief Parses `--sort` as `column[:asc|:desc]` against @p columns. */
[[nodiscard]] std::expected<SortChoice, Failure> ParseSortFlag(
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
            std::format("unresolvable sort column '{}'; available columns are {}.", spec, AvailableColumnList(columns))
        ));
    }
    choice.columnIndex = static_cast<std::size_t>(index);
    return choice;
}

/** @brief Resolves `--columns` names to unique table indices in the order given. */
[[nodiscard]] std::expected<std::vector<std::size_t>, Failure> ParseColumnsFlag(
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
    return AsciiLower(text);
#else
    return text;
#endif
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

/**
 * @brief Builds the persisted source from the sources the user typed.
 *
 * Paths are made absolute with forward slashes, matching the GUI's
 * locator form. Stdin is skipped.
 */
[[nodiscard]] std::optional<loglib::Source> BuildSavedSource(
    const InputOptions &input, const GlobalOptions &global, const LoadedTable &loaded
)
{
    loglib::Source source;
    for (const SourceSpec &spec : input.sources)
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
    if (loaded.format)
    {
        source.format = loaded.format->format;
        if (source.format == loglib::Source::Format::Regex)
        {
            source.regexPattern = loaded.format->regexPattern;
        }
    }
    source.followRotationSiblings = !global.noRotationHistory;
    return source;
}

[[nodiscard]] std::optional<Failure> WriteOne(
    const loglib::LogConfiguration &configuration, const std::filesystem::path &path, loglib::SaveScope scope
)
{
    try
    {
        loglib::LogConfigurationManager::Save(configuration, path, scope);
    }
    catch (const std::exception &ex)
    {
        return MakeFailure(
            ExitCode::Output,
            {},
            std::format("could not write configuration '{}': {}.", path.string(), FirstSentence(ex.what()))
        );
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

std::expected<EffectiveSession, Failure> ApplySessionConfiguration(
    const FilterOptions &filter,
    const ViewOptions &view,
    const loglib::LogTable &table,
    const RuntimeContext &runtime,
    std::string_view command
)
{
    EffectiveSession session;
    session.configuration = table.Configuration().Configuration();
    session.configuration.source.reset();
    auto &columns = session.configuration.columns;

    // Row selection: flags replace the configuration's expression.
    if (!filter.Active())
    {
        session.expression = session.configuration.expression;
    }
    else if (filter.OnlyInvert())
    {
        session.expression = loglib::FilterExpression{loglib::FilterExpression::Not{session.configuration.expression}};
    }
    else
    {
        auto built = BuildRowFilter(filter, table, runtime, command);
        if (!built)
        {
            return std::unexpected(std::move(built.error()));
        }
        session.expression = std::move(*built);
    }

    // Sort: `--sort` replaces the configuration's sort.
    if (view.sort)
    {
        auto parsed = ParseSortFlag(*view.sort, columns, command);
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
    if (view.allColumns)
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
    else if (view.columns)
    {
        auto selected = ParseColumnsFlag(*view.columns, columns, command);
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

std::optional<Failure> SaveSessionFiles(
    const SaveTargets &targets,
    const InputOptions &input,
    const GlobalOptions &global,
    const LoadedTable &loaded,
    const EffectiveSession &session
)
{
    if (targets.session)
    {
        loglib::LogConfiguration configuration = session.configuration;
        configuration.source = BuildSavedSource(input, global, loaded);
        if (auto failure = WriteOne(configuration, *targets.session, loglib::SaveScope::Full))
        {
            return failure;
        }
    }
    if (targets.columns)
    {
        if (auto failure = WriteOne(session.configuration, *targets.columns, loglib::SaveScope::ColumnsOnly))
        {
            return failure;
        }
    }
    return std::nullopt;
}

} // namespace slp
