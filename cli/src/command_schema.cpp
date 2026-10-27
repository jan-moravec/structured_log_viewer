#include "commands.hpp"

#include "cli_filter.hpp"
#include "console.hpp"
#include "diagnostics.hpp"
#include "json_report.hpp"
#include "load_table.hpp"
#include "text_table.hpp"
#include "text_util.hpp"

#include <loglib/enum_dictionary.hpp>
#include <loglib/log_value.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slp
{

// Report structs have external linkage: glaze reflects member names
// through templates that MSVC cannot instantiate for internal types.
namespace reports
{
// NOLINTBEGIN(misc-use-internal-linkage)

/** @brief One column of `slp schema`. Its members are the JSON keys. */
struct SchemaColumn
{
    std::string header;
    std::vector<std::string> keys;
    std::string type;
    bool autoDetect = false;
    bool visible = false;
    std::string printFormat;
    std::vector<std::string> parseFormats;
    std::uint64_t nonNullCount = 0;
    std::vector<std::string> samples;
    std::optional<std::uint64_t> enumCardinality;
    /** @brief Set only for a dictionary of at most `MAX_ENUM_VALUES_LISTED` values. */
    std::optional<std::vector<std::string>> enumValues;
};

/** @brief The `slp schema` report. */
struct SchemaReport
{
    std::vector<SchemaColumn> columns;
};

// NOLINTEND(misc-use-internal-linkage)

} // namespace reports

namespace
{

using reports::SchemaColumn;
using reports::SchemaReport;

constexpr std::size_t MAX_ENUM_VALUES_LISTED = 64;

[[nodiscard]] std::string_view ColumnTypeName(loglib::ColumnType type) noexcept
{
    switch (type)
    {
    case loglib::ColumnType::Any:
        return "any";
    case loglib::ColumnType::String:
        return "string";
    case loglib::ColumnType::Boolean:
        return "boolean";
    case loglib::ColumnType::Integer:
        return "integer";
    case loglib::ColumnType::Floating:
        return "floating";
    case loglib::ColumnType::Number:
        return "number";
    case loglib::ColumnType::Time:
        return "time";
    case loglib::ColumnType::Enumeration:
        return "enumeration";
    case loglib::ColumnType::Level:
        return "level";
    }
    return "any";
}

[[nodiscard]] SchemaColumn DescribeColumn(
    const loglib::LogTable &table, std::size_t columnIndex, std::uint64_t sampleLimit
)
{
    const loglib::Column &column = table.Configuration().Configuration().columns[columnIndex];
    SchemaColumn described{
        .header = column.header,
        .keys = column.keys,
        .type = std::string(ColumnTypeName(column.type)),
        .autoDetect = column.autoDetect,
        .visible = column.visible,
        .printFormat = column.printFormat,
        .parseFormats = column.parseFormats,
        .nonNullCount = 0,
        .samples = {},
        .enumCardinality = std::nullopt,
        .enumValues = std::nullopt,
    };

    std::string buffer;
    for (std::size_t row = 0; row < table.RowCount(); ++row)
    {
        if (!IsPresent(table.GetValue(row, columnIndex)))
        {
            continue;
        }
        ++described.nonNullCount;
        if (described.samples.size() < sampleLimit)
        {
            described.samples.emplace_back(table.GetValueOrFormatted(row, columnIndex, buffer));
        }
    }

    const auto lookup = table.ResolveEnumColumn(columnIndex);
    if (lookup.dictionary != nullptr)
    {
        const auto &values = lookup.dictionary->Values();
        described.enumCardinality = values.size();
        if (values.size() <= MAX_ENUM_VALUES_LISTED)
        {
            described.enumValues.emplace(values.begin(), values.end());
        }
    }
    return described;
}

[[nodiscard]] SchemaReport DescribeTable(const loglib::LogTable &table, std::uint64_t sampleLimit)
{
    SchemaReport report;
    for (std::size_t column = 0; column < table.ColumnCount(); ++column)
    {
        report.columns.push_back(DescribeColumn(table, column, sampleLimit));
    }
    return report;
}

[[nodiscard]] std::string RenderText(const SchemaReport &report)
{
    TextTable table({"header", "type", "detect", "visible", "nonNull", "keys", "samples"});
    for (const SchemaColumn &column : report.columns)
    {
        std::string samples = JoinComma(column.samples);
        if (column.enumCardinality)
        {
            if (!samples.empty())
            {
                samples += " | ";
            }
            samples += "enum=" + std::to_string(*column.enumCardinality);
            if (column.enumValues && !column.enumValues->empty())
            {
                samples += " [" + JoinComma(*column.enumValues) + "]";
            }
        }
        table.AddRow({
            column.header,
            column.type,
            column.autoDetect ? "auto" : "pinned",
            column.visible ? "yes" : "no",
            std::to_string(column.nonNullCount),
            JoinComma(column.keys),
            std::move(samples),
        });
    }
    return table.Render();
}

} // namespace

ExitCode RunSchema(
    const SchemaOptions &options, const GlobalOptions &global, Console &console, const RuntimeContext &runtime
)
{
    const auto loaded = LoadTable(global, options.input, console, runtime);
    if (!loaded)
    {
        return console.Report(loaded.error());
    }
    if (loaded->table.ColumnCount() == 0)
    {
        return console.Report(console.Fail(ExitCode::Input, "source produced no columns"));
    }
    const SchemaReport report = DescribeTable(loaded->table, options.sample);
    return console.Print(options.outputFormat == ReportFormat::Json ? WriteJsonReport(report) : RenderText(report));
}

} // namespace slp
