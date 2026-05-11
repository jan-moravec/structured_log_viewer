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

#include <catch2/catch_all.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
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

/// Build a single-column table of @p type with rows seeded from @p values
/// (each entry is `(key, LogValue)` for that row).
LogTable BuildSingleColumnTable(
    const TestLogFile &testFile,
    const std::string &columnKey,
    LogConfiguration::Type type,
    const std::vector<LogValue> &perRowValues,
    std::string printFormat = "{}"
)
{
    auto source = testFile.CreateFileLineSource();
    FileLineSource *sourcePtr = source.get();

    LogConfiguration cfg;
    cfg.columns.push_back(
        {.header = columnKey,
         .keys = {columnKey},
         .printFormat = std::move(printFormat),
         .type = type,
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

int SignOf(int v)
{
    return (v > 0) - (v < 0);
}

} // namespace

TEST_CASE("EnumDictRank orders ids alphabetically by their resolved bytes", "[log_compare][enum_dict_rank]")
{
    EnumDictionary dict{16};
    // Insertion order intentionally not alphabetical.
    const EnumValueId warnId = dict.Insert("warn");
    const EnumValueId infoId = dict.Insert("info");
    const EnumValueId errorId = dict.Insert("error");
    const EnumValueId debugId = dict.Insert("debug");

    EnumDictRank rank{dict};
    REQUIRE(rank.DictSize() == 4);

    // Alphabetic order: debug < error < info < warn.
    CHECK(rank.RankOf(debugId) == 0);
    CHECK(rank.RankOf(errorId) == 1);
    CHECK(rank.RankOf(infoId) == 2);
    CHECK(rank.RankOf(warnId) == 3);

    // Past-the-end id sorts after every known value.
    CHECK(rank.RankOf(EnumValueId{rank.DictSize()}) == rank.DictSize());
}

TEST_CASE("EnumDictRank handles an empty dictionary", "[log_compare][enum_dict_rank]")
{
    const EnumDictionary dict{16};
    const EnumDictRank rank{dict};
    CHECK(rank.DictSize() == 0);
    CHECK(rank.Empty());
}

TEST_CASE("CompareRows handles Integer columns with monostate-tail order", "[log_compare][integer]")
{
    const TestLogFile fixture("log_compare_integer.json");
    fixture.Write("");
    const std::vector<LogValue> values = {int64_t{3}, int64_t{1}, std::monostate{}, int64_t{2}};
    LogTable table = BuildSingleColumnTable(fixture, "n", LogConfiguration::Type::Integer, values);

    // 1 < 2 < 3 < monostate
    CHECK(SignOf(CompareRows(table, 0, 1, 0)) == 1);  // 3 vs 1
    CHECK(SignOf(CompareRows(table, 1, 3, 0)) == -1); // 1 vs 2
    CHECK(SignOf(CompareRows(table, 0, 0, 0)) == 0);  // equal row
    CHECK(SignOf(CompareRows(table, 2, 0, 0)) == 1);  // monostate > 3
    CHECK(SignOf(CompareRows(table, 0, 2, 0)) == -1); // 3 < monostate
    CHECK(SignOf(CompareRows(table, 2, 2, 0)) == 0);  // monostate == monostate
}

TEST_CASE("CompareRows handles Floating columns with NaN at the tail", "[log_compare][floating]")
{
    const TestLogFile fixture("log_compare_floating.json");
    fixture.Write("");
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const std::vector<LogValue> values = {1.5, -2.0, nan, 4.25};
    LogTable table = BuildSingleColumnTable(fixture, "x", LogConfiguration::Type::Floating, values);

    CHECK(SignOf(CompareRows(table, 1, 0, 0)) == -1); // -2 < 1.5
    CHECK(SignOf(CompareRows(table, 0, 3, 0)) == -1); // 1.5 < 4.25
    CHECK(SignOf(CompareRows(table, 2, 0, 0)) == 1);  // NaN > 1.5
    CHECK(SignOf(CompareRows(table, 2, 2, 0)) == 0);  // NaN == NaN
}

TEST_CASE("CompareRows handles Time columns by microseconds-since-epoch", "[log_compare][time]")
{
    const TestLogFile fixture("log_compare_time.json");
    fixture.Write("");
    const TimeStamp t100{std::chrono::microseconds{100}};
    const TimeStamp t300{std::chrono::microseconds{300}};
    const TimeStamp t200{std::chrono::microseconds{200}};
    const std::vector<LogValue> values = {t100, t300, t200, std::monostate{}};
    LogTable table = BuildSingleColumnTable(fixture, "ts", LogConfiguration::Type::Time, values, "{:%FT%T}");

    CHECK(SignOf(CompareRows(table, 0, 1, 0)) == -1); // 100 < 300
    CHECK(SignOf(CompareRows(table, 1, 2, 0)) == 1);  // 300 > 200
    CHECK(SignOf(CompareRows(table, 0, 2, 0)) == -1); // 100 < 200
    CHECK(SignOf(CompareRows(table, 3, 0, 0)) == 1);  // monostate > populated
}

TEST_CASE("CompareRows on an Enumeration column uses the rank table", "[log_compare][enum]")
{
    const TestLogFile fixture("log_compare_enum.json");
    fixture.Write("");

    auto source = fixture.CreateFileLineSource();
    FileLineSource *sourcePtr = source.get();
    LogConfiguration cfg;
    cfg.columns.push_back(
        {.header = "level",
         .keys = {"level"},
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
    // Insertion order is the dict order: warn=0, info=1, error=2, debug=3.
    // Alphabetic rank should re-order to debug<error<info<warn.
    const std::vector<std::string> rowValues = {"warn", "info", "error", "debug", "warn"};
    for (const auto &v : rowValues)
    {
        batch.lines.push_back(MakeLine(keys, *sourcePtr, {{"level", std::string(v)}}));
    }
    batch.newKeys.emplace_back("level");
    table.AppendBatch(std::move(batch));

    REQUIRE(table.Configuration().Configuration().columns[0].type == LogConfiguration::Type::Enumeration);
    const KeyId levelKey = keys.Find("level");
    REQUIRE(levelKey != INVALID_KEY_ID);
    const EnumDictionary *dict = table.EnumDictionaries().Find(levelKey);
    REQUIRE(dict != nullptr);

    EnumDictRank rank{*dict};

    // Row 0 = warn, row 1 = info; alphabetic: info < warn so (0 vs 1) > 0.
    CHECK(SignOf(CompareRows(table, 0, 1, 0, &rank)) == 1);
    // Row 3 = debug, row 2 = error; alphabetic: debug < error.
    CHECK(SignOf(CompareRows(table, 3, 2, 0, &rank)) == -1);
    // Same value tie -> 0.
    CHECK(SignOf(CompareRows(table, 0, 4, 0, &rank)) == 0);
}

TEST_CASE("CompareRows on enum column without rank falls back to string compare", "[log_compare][enum][fallback]")
{
    const TestLogFile fixture("log_compare_enum_no_rank.json");
    fixture.Write("");
    auto source = fixture.CreateFileLineSource();
    FileLineSource *sourcePtr = source.get();
    LogConfiguration cfg;
    cfg.columns.push_back(
        {.header = "level",
         .keys = {"level"},
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
    const std::vector<std::string> rowValues = {"warn", "info", "error"};
    for (const auto &v : rowValues)
    {
        batch.lines.push_back(MakeLine(keys, *sourcePtr, {{"level", std::string(v)}}));
    }
    batch.newKeys.emplace_back("level");
    table.AppendBatch(std::move(batch));

    // Compare resolved bytes directly. error < info < warn.
    CHECK(SignOf(CompareRows(table, 2, 1, 0, /*rankForEnumColumn=*/nullptr)) == -1); // error < info
    CHECK(SignOf(CompareRows(table, 1, 0, 0, /*rankForEnumColumn=*/nullptr)) == -1); // info < warn
}

TEST_CASE("CompareRows on String/Any columns compares byte-wise", "[log_compare][string]")
{
    const TestLogFile fixture("log_compare_string.json");
    fixture.Write("");
    const std::vector<LogValue> values = {
        std::string("delta"),
        std::string("alpha"),
        std::string("charlie"),
        std::monostate{},
    };
    LogTable table = BuildSingleColumnTable(fixture, "label", LogConfiguration::Type::Any, values);

    CHECK(SignOf(CompareRows(table, 0, 1, 0)) == 1);  // delta > alpha
    CHECK(SignOf(CompareRows(table, 2, 0, 0)) == -1); // charlie < delta
    CHECK(SignOf(CompareRows(table, 3, 0, 0)) == 1);  // monostate > populated
    CHECK(SignOf(CompareRows(table, 0, 3, 0)) == -1);
}

TEST_CASE("CompareRows is total: a<b iff b>a, and a=a", "[log_compare][properties]")
{
    const TestLogFile fixture("log_compare_total_order.json");
    fixture.Write("");
    const std::vector<LogValue> values = {int64_t{42}, int64_t{-7}, std::monostate{}, int64_t{42}};
    LogTable table = BuildSingleColumnTable(fixture, "n", LogConfiguration::Type::Integer, values);

    for (size_t a = 0; a < table.RowCount(); ++a)
    {
        for (size_t b = 0; b < table.RowCount(); ++b)
        {
            const int forward = SignOf(CompareRows(table, a, b, 0));
            const int reverse = SignOf(CompareRows(table, b, a, 0));
            INFO("a=" << a << " b=" << b);
            CHECK(forward == -reverse);
        }
        CHECK(CompareRows(table, a, a, 0) == 0);
    }
}
