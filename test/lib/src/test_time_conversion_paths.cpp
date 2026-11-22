// A time column is converted by the parser's workers or by the table, depending on how a file is loaded. These tests
// load the same input through every path and require the same table, so the choice never shows in the result.

#include "common.hpp"
#include "time_conversion_paths.hpp"

#include <loglib/parse/parsers/csv_parser.hpp>
#include <loglib/parse/parsers/json_parser.hpp>
#include <loglib/parse/parsers/logfmt_parser.hpp>
#include <loglib/parse/parsers/regex_parser.hpp>

#include <catch2/catch_all.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

using namespace loglib;
using test_support::LoadFileViaPath;
using test_support::PathLoad;
using test_support::TimePath;
using test_support::TimePathName;

namespace
{

/** Enough lines for the table sink to flush several times. */
constexpr int LINE_COUNT = 2600;

/** The text a time cell holds on a line: an ISO form with an offset, a naive one, or a mix with values that fail. */
enum class TimeText
{
    Offset,
    Naive,
    Mixed,
};

[[nodiscard]] std::string TimeTextOf(TimeText kind, int line)
{
    const int second = line % 60;
    const int minute = (line / 60) % 60;
    const std::string clock = std::format("10:{:02}:{:02}", minute, second);
    switch (kind)
    {
    case TimeText::Offset:
        return std::format("2026-01-01T{}.{:03}+02:00", clock, line % 1000);
    case TimeText::Naive:
        return std::format("2026-01-01 {}", clock);
    case TimeText::Mixed:
        if (line % 7 == 3)
        {
            return "not a time";
        }
        if (line % 11 == 5)
        {
            return "";
        }
        return line % 2 == 0 ? std::format("2026-01-01T{}Z", clock) : std::format("2026-01-01 {}", clock);
    }
    return {};
}

[[nodiscard]] std::string JsonInput(std::string_view timeKey, TimeText kind)
{
    std::string bytes;
    for (int i = 0; i < LINE_COUNT; ++i)
    {
        bytes += std::format(R"({{"{}":"{}","n":{},"level":"info"}})", timeKey, TimeTextOf(kind, i), i);
        bytes += '\n';
        if (i == 1000)
        {
            bytes += "this line is not json\n";
        }
    }
    return bytes;
}

[[nodiscard]] std::string LogfmtInput(std::string_view timeKey, TimeText kind)
{
    std::string bytes;
    for (int i = 0; i < LINE_COUNT; ++i)
    {
        const std::string text = TimeTextOf(kind, i);
        bytes += std::format("{}=\"{}\" n={} level=info\n", timeKey, text, i);
        if (i % 500 == 250)
        {
            bytes += "  a continuation line\n";
        }
    }
    return bytes;
}

[[nodiscard]] std::string CsvInput(std::string_view timeKey, TimeText kind)
{
    std::string bytes = std::format("{},n,level\n", timeKey);
    for (int i = 0; i < LINE_COUNT; ++i)
    {
        bytes += std::format("\"{}\",{},info\n", TimeTextOf(kind, i), i);
    }
    return bytes;
}

[[nodiscard]] std::string SyslogInput(TimeText kind)
{
    std::string bytes;
    static constexpr std::array<std::string_view, 3> MONTHS = {"Jan", "Mar", "Dec"};
    for (int i = 0; i < LINE_COUNT; ++i)
    {
        std::string time;
        if (kind == TimeText::Offset)
        {
            time = std::format("2026-01-01T10:{:02}:{:02}+02:00", (i / 60) % 60, i % 60);
        }
        else
        {
            time = std::format(
                "{} {:2} 10:{:02}:{:02}", MONTHS[static_cast<size_t>(i) % 3], 1 + (i % 28), (i / 60) % 60, i % 60
            );
        }
        bytes += std::format("{} host program[{}]: message {}\n", time, i % 90, i);
        if (kind == TimeText::Mixed && i % 400 == 399)
        {
            bytes += "a line that does not match\n";
        }
    }
    return bytes;
}

/** A value as text with its type, so cells that differ in type or in payload differ. */
[[nodiscard]] std::string CellText(const LogValue &value)
{
    return std::visit(
        [](const auto &held) -> std::string {
            using T = std::decay_t<decltype(held)>;
            if constexpr (std::is_same_v<T, std::string_view> || std::is_same_v<T, std::string>)
            {
                return "text:" + std::string(held);
            }
            else if constexpr (std::is_same_v<T, std::int64_t>)
            {
                return "int:" + std::to_string(held);
            }
            else if constexpr (std::is_same_v<T, std::uint64_t>)
            {
                return "uint:" + std::to_string(held);
            }
            else if constexpr (std::is_same_v<T, double>)
            {
                return std::format("double:{:.17g}", held);
            }
            else if constexpr (std::is_same_v<T, bool>)
            {
                return held ? "bool:true" : "bool:false";
            }
            else if constexpr (std::is_same_v<T, TimeStamp>)
            {
                return "time:" + std::to_string(held.time_since_epoch().count());
            }
            else
            {
                return "none";
            }
        },
        value
    );
}

struct InputCase
{
    const char *name;
    const LogParser *parser;
    std::string bytes;
    std::vector<std::string> timeKeys;
};

/** Checks that two loads gave the same table: layout, cells and errors. */
void RequireSameTable(const PathLoad &expected, const PathLoad &actual)
{
    const LogTable &a = *expected.table;
    const LogTable &b = *actual.table;

    REQUIRE(a.RowCount() == b.RowCount());
    REQUIRE(a.ColumnCount() == b.ColumnCount());
    const auto &columnsA = a.Layout().Columns();
    const auto &columnsB = b.Layout().Columns();
    REQUIRE(columnsA.size() == columnsB.size());
    for (size_t column = 0; column < columnsA.size(); ++column)
    {
        INFO("column " << column << " " << columnsA[column].header);
        CHECK(columnsA[column].header == columnsB[column].header);
        CHECK(columnsA[column].keys == columnsB[column].keys);
        CHECK(columnsA[column].type == columnsB[column].type);
        CHECK(columnsA[column].printFormat == columnsB[column].printFormat);
        CHECK(columnsA[column].parseFormats == columnsB[column].parseFormats);
        CHECK(columnsA[column].autoDetect == columnsB[column].autoDetect);
    }

    for (size_t column = 0; column < a.ColumnCount(); ++column)
    {
        for (size_t row = 0; row < a.RowCount(); ++row)
        {
            const std::string cellA = CellText(a.GetValue(row, column));
            const std::string cellB = CellText(b.GetValue(row, column));
            if (cellA != cellB)
            {
                INFO("row " << row << " column " << column << " " << a.GetHeader(column));
                REQUIRE(cellA == cellB);
            }
        }
    }

    CHECK(expected.errors == actual.errors);
    CHECK(expected.errorCount == actual.errorCount);
    CHECK(expected.linesConsumed == actual.linesConsumed);
}

/** Checks that each key named a Time column whose cells include timestamps, so the comparison is not of plain text. */
void RequireTimeColumns(const PathLoad &load, const std::vector<std::string> &timeKeys)
{
    const LogTable &table = *load.table;
    for (const std::string &key : timeKeys)
    {
        size_t found = table.ColumnCount();
        for (size_t column = 0; column < table.ColumnCount(); ++column)
        {
            if (table.GetHeader(column) == key)
            {
                found = column;
            }
        }
        REQUIRE(found < table.ColumnCount());
        CHECK(table.Layout().Columns()[found].type == ColumnType::Time);
        size_t timestamps = 0;
        for (size_t row = 0; row < table.RowCount(); ++row)
        {
            timestamps += std::holds_alternative<TimeStamp>(table.GetValue(row, found)) ? 1 : 0;
        }
        CHECK(timestamps > table.RowCount() / 2);
    }
}

void RequireEveryPathAgrees(const InputCase &input)
{
    const TestLogFile file("time_conversion_paths.log");
    file.Write(input.bytes);

    for (const unsigned threads : {1U, 4U})
    {
        for (const size_t batchBytes : {size_t{4096}, size_t{64 * 1024}})
        {
            INFO(input.name << ", threads " << threads << ", batch bytes " << batchBytes);
            const PathLoad reference = LoadFileViaPath(
                TimePath::NoFeedback, *input.parser, file.CreateFileLineSource(), threads, batchBytes, input.timeKeys
            );
            REQUIRE(reference.table->RowCount() > 0);
            RequireTimeColumns(reference, input.timeKeys);
            for (const TimePath path : {TimePath::TableSink, TimePath::UpFront})
            {
                INFO("path " << TimePathName(path));
                const PathLoad other = LoadFileViaPath(
                    path, *input.parser, file.CreateFileLineSource(), threads, batchBytes, input.timeKeys
                );
                RequireSameTable(reference, other);
            }
        }
    }
}

} // namespace

TEST_CASE("Every time conversion path gives the same table for JSON", "[time_conversion_paths][json]")
{
    const JsonParser parser;
    const auto kind = GENERATE(TimeText::Offset, TimeText::Naive, TimeText::Mixed);
    RequireEveryPathAgrees({"json time key", &parser, JsonInput("timestamp", kind), {"timestamp"}});
}

TEST_CASE(
    "Every time conversion path gives the same table for a JSON key that is not a time", "[time_conversion_paths][json]"
)
{
    const JsonParser parser;
    const auto kind = GENERATE(TimeText::Offset, TimeText::Mixed);
    RequireEveryPathAgrees({"json other key", &parser, JsonInput("when", kind), {}});
}

TEST_CASE("Every time conversion path gives the same table for logfmt", "[time_conversion_paths][logfmt]")
{
    const LogfmtParser parser;
    const auto kind = GENERATE(TimeText::Offset, TimeText::Naive, TimeText::Mixed);
    RequireEveryPathAgrees({"logfmt time key", &parser, LogfmtInput("ts", kind), {"ts"}});
}

TEST_CASE(
    "Every time conversion path gives the same table for a logfmt key that is not a time",
    "[time_conversion_paths][logfmt]"
)
{
    const LogfmtParser parser;
    RequireEveryPathAgrees({"logfmt other key", &parser, LogfmtInput("seen", TimeText::Offset), {}});
}

TEST_CASE("Every time conversion path gives the same table for CSV", "[time_conversion_paths][csv]")
{
    const CsvParser parser;
    const auto kind = GENERATE(TimeText::Offset, TimeText::Naive, TimeText::Mixed);
    RequireEveryPathAgrees({"csv time key", &parser, CsvInput("time", kind), {"time"}});
}

TEST_CASE(
    "Every time conversion path gives the same table for a CSV column that is not a time",
    "[time_conversion_paths][csv]"
)
{
    const CsvParser parser;
    RequireEveryPathAgrees({"csv other key", &parser, CsvInput("seen", TimeText::Offset), {}});
}

TEST_CASE("Every time conversion path gives the same table for the syslog template", "[time_conversion_paths][regex]")
{
    const RegexTemplate *tmpl = FindTemplateByName("Syslog (RFC3164)");
    REQUIRE(tmpl != nullptr);
    const RegexParser parser{tmpl->regex};
    const auto kind = GENERATE(TimeText::Offset, TimeText::Naive, TimeText::Mixed);
    RequireEveryPathAgrees({"syslog", &parser, SyslogInput(kind), {"timestamp"}});
}
