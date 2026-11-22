#include "common.hpp"
#include "row_compare_helpers.hpp"
#include "table_fixture.hpp"

#include <loglib/config/column_layout.hpp>
#include <loglib/config/log_configuration.hpp>
#include <loglib/core/enum_dictionary.hpp>
#include <loglib/core/file_line_source.hpp>
#include <loglib/core/key_index.hpp>
#include <loglib/core/log_line.hpp>
#include <loglib/core/log_value.hpp>
#include <loglib/parse/log_parse_sink.hpp>
#include <loglib/query/row_ordering.hpp>
#include <loglib/table/log_table.hpp>

#include <catch2/catch_all.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace loglib;
using namespace row_compare_helpers;

namespace
{

LogLine MakeLine(KeyIndex &keys, LineSource &source, const std::vector<std::pair<std::string, LogValue>> &fields)
{
    std::vector<std::pair<KeyId, LogValue>> sorted;
    sorted.reserve(fields.size());
    for (const auto &[key, value] : fields)
    {
        sorted.emplace_back(keys.GetOrInsert(key), value);
    }
    std::ranges::sort(sorted, [](const auto &a, const auto &b) { return a.first < b.first; });
    return {std::move(sorted), source, 0};
}

LogTable BuildSingleColumnTable(
    const TestLogFile &testFile,
    const std::string &columnKey,
    ColumnType type,
    const std::vector<LogValue> &perRowValues
)
{
    auto source = testFile.CreateFileLineSource();
    FileLineSource *sourcePtr = source.get();

    LogConfiguration cfg;
    cfg.columns.push_back(
        {.header = columnKey, .keys = {columnKey}, .printFormat = "{}", .type = type, .parseFormats = {}}
    );
    const ColumnLayout mgr(cfg.columns);

    loglib::LogTable table(mgr);
    table.BeginStreaming(std::move(source));

    KeyIndex &keys = table.Keys();
    RowBatch batch;
    batch.firstLineNumber = 1;
    batch.lines.reserve(perRowValues.size());
    for (const auto &v : perRowValues)
    {
        batch.lines.push_back(MakeLine(keys, *sourcePtr, {{columnKey, v}}));
    }
    if (!perRowValues.empty())
    {
        batch.newKeys.emplace_back(columnKey);
    }
    AppendFixtureBatch(table, std::move(batch));
    return table;
}

std::vector<std::size_t> IotaRows(std::size_t n)
{
    std::vector<std::size_t> rows(n);
    std::iota(rows.begin(), rows.end(), std::size_t{0});
    return rows;
}

/** The ranks that RowOrdering is expected to build for the column, or none for a column without a dictionary. */
std::optional<EnumSortRank> ExpectedRank(const LogTable &table, std::size_t column)
{
    const auto &columns = table.Layout().Columns();
    if (column >= columns.size() || columns[column].type != ColumnType::Enumeration)
    {
        return std::nullopt;
    }
    const EnumDictionary *dictionary = table.ResolveEnumColumn(column).dictionary;
    if (dictionary == nullptr)
    {
        return std::nullopt;
    }
    return EnumSortRank{*dictionary};
}

void RequirePermuteMatchesSortPermutation(
    const LogTable &table, std::span<const std::size_t> logRows, const RowOrdering &ordering
)
{
    const auto viaOrdering = ordering.Permute(logRows);
    const auto rank = ExpectedRank(table, ordering.Column());
    const auto viaLib = SortPermutationByColumn(
        table,
        logRows,
        ordering.Column(),
        ordering.Direction() == SortDirection::Ascending,
        rank.has_value() ? &*rank : nullptr
    );
    REQUIRE(viaOrdering == viaLib);
}

void RequireLessThanAgreesWithPermute(std::span<const std::size_t> logRows, const RowOrdering &ordering)
{
    const auto perm = ordering.Permute(logRows);
    REQUIRE(perm.size() == logRows.size());
    for (std::size_t i = 1; i < perm.size(); ++i)
    {
        const std::size_t lhsIdx = perm[i - 1];
        const std::size_t rhsIdx = perm[i];
        REQUIRE(ordering.LessThan(logRows[lhsIdx], logRows[rhsIdx], lhsIdx, rhsIdx));
        REQUIRE_FALSE(ordering.LessThan(logRows[rhsIdx], logRows[lhsIdx], rhsIdx, lhsIdx));
    }
}

/** Every pair of rows: LessThan with the input index as tie key says what the position in Permute says. */
void RequireLessThanMatchesPermutePositionForEveryPair(
    std::span<const std::size_t> logRows, const RowOrdering &ordering
)
{
    const auto perm = ordering.Permute(logRows);
    REQUIRE(perm.size() == logRows.size());
    std::vector<std::size_t> position(perm.size());
    for (std::size_t k = 0; k < perm.size(); ++k)
    {
        position[perm[k]] = k;
    }
    for (std::size_t a = 0; a < logRows.size(); ++a)
    {
        for (std::size_t b = 0; b < logRows.size(); ++b)
        {
            if (a == b)
            {
                continue;
            }
            CAPTURE(a, b);
            CHECK(ordering.LessThan(logRows[a], logRows[b], a, b) == (position[a] < position[b]));
        }
    }
}

void RequireCompareMatchesCompareRows(const LogTable &table, std::size_t rowCount, const RowOrdering &ordering)
{
    const auto rank = ExpectedRank(table, ordering.Column());
    for (std::size_t a = 0; a < rowCount; ++a)
    {
        for (std::size_t b = 0; b < rowCount; ++b)
        {
            CHECK(
                ordering.Compare(a, b) ==
                CompareRows(table, a, b, ordering.Column(), rank.has_value() ? &*rank : nullptr)
            );
        }
    }
}

} // namespace

TEST_CASE("RowOrdering Compare and Permute match CompareRows / SortPermutationByColumn", "[row_ordering]")
{
    const TestLogFile fixture("row_ordering_integer.json");
    fixture.Write("");
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const std::vector<LogValue> values = {int64_t{42}, std::monostate{}, int64_t{-1}, nan, int64_t{0}, int64_t{42}};
    const LogTable table = BuildSingleColumnTable(fixture, "n", ColumnType::Integer, values);
    const auto logRows = IotaRows(values.size());

    for (const SortDirection direction : {SortDirection::Ascending, SortDirection::Descending})
    {
        const RowOrdering ordering(table, 0, direction);
        RequireCompareMatchesCompareRows(table, values.size(), ordering);
        RequirePermuteMatchesSortPermutation(table, logRows, ordering);
        RequireLessThanAgreesWithPermute(logRows, ordering);
    }
}

TEST_CASE("RowOrdering Floating NaN and monostate join the tail bucket", "[row_ordering][floating]")
{
    const TestLogFile fixture("row_ordering_floating.json");
    fixture.Write("");
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const std::vector<LogValue> values = {1.5, nan, std::monostate{}, -2.0, 0.0};
    const LogTable table = BuildSingleColumnTable(fixture, "x", ColumnType::Floating, values);
    const auto logRows = IotaRows(values.size());
    const RowOrdering ordering(table, 0, SortDirection::Ascending);
    RequireCompareMatchesCompareRows(table, values.size(), ordering);
    RequirePermuteMatchesSortPermutation(table, logRows, ordering);
    CHECK(ordering.Compare(1, 0) > 0); // NaN after finite
    CHECK(ordering.Compare(2, 0) > 0); // monostate after finite
    CHECK(ordering.Compare(3, 0) < 0); // -2 < 1.5
}

TEST_CASE("RowOrdering Boolean false < true and sinks non-bool to tail", "[row_ordering][boolean]")
{
    const TestLogFile fixture("row_ordering_bool.json");
    fixture.Write("");
    const std::vector<LogValue> values = {true, std::monostate{}, false, true, std::string("x")};
    const LogTable table = BuildSingleColumnTable(fixture, "ok", ColumnType::Boolean, values);
    const auto logRows = IotaRows(values.size());
    const RowOrdering ordering(table, 0, SortDirection::Ascending);
    RequireCompareMatchesCompareRows(table, values.size(), ordering);
    RequirePermuteMatchesSortPermutation(table, logRows, ordering);
    CHECK(ordering.LessThan(2, 0, 2, 0)); // false before true
}

TEST_CASE("RowOrdering String missing slots are monostate tail", "[row_ordering][string]")
{
    const TestLogFile fixture("row_ordering_string.json");
    fixture.Write("");
    const std::vector<LogValue> values = {std::string("b"), std::monostate{}, std::string("a"), std::string("b")};
    const LogTable table = BuildSingleColumnTable(fixture, "s", ColumnType::String, values);
    const auto logRows = IotaRows(values.size());
    const RowOrdering ordering(table, 0, SortDirection::Ascending);
    RequireCompareMatchesCompareRows(table, values.size(), ordering);
    RequirePermuteMatchesSortPermutation(table, logRows, ordering);
}

TEST_CASE("RowOrdering Time compares microseconds-since-epoch", "[row_ordering][time]")
{
    const TestLogFile fixture("row_ordering_time.json");
    fixture.Write("");
    const TimeStamp t0{std::chrono::microseconds(1)};
    const TimeStamp t1{std::chrono::microseconds(100)};
    const std::vector<LogValue> values = {t1, std::monostate{}, t0, t1};
    const LogTable table = BuildSingleColumnTable(fixture, "ts", ColumnType::Time, values);
    const auto logRows = IotaRows(values.size());
    const RowOrdering ordering(table, 0, SortDirection::Ascending);
    RequireCompareMatchesCompareRows(table, values.size(), ordering);
    RequirePermuteMatchesSortPermutation(table, logRows, ordering);
}

TEST_CASE("RowOrdering Enumeration builds its own ranks and keeps input-index ties", "[row_ordering][enum]")
{
    const TestLogFile fixture("row_ordering_enum.json");
    fixture.Write("");
    auto source = fixture.CreateFileLineSource();
    FileLineSource *sourcePtr = source.get();
    LogConfiguration cfg;
    cfg.columns.push_back({
        .header = "category",
        .keys = {"category"},
        .printFormat = "{}",
        .type = ColumnType::Enumeration,
        .parseFormats = {},
    });
    const ColumnLayout mgr(cfg.columns);
    loglib::LogTable table(mgr);
    table.BeginStreaming(std::move(source));

    KeyIndex &keys = table.Keys();
    RowBatch batch;
    batch.firstLineNumber = 1;
    const std::vector<std::string> rowValues = {"warn", "info", "error", "debug", "warn"};
    for (const auto &v : rowValues)
    {
        batch.lines.push_back(MakeLine(keys, *sourcePtr, {{"category", std::string(v)}}));
    }
    batch.newKeys.emplace_back("category");
    AppendFixtureBatch(table, std::move(batch));

    const KeyId categoryKey = keys.Find("category");
    REQUIRE(categoryKey != INVALID_KEY_ID);
    REQUIRE(table.EnumDictionaries().Find(categoryKey) != nullptr);

    const auto logRows = IotaRows(rowValues.size());
    const RowOrdering ordering(table, 0, SortDirection::Ascending);
    RequireCompareMatchesCompareRows(table, rowValues.size(), ordering);
    RequirePermuteMatchesSortPermutation(table, logRows, ordering);
    RequireLessThanAgreesWithPermute(logRows, ordering);

    CHECK(ordering.Compare(0, 4) == 0);
    // Equal "warn" rows: earlier input index sorts first in both directions.
    CHECK(ordering.LessThan(0, 4, 0, 4));
    const RowOrdering descending(table, 0, SortDirection::Descending);
    CHECK(descending.LessThan(0, 4, 0, 4));
    const auto perm = descending.Permute(logRows);
    REQUIRE(perm.size() == 5);
    const auto firstWarn = std::ranges::find_if(perm, [&](std::size_t idx) { return idx == 0 || idx == 4; });
    const auto secondWarn =
        std::ranges::find_if(std::next(firstWarn), perm.end(), [&](std::size_t idx) { return idx == 0 || idx == 4; });
    REQUIRE(firstWarn != perm.end());
    REQUIRE(secondWarn != perm.end());
    CHECK(*firstWarn == 0);
    CHECK(*secondWarn == 4);
}

TEST_CASE("RowOrdering Level uses canonical severity, not byte order", "[row_ordering][level]")
{
    const TestLogFile fixture("row_ordering_level.json");
    fixture.Write("");
    auto source = fixture.CreateFileLineSource();
    FileLineSource *sourcePtr = source.get();
    LogConfiguration cfg;
    cfg.columns.push_back(
        {.header = "level", .keys = {"level"}, .printFormat = "{}", .type = ColumnType::Level, .parseFormats = {}}
    );
    const ColumnLayout mgr(cfg.columns);
    loglib::LogTable table(mgr);
    table.BeginStreaming(std::move(source));
    KeyIndex &keys = table.Keys();

    RowBatch batch;
    batch.firstLineNumber = 1;
    const std::vector<std::string> rowValues = {"WARN", "INFO", "ERROR", "FATAL", "qux"};
    for (const auto &v : rowValues)
    {
        batch.lines.push_back(MakeLine(keys, *sourcePtr, {{"level", std::string(v)}}));
    }
    batch.newKeys.emplace_back("level");
    AppendFixtureBatch(table, std::move(batch));

    const auto logRows = IotaRows(rowValues.size());
    const RowOrdering ascending(table, 0, SortDirection::Ascending);
    RequireCompareMatchesCompareRows(table, rowValues.size(), ascending);
    RequirePermuteMatchesSortPermutation(table, logRows, ascending);
    CHECK(ascending.Compare(1, 0) < 0); // INFO < WARN

    const RowOrdering descending(table, 0, SortDirection::Descending);
    RequirePermuteMatchesSortPermutation(table, logRows, descending);
    const auto perm = descending.Permute(logRows);
    REQUIRE(perm.front() == 4); // unmapped tail first when descending
}

TEST_CASE("RowOrdering mixed-column table sorts the requested column only", "[row_ordering][mixed]")
{
    const TestLogFile fixture("row_ordering_mixed.json");
    fixture.Write("");
    auto source = fixture.CreateFileLineSource();
    FileLineSource *sourcePtr = source.get();
    LogConfiguration cfg;
    cfg.columns.push_back(
        {.header = "n", .keys = {"n"}, .printFormat = "{}", .type = ColumnType::Integer, .parseFormats = {}}
    );
    cfg.columns.push_back(
        {.header = "s", .keys = {"s"}, .printFormat = "{}", .type = ColumnType::String, .parseFormats = {}}
    );
    const ColumnLayout mgr(cfg.columns);
    loglib::LogTable table(mgr);
    table.BeginStreaming(std::move(source));
    KeyIndex &keys = table.Keys();

    RowBatch batch;
    batch.firstLineNumber = 1;
    const std::array<std::pair<LogValue, LogValue>, 4> rows = {
        {
            {int64_t{2}, std::string("a")},
            {std::monostate{}, std::string("c")},
            {int64_t{1}, std::string("b")},
            {int64_t{2}, std::monostate{}},
        },
    };
    for (const auto &[n, s] : rows)
    {
        batch.lines.push_back(MakeLine(keys, *sourcePtr, {{"n", n}, {"s", s}}));
    }
    batch.newKeys.emplace_back("n");
    batch.newKeys.emplace_back("s");
    AppendFixtureBatch(table, std::move(batch));

    const auto logRows = IotaRows(rows.size());
    const RowOrdering byNumber(table, 0, SortDirection::Ascending);
    const RowOrdering byString(table, 1, SortDirection::Ascending);
    RequireCompareMatchesCompareRows(table, rows.size(), byNumber);
    RequireCompareMatchesCompareRows(table, rows.size(), byString);
    RequirePermuteMatchesSortPermutation(table, logRows, byNumber);
    RequirePermuteMatchesSortPermutation(table, logRows, byString);

    const auto byN = byNumber.Permute(logRows);
    CHECK(logRows[byN[0]] == 2);     // 1
    CHECK(logRows[byN.back()] == 1); // monostate tail
}

TEST_CASE("RowOrdering input-index tie-break is stable across direction", "[row_ordering][stable]")
{
    const TestLogFile fixture("row_ordering_stable.json");
    fixture.Write("");
    const std::vector<LogValue> values = {int64_t{1}, int64_t{1}, int64_t{1}};
    const LogTable table = BuildSingleColumnTable(fixture, "n", ColumnType::Integer, values);
    const std::vector<std::size_t> logRows = {2, 0, 1};

    for (const SortDirection direction : {SortDirection::Ascending, SortDirection::Descending})
    {
        const RowOrdering ordering(table, 0, direction);
        const auto perm = ordering.Permute(logRows);
        REQUIRE(perm == std::vector<std::size_t>{0, 1, 2});
        CHECK(ordering.LessThan(logRows[0], logRows[1], 0, 1));
        CHECK(ordering.LessThan(logRows[1], logRows[2], 1, 2));
    }
}

TEST_CASE("RowOrdering LessThan and Permute agree for every pair on a mixed column with ties", "[row_ordering][mixed]")
{
    const TestLogFile fixture("row_ordering_pairs.json");
    fixture.Write("");
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const std::vector<LogValue> values = {
        int64_t{3},
        std::monostate{},
        int64_t{1},
        nan,
        int64_t{3},
        int64_t{-7},
        std::monostate{},
        int64_t{1},
        nan,
        int64_t{3},
    };
    const LogTable table = BuildSingleColumnTable(fixture, "n", ColumnType::Integer, values);
    const auto logRows = IotaRows(values.size());

    for (const SortDirection direction : {SortDirection::Ascending, SortDirection::Descending})
    {
        const RowOrdering ordering(table, 0, direction);
        RequireLessThanMatchesPermutePositionForEveryPair(logRows, ordering);
    }
}

TEST_CASE("RowOrdering on an Enumeration column builds its ranks and sorts alphabetically", "[row_ordering][enum]")
{
    const TestLogFile fixture("row_ordering_enum_rank.json");
    fixture.Write("");
    auto source = fixture.CreateFileLineSource();
    FileLineSource *sourcePtr = source.get();
    LogConfiguration cfg;
    cfg.columns.push_back({
        .header = "category",
        .keys = {"category"},
        .printFormat = "{}",
        .type = ColumnType::Enumeration,
        .parseFormats = {},
    });
    const ColumnLayout mgr(cfg.columns);
    loglib::LogTable table(mgr);
    table.BeginStreaming(std::move(source));
    KeyIndex &keys = table.Keys();

    RowBatch batch;
    batch.firstLineNumber = 1;
    // The ids are handed out in the order the values first appear, which is not the alphabetical order.
    const std::vector<std::string> rowValues = {"warn", "info", "error", "debug", "warn", "info"};
    for (const auto &v : rowValues)
    {
        batch.lines.push_back(MakeLine(keys, *sourcePtr, {{"category", std::string(v)}}));
    }
    batch.newKeys.emplace_back("category");
    AppendFixtureBatch(table, std::move(batch));

    const auto logRows = IotaRows(rowValues.size());
    const RowOrdering ascending(table, 0, SortDirection::Ascending);
    REQUIRE_FALSE(ascending.IsOutdated());
    const auto perm = ascending.Permute(logRows);
    std::vector<std::string> sorted;
    sorted.reserve(perm.size());
    for (const std::size_t index : perm)
    {
        sorted.push_back(rowValues[logRows[index]]);
    }
    CHECK(sorted == std::vector<std::string>{"debug", "error", "info", "info", "warn", "warn"});
    RequirePermuteMatchesSortPermutation(table, logRows, ascending);
    RequireLessThanMatchesPermutePositionForEveryPair(logRows, ascending);

    // The window is the same part of the same order.
    const auto window = ascending.PermuteWindow(logRows, 1, 4);
    CHECK(window == std::vector<std::size_t>(perm.begin() + 1, perm.begin() + 4));

    // A new value in the dictionary outdates the ordering, and a new ordering ranks it.
    {
        RowBatch more;
        more.firstLineNumber = 7;
        more.lines.push_back(MakeLine(keys, *sourcePtr, {{"category", std::string("alert")}}));
        AppendFixtureBatch(table, std::move(more));
    }
    CHECK(ascending.IsOutdated());
    const RowOrdering rebuilt(table, 0, SortDirection::Ascending);
    CHECK_FALSE(rebuilt.IsOutdated());
    const auto allRows = IotaRows(rowValues.size() + 1);
    const auto order = rebuilt.Permute(allRows);
    CHECK(order.front() == rowValues.size()); // "alert" sorts first
}
