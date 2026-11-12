#include "table_from_data.hpp"

#include <loglib/core/bytes_producer.hpp>
#include <loglib/core/key_index.hpp>
#include <loglib/core/log_data.hpp>
#include <loglib/core/log_line.hpp>
#include <loglib/core/log_value.hpp>
#include <loglib/core/stream_line_source.hpp>
#include <loglib/schema/log_configuration.hpp>
#include <loglib/table/log_table.hpp>

#include <catch2/catch_all.hpp>
#include <fmt/format.h>

#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <numbers>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace loglib;

namespace
{

/** @brief A table with one column that holds one row for every value, shown with the given print format. */
LogTable TableOfValues(const std::vector<LogValue> &values, const std::string &printFormat)
{
    auto stream = std::make_unique<StreamLineSource>(std::filesystem::path("values.jsonl"), nullptr);
    for (std::size_t i = 0; i < values.size(); ++i)
    {
        stream->AppendLine("{}", {});
    }
    StreamLineSource *source = stream.get();

    KeyIndex keys;
    std::vector<LogLine> lines;
    lines.reserve(values.size());
    for (std::size_t i = 0; i < values.size(); ++i)
    {
        lines.emplace_back(LogMap{{"v", values[i]}}, keys, *source, i + 1);
    }
    LogData data(std::move(stream), std::move(lines), std::move(keys));

    LogConfiguration configuration;
    configuration.columns.push_back(
        Column{
            .header = "v",
            .keys = {"v"},
            .printFormat = printFormat,
            .type = ColumnType::Any,
            .parseFormats = {},
        }
    );
    LogConfigurationManager manager;
    manager.SetConfiguration(std::move(configuration));
    return test_common_lib::TableFromData(std::move(data), std::move(manager));
}

/** @brief Checks that every cell reads as fmt writes the value with the print format. */
template <typename Number> void CheckAgainstFmt(const std::vector<Number> &numbers, const std::string &printFormat)
{
    std::vector<LogValue> values;
    values.reserve(numbers.size());
    for (const Number number : numbers)
    {
        values.emplace_back(number);
    }
    const LogTable table = TableOfValues(values, printFormat);
    std::string buffer;
    for (std::size_t row = 0; row < numbers.size(); ++row)
    {
        // A copy, because an element of a vector of bool is a proxy that fmt cannot format.
        const Number number = numbers[row];
        const std::string expected = fmt::format(fmt::runtime(printFormat), number);
        INFO("format " << printFormat << " row " << row);
        buffer = "left over";
        CHECK(std::string(table.GetValueOrFormatted(row, 0, buffer)) == expected);
        CHECK(table.GetFormattedValue(row, 0) == expected);
    }
}

} // namespace

TEST_CASE("Integer cells read as the print format writes them", "[cell_formatting]")
{
    const std::vector<std::int64_t> numbers{
        0,
        1,
        -1,
        42,
        -9'999,
        1'234'567'890'123,
        std::numeric_limits<std::int64_t>::max(),
        std::numeric_limits<std::int64_t>::min(),
    };
    for (const char *format : {"{}", "{:05}", "{:x}", "{:+}"})
    {
        CheckAgainstFmt(numbers, format);
    }
    CheckAgainstFmt(std::vector<std::uint64_t>{0, 7, std::numeric_limits<std::uint64_t>::max()}, "{}");
}

TEST_CASE("Floating cells read as the print format writes them", "[cell_formatting]")
{
    const std::vector<double> numbers{
        0.0,
        -0.0,
        0.5,
        -0.5,
        1.005,
        2.675,
        0.125,
        0.375,
        std::numbers::pi,
        -std::numbers::e,
        1e-7,
        123456789.987654321,
        1e15,
        1e22,
        1.7976931348623157e308,
        -1.7976931348623157e308,
        5e-324,
        2.2250738585072014e-308,
        std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::quiet_NaN(),
    };
    for (const char *format : {"{}", "{:.0f}", "{:.1f}", "{:.2f}", "{:.3f}", "{:.12f}", "{:8.2f}", "{:e}", "{:.2g}"})
    {
        CheckAgainstFmt(numbers, format);
    }
}

TEST_CASE("Boolean cells read as the print format writes them", "[cell_formatting]")
{
    CheckAgainstFmt(std::vector<bool>{true, false}, "{}");
}
