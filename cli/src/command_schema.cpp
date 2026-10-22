#include "commands.hpp"

#include "diagnostics.hpp"
#include "load_table.hpp"

#include <loglib/enum_dictionary.hpp>
#include <loglib/log_value.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace slp
{

namespace
{

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

void AppendJsonString(std::string &out, std::string_view text)
{
    out.push_back('"');
    for (const unsigned char ch : text)
    {
        switch (ch)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (ch < 0x20)
            {
                constexpr char HEX[] = "0123456789abcdef";
                out += "\\u00";
                out.push_back(HEX[ch >> 4]);
                out.push_back(HEX[ch & 0x0f]);
            }
            else
            {
                out.push_back(static_cast<char>(ch));
            }
            break;
        }
    }
    out.push_back('"');
}

[[nodiscard]] bool IsPresent(const loglib::LogValue &value) noexcept
{
    return !std::holds_alternative<std::monostate>(value);
}

[[nodiscard]] std::string FormatSample(const loglib::LogTable &table, std::size_t row, std::size_t column)
{
    std::string buffer;
    const std::string_view text = table.GetValueOrFormatted(row, column, buffer);
    return std::string(text);
}

struct ColumnStats
{
    std::uint64_t nonNullCount = 0;
    std::vector<std::string> samples;
    std::optional<std::uint64_t> enumCardinality;
    std::vector<std::string> enumValues;
};

[[nodiscard]] ColumnStats CollectColumnStats(
    const loglib::LogTable &table, std::size_t columnIndex, std::uint64_t sampleLimit
)
{
    ColumnStats stats;
    const std::size_t rows = table.RowCount();
    for (std::size_t row = 0; row < rows; ++row)
    {
        const loglib::LogValue value = table.GetValue(row, columnIndex);
        if (!IsPresent(value))
        {
            continue;
        }
        ++stats.nonNullCount;
        if (stats.samples.size() < sampleLimit)
        {
            stats.samples.push_back(FormatSample(table, row, columnIndex));
        }
    }

    const auto lookup = table.ResolveEnumColumn(columnIndex);
    if (lookup.dictionary != nullptr)
    {
        stats.enumCardinality = lookup.dictionary->Values().size();
        if (*stats.enumCardinality <= MAX_ENUM_VALUES_LISTED)
        {
            for (const std::string &value : lookup.dictionary->Values())
            {
                stats.enumValues.push_back(value);
            }
        }
    }
    return stats;
}

[[nodiscard]] std::string JoinComma(const std::vector<std::string> &parts)
{
    std::ostringstream out;
    for (std::size_t i = 0; i < parts.size(); ++i)
    {
        if (i != 0)
        {
            out << ", ";
        }
        out << parts[i];
    }
    return out.str();
}

[[nodiscard]] std::string RenderText(const loglib::LogTable &table, std::uint64_t sampleLimit)
{
    const auto &columns = table.Configuration().Configuration().columns;
    std::vector<ColumnStats> stats;
    stats.reserve(columns.size());
    for (std::size_t i = 0; i < columns.size(); ++i)
    {
        stats.push_back(CollectColumnStats(table, i, sampleLimit));
    }

    std::vector<std::string> headers = {"header", "type", "detect", "visible", "nonNull", "keys", "samples"};
    std::vector<std::vector<std::string>> rows;
    rows.reserve(columns.size());
    for (std::size_t i = 0; i < columns.size(); ++i)
    {
        const auto &column = columns[i];
        std::vector<std::string> row;
        row.push_back(column.header);
        row.emplace_back(ColumnTypeName(column.type));
        row.emplace_back(column.autoDetect ? "auto" : "pinned");
        row.emplace_back(column.visible ? "yes" : "no");
        row.push_back(std::to_string(stats[i].nonNullCount));
        row.push_back(JoinComma(column.keys));
        std::string sampleCell = JoinComma(stats[i].samples);
        if (const auto cardinality = stats[i].enumCardinality)
        {
            if (!sampleCell.empty())
            {
                sampleCell += " | ";
            }
            sampleCell += "enum=" + std::to_string(*cardinality);
            if (!stats[i].enumValues.empty())
            {
                sampleCell += " [" + JoinComma(stats[i].enumValues) + "]";
            }
        }
        row.push_back(std::move(sampleCell));
        rows.push_back(std::move(row));
    }

    std::vector<std::size_t> widths(headers.size(), 0);
    for (std::size_t c = 0; c < headers.size(); ++c)
    {
        widths[c] = headers[c].size();
    }
    for (const auto &row : rows)
    {
        for (std::size_t c = 0; c < row.size(); ++c)
        {
            widths[c] = std::max(widths[c], row[c].size());
        }
    }

    std::ostringstream out;
    auto writeRow = [&](const std::vector<std::string> &cells) {
        for (std::size_t c = 0; c < cells.size(); ++c)
        {
            if (c != 0)
            {
                out << "  ";
            }
            out << cells[c];
            if (c + 1 < cells.size())
            {
                out << std::string(widths[c] - cells[c].size(), ' ');
            }
        }
        out << '\n';
    };
    writeRow(headers);
    for (const auto &row : rows)
    {
        writeRow(row);
    }
    return out.str();
}

[[nodiscard]] std::string RenderJson(const loglib::LogTable &table, std::uint64_t sampleLimit)
{
    const auto &columns = table.Configuration().Configuration().columns;
    std::string out = "{\n  \"columns\": [\n";
    for (std::size_t i = 0; i < columns.size(); ++i)
    {
        const auto &column = columns[i];
        const ColumnStats stats = CollectColumnStats(table, i, sampleLimit);
        if (i != 0)
        {
            out += ",\n";
        }
        out += "    {\n      \"header\": ";
        AppendJsonString(out, column.header);
        out += ",\n      \"keys\": [";
        for (std::size_t k = 0; k < column.keys.size(); ++k)
        {
            if (k != 0)
            {
                out += ", ";
            }
            AppendJsonString(out, column.keys[k]);
        }
        out += "],\n      \"type\": ";
        AppendJsonString(out, ColumnTypeName(column.type));
        out += ",\n      \"autoDetect\": ";
        out += column.autoDetect ? "true" : "false";
        out += ",\n      \"visible\": ";
        out += column.visible ? "true" : "false";
        out += ",\n      \"printFormat\": ";
        AppendJsonString(out, column.printFormat);
        out += ",\n      \"parseFormats\": [";
        for (std::size_t p = 0; p < column.parseFormats.size(); ++p)
        {
            if (p != 0)
            {
                out += ", ";
            }
            AppendJsonString(out, column.parseFormats[p]);
        }
        out += "],\n      \"nonNullCount\": ";
        out += std::to_string(stats.nonNullCount);
        out += ",\n      \"samples\": [";
        for (std::size_t s = 0; s < stats.samples.size(); ++s)
        {
            if (s != 0)
            {
                out += ", ";
            }
            AppendJsonString(out, stats.samples[s]);
        }
        out += "]";
        if (stats.enumCardinality)
        {
            out += ",\n      \"enumCardinality\": ";
            out += std::to_string(*stats.enumCardinality);
            if (!stats.enumValues.empty())
            {
                out += ",\n      \"enumValues\": [";
                for (std::size_t e = 0; e < stats.enumValues.size(); ++e)
                {
                    if (e != 0)
                    {
                        out += ", ";
                    }
                    AppendJsonString(out, stats.enumValues[e]);
                }
                out += "]";
            }
        }
        out += "\n    }";
    }
    out += "\n  ]\n}\n";
    return out;
}

} // namespace

CommandOutput RunSchema(const Options &options, const RuntimeContext &runtime)
{
    auto [loaded, side] = LoadTable(options, runtime);
    CommandOutput out;
    out.err = std::move(side.err);
    if (side.exitCode != 0)
    {
        out.exitCode = side.exitCode;
        return out;
    }

    if (loaded.table.ColumnCount() == 0)
    {
        out.exitCode = static_cast<int>(ExitCode::Input);
        out.err += FormatErrorLine("schema", "source produced no columns");
        out.err.push_back('\n');
        return out;
    }

    if (options.outputFormat == "json")
    {
        out.out = RenderJson(loaded.table, options.sample);
    }
    else
    {
        out.out = RenderText(loaded.table, options.sample);
    }
    out.exitCode = static_cast<int>(ExitCode::Success);
    return out;
}

} // namespace slp
