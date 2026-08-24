#include "common.hpp"

#include <loglib/enum_dictionary.hpp>
#include <loglib/file_line_source.hpp>
#include <loglib/key_index.hpp>
#include <loglib/log_compare.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_line.hpp>
#include <loglib/log_parse_sink.hpp>
#include <loglib/log_table.hpp>
#include <loglib/log_value.hpp>
#include <loglib/row_ordering.hpp>

#include <catch2/catch_all.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace loglib;

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
    return {std::move(sorted), keys, source, 0};
}

LogTable BuildSingleColumnTable(
    const TestLogFile &testFile,
    const std::string &columnKey,
    LogConfiguration::Type type,
    const std::vector<LogValue> &perRowValues
)
{
    auto source = testFile.CreateFileLineSource();
    FileLineSource *sourcePtr = source.get();

    LogConfiguration cfg;
    cfg.columns.push_back(
        {.header = columnKey, .keys = {columnKey}, .printFormat = "{}", .type = type, .parseFormats = {}}
    );
    const TestLogConfiguration cfgFile;
    cfgFile.Write(cfg);
    LogConfigurationManager mgr;
    mgr.Load(cfgFile.GetFilePath());

    LogTable table({}, std::move(mgr));
    table.BeginStreaming(std::move(source));

    KeyIndex &keys = table.Keys();
    StreamedBatch batch;
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
    table.AppendBatch(std::move(batch));
    return table;
}

std::vector<std::size_t> IotaRows(std::size_t n)
{
    std::vector<std::size_t> rows(n);
    std::iota(rows.begin(), rows.end(), std::size_t{0});
    return rows;
}

void RequirePermuteMatchesSortPermutation(
    const LogTable &table, std::span<const std::size_t> logRows, const RowOrdering &ordering
)
{
    const auto viaOrdering = ordering.Permute(table, logRows);
    const auto viaLib = SortPermutationByColumn(
        table, logRows, ordering.ColumnIndex(), ordering.Ascending(), ordering.RankForEnumColumn()
    );
    REQUIRE(viaOrdering == viaLib);
}

void RequireLessThanAgreesWithPermute(const LogTable &table, std::span<const std::size_t> logRows, const RowOrdering &ordering)
{
    const auto perm = ordering.Permute(table, logRows);
    REQUIRE(perm.size() == logRows.size());
    for (std::size_t i = 1; i < perm.size(); ++i)
    {
        const std::size_t lhsIdx = perm[i - 1];
        const std::size_t rhsIdx = perm[i];
        REQUIRE(ordering.LessThan(table, logRows[lhsIdx], logRows[rhsIdx], lhsIdx, rhsIdx));
        REQUIRE_FALSE(ordering.LessThan(table, logRows[rhsIdx], logRows[lhsIdx], rhsIdx, lhsIdx));
    }
}

void RequireCompareMatchesCompareRows(
    const LogTable &table, std::size_t rowCount, const RowOrdering &ordering
)
{
    for (std::size_t a = 0; a < rowCount; ++a)
    {
        for (std::size_t b = 0; b < rowCount; ++b)
        {
            CHECK(
                ordering.Compare(table, a, b) ==
                CompareRows(table, a, b, ordering.ColumnIndex(), ordering.RankForEnumColumn())
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
    const std::vector<LogValue> values = {
        int64_t{42}, std::monostate{}, int64_t{-1}, nan, int64_t{0}, int64_t{42}
    };
    const LogTable table = BuildSingleColumnTable(fixture, "n", LogConfiguration::Type::Integer, values);
    const auto logRows = IotaRows(values.size());

    for (const SortDirection direction : {SortDirection::Ascending, SortDirection::Descending})
    {
        const RowOrdering ordering(0, direction);
        RequireCompareMatchesCompareRows(table, values.size(), ordering);
        RequirePermuteMatchesSortPermutation(table, logRows, ordering);
        RequireLessThanAgreesWithPermute(table, logRows, ordering);
    }
}

TEST_CASE("RowOrdering Floating NaN and monostate join the tail bucket", "[row_ordering][floating]")
{
    const TestLogFile fixture("row_ordering_floating.json");
    fixture.Write("");
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const std::vector<LogValue> values = {1.5, nan, std::monostate{}, -2.0, 0.0};
    const LogTable table = BuildSingleColumnTable(fixture, "x", LogConfiguration::Type::Floating, values);
    const auto logRows = IotaRows(values.size());
    const RowOrdering ordering(0, SortDirection::Ascending);
    RequireCompareMatchesCompareRows(table, values.size(), ordering);
    RequirePermuteMatchesSortPermutation(table, logRows, ordering);
    CHECK(ordering.Compare(table, 1, 0) > 0); // NaN after finite
    CHECK(ordering.Compare(table, 2, 0) > 0); // monostate after finite
    CHECK(ordering.Compare(table, 3, 0) < 0); // -2 < 1.5
}

TEST_CASE("RowOrdering Boolean false < true and sinks non-bool to tail", "[row_ordering][boolean]")
{
    const TestLogFile fixture("row_ordering_bool.json");
    fixture.Write("");
    const std::vector<LogValue> values = {true, std::monostate{}, false, true, std::string("x")};
    const LogTable table = BuildSingleColumnTable(fixture, "ok", LogConfiguration::Type::Boolean, values);
    const auto logRows = IotaRows(values.size());
    const RowOrdering ordering(0, SortDirection::Ascending);
    RequireCompareMatchesCompareRows(table, values.size(), ordering);
    RequirePermuteMatchesSortPermutation(table, logRows, ordering);
    CHECK(ordering.LessThan(table, 2, 0, 2, 0)); // false before true
}

TEST_CASE("RowOrdering String missing slots are monostate tail", "[row_ordering][string]")
{
    const TestLogFile fixture("row_ordering_string.json");
    fixture.Write("");
    const std::vector<LogValue> values = {std::string("b"), std::monostate{}, std::string("a"), std::string("b")};
    const LogTable table = BuildSingleColumnTable(fixture, "s", LogConfiguration::Type::String, values);
    const auto logRows = IotaRows(values.size());
    const RowOrdering ordering(0, SortDirection::Ascending);
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
    const LogTable table = BuildSingleColumnTable(fixture, "ts", LogConfiguration::Type::Time, values);
    const auto logRows = IotaRows(values.size());
    const RowOrdering ordering(0, SortDirection::Ascending);
    RequireCompareMatchesCompareRows(table, values.size(), ordering);
    RequirePermuteMatchesSortPermutation(table, logRows, ordering);
}

TEST_CASE("RowOrdering Enumeration uses EnumDictRank and keeps input-index ties", "[row_ordering][enum]")
{
    const TestLogFile fixture("row_ordering_enum.json");
    fixture.Write("");
    auto source = fixture.CreateFileLineSource();
    FileLineSource *sourcePtr = source.get();
    LogConfiguration cfg;
    cfg.columns.push_back(
        {.header = "category",
         .keys = {"category"},
         .printFormat = "{}",
         .type = LogConfiguration::Type::Enumeration,
         .parseFormats = {}}
    );
    const TestLogConfiguration cfgFile;
    cfgFile.Write(cfg);
    LogConfigurationManager mgr;
    mgr.Load(cfgFile.GetFilePath());
    LogTable table({}, std::move(mgr));
    table.BeginStreaming(std::move(source));

    KeyIndex &keys = table.Keys();
    StreamedBatch batch;
    batch.firstLineNumber = 1;
    const std::vector<std::string> rowValues = {"warn", "info", "error", "debug", "warn"};
    for (const auto &v : rowValues)
    {
        batch.lines.push_back(MakeLine(keys, *sourcePtr, {{"category", std::string(v)}}));
    }
    batch.newKeys.emplace_back("category");
    table.AppendBatch(std::move(batch));

    const KeyId categoryKey = keys.Find("category");
    REQUIRE(categoryKey != INVALID_KEY_ID);
    const EnumDictionary *dict = table.EnumDictionaries().Find(categoryKey);
    REQUIRE(dict != nullptr);
    const EnumDictRank rank{*dict};

    const auto logRows = IotaRows(rowValues.size());
    const RowOrdering ordering(0, SortDirection::Ascending, &rank);
    RequireCompareMatchesCompareRows(table, rowValues.size(), ordering);
    RequirePermuteMatchesSortPermutation(table, logRows, ordering);
    RequireLessThanAgreesWithPermute(table, logRows, ordering);

    CHECK(ordering.Compare(table, 0, 4) == 0);
    // Equal "warn" rows: earlier input index sorts first in both directions.
    CHECK(ordering.LessThan(table, 0, 4, 0, 4));
    const RowOrdering descending(0, SortDirection::Descending, &rank);
    CHECK(descending.LessThan(table, 0, 4, 0, 4));
    const auto perm = descending.Permute(table, logRows);
    REQUIRE(perm.size() == 5);
    const auto firstWarn = std::ranges::find_if(perm, [&](std::size_t idx) { return idx == 0 || idx == 4; });
    const auto secondWarn = std::ranges::find_if(std::next(firstWarn), perm.end(), [&](std::size_t idx) {
        return idx == 0 || idx == 4;
    });
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
        {.header = "level",
         .keys = {"level"},
         .printFormat = "{}",
         .type = LogConfiguration::Type::Level,
         .parseFormats = {}}
    );
    const TestLogConfiguration cfgFile;
    cfgFile.Write(cfg);
    LogConfigurationManager mgr;
    mgr.Load(cfgFile.GetFilePath());
    LogTable table({}, std::move(mgr));
    table.BeginStreaming(std::move(source));
    KeyIndex &keys = table.Keys();

    StreamedBatch batch;
    batch.firstLineNumber = 1;
    const std::vector<std::string> rowValues = {"WARN", "INFO", "ERROR", "FATAL", "qux"};
    for (const auto &v : rowValues)
    {
        batch.lines.push_back(MakeLine(keys, *sourcePtr, {{"level", std::string(v)}}));
    }
    batch.newKeys.emplace_back("level");
    table.AppendBatch(std::move(batch));

    const auto logRows = IotaRows(rowValues.size());
    const RowOrdering ascending(0, SortDirection::Ascending);
    RequireCompareMatchesCompareRows(table, rowValues.size(), ascending);
    RequirePermuteMatchesSortPermutation(table, logRows, ascending);
    CHECK(ascending.Compare(table, 1, 0) < 0); // INFO < WARN

    const RowOrdering descending(0, SortDirection::Descending);
    RequirePermuteMatchesSortPermutation(table, logRows, descending);
    const auto perm = descending.Permute(table, logRows);
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
        {.header = "n", .keys = {"n"}, .printFormat = "{}", .type = LogConfiguration::Type::Integer, .parseFormats = {}}
    );
    cfg.columns.push_back(
        {.header = "s", .keys = {"s"}, .printFormat = "{}", .type = LogConfiguration::Type::String, .parseFormats = {}}
    );
    const TestLogConfiguration cfgFile;
    cfgFile.Write(cfg);
    LogConfigurationManager mgr;
    mgr.Load(cfgFile.GetFilePath());
    LogTable table({}, std::move(mgr));
    table.BeginStreaming(std::move(source));
    KeyIndex &keys = table.Keys();

    StreamedBatch batch;
    batch.firstLineNumber = 1;
    const std::array<std::pair<LogValue, LogValue>, 4> rows = {{
        {int64_t{2}, std::string("a")},
        {std::monostate{}, std::string("c")},
        {int64_t{1}, std::string("b")},
        {int64_t{2}, std::monostate{}},
    }};
    for (const auto &[n, s] : rows)
    {
        batch.lines.push_back(MakeLine(keys, *sourcePtr, {{"n", n}, {"s", s}}));
    }
    batch.newKeys.emplace_back("n");
    batch.newKeys.emplace_back("s");
    table.AppendBatch(std::move(batch));

    const auto logRows = IotaRows(rows.size());
    const RowOrdering byNumber(0, SortDirection::Ascending);
    const RowOrdering byString(1, SortDirection::Ascending);
    RequireCompareMatchesCompareRows(table, rows.size(), byNumber);
    RequireCompareMatchesCompareRows(table, rows.size(), byString);
    RequirePermuteMatchesSortPermutation(table, logRows, byNumber);
    RequirePermuteMatchesSortPermutation(table, logRows, byString);

    const auto byN = byNumber.Permute(table, logRows);
    CHECK(logRows[byN[0]] == 2); // 1
    CHECK(logRows[byN.back()] == 1); // monostate tail
}

TEST_CASE("RowOrdering input-index tie-break is stable across direction", "[row_ordering][stable]")
{
    const TestLogFile fixture("row_ordering_stable.json");
    fixture.Write("");
    const std::vector<LogValue> values = {int64_t{1}, int64_t{1}, int64_t{1}};
    const LogTable table = BuildSingleColumnTable(fixture, "n", LogConfiguration::Type::Integer, values);
    const std::vector<std::size_t> logRows = {2, 0, 1};

    for (const SortDirection direction : {SortDirection::Ascending, SortDirection::Descending})
    {
        const RowOrdering ordering(0, direction);
        const auto perm = ordering.Permute(table, logRows);
        REQUIRE(perm == std::vector<std::size_t>{0, 1, 2});
        CHECK(ordering.LessThan(table, logRows[0], logRows[1], 0, 1));
        CHECK(ordering.LessThan(table, logRows[1], logRows[2], 1, 2));
    }
}
