#include "common.hpp"

#include <loglib/enum_dictionary.hpp>
#include <loglib/file_line_source.hpp>
#include <loglib/key_index.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_filter.hpp>
#include <loglib/log_level.hpp>
#include <loglib/log_line.hpp>
#include <loglib/log_parse_sink.hpp>
#include <loglib/log_table.hpp>
#include <loglib/log_value.hpp>

#include <catch2/catch_all.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace loglib;

namespace
{

/// `LogLine` from `(key, value)` pairs; resolves keys via `GetOrInsert`.
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

/// Single-column `Type::Enumeration` `LogTable`; rows cycle @p distinctValues.
LogTable BuildEnumTable(
    const TestLogFile &testFile,
    const std::string &columnKey,
    const std::vector<std::string> &distinctValues,
    size_t rowCount
)
{
    auto source = testFile.CreateFileLineSource();
    FileLineSource *sourcePtr = source.get();

    LogConfiguration cfg;
    cfg.columns.push_back(
        {.header = columnKey,
         .keys = {columnKey},
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
    batch.lines.reserve(rowCount);
    for (size_t i = 0; i < rowCount; ++i)
    {
        batch.lines.push_back(
            MakeLine(keys, *sourcePtr, {{columnKey, std::string(distinctValues[i % distinctValues.size()])}})
        );
    }
    if (keys.Size() > 0)
    {
        batch.newKeys.emplace_back(columnKey);
    }
    table.AppendBatch(std::move(batch));
    return table;
}

/// Single-column `Type::Any + autoDetect=false` `LogTable`; values
/// land as strings (no auto-promotion) so the string-fallback path
/// can be exercised.
LogTable BuildStringTable(
    const TestLogFile &testFile, const std::string &columnKey, const std::vector<std::string> &perRowValues
)
{
    auto source = testFile.CreateFileLineSource();
    FileLineSource *sourcePtr = source.get();

    LogConfiguration cfg;
    cfg.columns.push_back(
        {.header = columnKey,
         .keys = {columnKey},
         .printFormat = "{}",
         .type = LogConfiguration::Type::Any,
         .parseFormats = {},
         .visible = true,
         .levelMapping = {},
         .autoDetect = false}
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
        batch.lines.push_back(MakeLine(keys, *sourcePtr, {{columnKey, std::string(v)}}));
    }
    if (!perRowValues.empty())
    {
        batch.newKeys.emplace_back(columnKey);
    }
    table.AppendBatch(std::move(batch));
    return table;
}

/// Single-column `Type::Time` `LogTable` from microseconds since epoch.
LogTable BuildTimeTable(
    const TestLogFile &testFile, const std::string &columnKey, const std::vector<int64_t> &microsSinceEpoch
)
{
    auto source = testFile.CreateFileLineSource();
    FileLineSource *sourcePtr = source.get();

    LogConfiguration cfg;
    cfg.columns.push_back(
        {.header = columnKey,
         .keys = {columnKey},
         .printFormat = "{:%FT%T}",
         .type = LogConfiguration::Type::Time,
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
    batch.lines.reserve(microsSinceEpoch.size());
    for (const int64_t us : microsSinceEpoch)
    {
        const TimeStamp ts{std::chrono::microseconds{us}};
        batch.lines.push_back(MakeLine(keys, *sourcePtr, {{columnKey, ts}}));
    }
    if (!microsSinceEpoch.empty())
    {
        batch.newKeys.emplace_back(columnKey);
    }
    table.AppendBatch(std::move(batch));
    return table;
}

const EnumDictionary *FindDictionary(const LogTable &table, const std::string &columnKey)
{
    const KeyId keyId = table.Keys().Find(columnKey);
    if (keyId == INVALID_KEY_ID)
    {
        return nullptr;
    }
    return table.EnumDictionaries().Find(keyId);
}

std::vector<std::string_view> ToViews(const std::vector<std::string> &values)
{
    std::vector<std::string_view> views;
    views.reserve(values.size());
    for (const auto &v : values)
    {
        views.emplace_back(v);
    }
    return views;
}

} // namespace

TEST_CASE("EnumRowPredicate accepts rows whose value is in the selection", "[log_filter][enum]")
{
    // Non-level key (`category`) keeps the column Enumeration; a
    // `level`-named column with the same values would auto-promote
    // to Level and break the REQUIRE below.
    const TestLogFile fixture("log_filter_enum_accept.json");
    fixture.Write("");
    const std::vector<std::string> values = {"info", "warn", "error"};
    LogTable table = BuildEnumTable(fixture, "category", values, 12);
    REQUIRE(table.Configuration().Configuration().columns[0].type == LogConfiguration::Type::Enumeration);
    const EnumDictionary *dict = FindDictionary(table, "category");
    REQUIRE(dict != nullptr);

    const std::vector<std::string> selected = {"warn", "error"};
    const std::vector<std::string_view> selectedViews = ToViews(selected);
    const EnumRowPredicate predicate(0, selectedViews, dict);
    REQUIRE(predicate.IsFastPathArmed());

    // Rows cycle [info, warn, error, info, warn, error, ...].
    for (size_t row = 0; row < table.RowCount(); ++row)
    {
        const std::string_view expected = values[row % values.size()];
        const bool selectedRow = expected != "info";
        INFO("row=" << row << " value=" << expected);
        CHECK(predicate.MatchesRow(table, row) == selectedRow);
    }
}

TEST_CASE("EnumRowPredicate rejects every row on an empty selection", "[log_filter][enum]")
{
    const TestLogFile fixture("log_filter_enum_empty.json");
    fixture.Write("");
    const LogTable table = BuildEnumTable(fixture, "level", {"info", "warn"}, 6);
    const EnumDictionary *dict = FindDictionary(table, "level");
    REQUIRE(dict != nullptr);

    const std::vector<std::string_view> empty;
    const EnumRowPredicate predicate(0, empty, dict);
    REQUIRE_FALSE(predicate.IsFastPathArmed());

    for (size_t row = 0; row < table.RowCount(); ++row)
    {
        CHECK_FALSE(predicate.MatchesRow(table, row));
    }
}

TEST_CASE(
    "EnumRowPredicate falls back to the string set when the column isn't promoted", "[log_filter][enum][fallback]"
)
{
    const TestLogFile fixture("log_filter_enum_string_fallback.json");
    fixture.Write("");
    const std::vector<std::string> values = {"alpha", "beta", "gamma", "alpha", "beta"};
    LogTable table = BuildStringTable(fixture, "label", values);
    REQUIRE(table.Configuration().Configuration().columns[0].type != LogConfiguration::Type::Enumeration);

    const std::vector<std::string> selected = {"alpha", "gamma"};
    const std::vector<std::string_view> selectedViews = ToViews(selected);
    const EnumRowPredicate predicate(0, selectedViews, /*dictionary=*/nullptr);
    REQUIRE_FALSE(predicate.IsFastPathArmed());

    const std::array<bool, 5> expected = {true, false, true, true, false};
    for (size_t row = 0; row < expected.size(); ++row)
    {
        INFO("row=" << row);
        CHECK(predicate.MatchesRow(table, row) == expected[row]);
    }
}

TEST_CASE("EnumRowPredicate rejects out-of-range ids when the bitset is armed", "[log_filter][enum][post_dict_growth]")
{
    // Fully-resolved predicate: an id past the bitset is provably for
    // an unselected value, so the predicate must reject (not fall
    // through to the string set).
    const TestLogFile fixture("log_filter_enum_oob.json");
    fixture.Write("");
    LogTable table = BuildEnumTable(fixture, "level", {"info", "warn"}, 4);
    const EnumDictionary *dict = FindDictionary(table, "level");
    REQUIRE(dict != nullptr);
    REQUIRE(dict->Size() == 2);

    const std::vector<std::string> selected = {"info"};
    const std::vector<std::string_view> selectedViews = ToViews(selected);
    const EnumRowPredicate predicate(0, selectedViews, dict);
    REQUIRE(predicate.IsFastPathArmed());

    // Append a batch whose value mints a new id past the bitset.
    KeyIndex &keys = table.Keys();
    auto source = std::make_unique<FileLineSource>(std::make_unique<LogFile>(fixture.GetFilePath()));
    FileLineSource *sourcePtr = source.get();
    table.AppendStreaming(std::move(source));
    StreamedBatch batch;
    batch.firstLineNumber = table.RowCount() + 1;
    batch.lines.push_back(MakeLine(keys, *sourcePtr, {{"level", std::string("debug")}}));
    table.AppendBatch(std::move(batch));

    const KeyId levelKey = keys.Find("level");
    REQUIRE(levelKey != INVALID_KEY_ID);
    REQUIRE(table.EnumDictionaries().Find(levelKey)->Size() == 3);

    const size_t newRow = table.RowCount() - 1;
    const auto newId = table.GetEnumValueId(newRow, 0);
    REQUIRE(newId.has_value());
    REQUIRE(static_cast<size_t>(*newId) >= 2); // past the predicate's bitset

    // Stale predicate must still reject the new id (it's "debug", not
    // "info") instead of falling through to the empty display path.
    CHECK_FALSE(predicate.MatchesRow(table, newRow));

    // Pre-existing rows continue to match correctly.
    for (size_t row = 0; row < 4; ++row)
    {
        const bool isInfo = (row % 2 == 0);
        CHECK(predicate.MatchesRow(table, row) == isInfo);
    }
}

TEST_CASE(
    "EnumRowPredicate falls through to the string set for a stale predicate's unresolved selected value",
    "[log_filter][enum][post_dict_growth]"
)
{
    // Regression: a selected value that was unresolved at construction
    // and is later interned past the bitset must still match via the
    // string-set fallback. Predicate correctness must not rely on the
    // GUI's `enumColumnsChanged` rebuild gate.
    const TestLogFile fixture("log_filter_enum_stale.json");
    fixture.Write("");
    LogTable table = BuildEnumTable(fixture, "level", {"info"}, 2);
    const EnumDictionary *dict = FindDictionary(table, "level");
    REQUIRE(dict != nullptr);
    REQUIRE(dict->Size() == 1);

    // Select "info" (resolves) and "debug" (unresolved at construction).
    // `mAllResolved == false`, so the past-bitset branch should fall
    // through to `mSelectedStrings`.
    const std::vector<std::string> selected = {"info", "debug"};
    const std::vector<std::string_view> selectedViews = ToViews(selected);
    const EnumRowPredicate predicate(0, selectedViews, dict);
    REQUIRE(predicate.IsFastPathArmed());

    // Append a batch interning "debug" past the bitset, then verify the
    // stale predicate accepts the new row via the string fallback.
    KeyIndex &keys = table.Keys();
    auto source = std::make_unique<FileLineSource>(std::make_unique<LogFile>(fixture.GetFilePath()));
    FileLineSource *sourcePtr = source.get();
    table.AppendStreaming(std::move(source));
    StreamedBatch batch;
    batch.firstLineNumber = table.RowCount() + 1;
    batch.lines.push_back(MakeLine(keys, *sourcePtr, {{"level", std::string("debug")}}));
    table.AppendBatch(std::move(batch));

    const KeyId levelKey = keys.Find("level");
    REQUIRE(levelKey != INVALID_KEY_ID);
    REQUIRE(table.EnumDictionaries().Find(levelKey)->Size() == 2);

    const size_t debugRow = table.RowCount() - 1;
    const auto debugId = table.GetEnumValueId(debugRow, 0);
    REQUIRE(debugId.has_value());
    REQUIRE(static_cast<size_t>(*debugId) >= 1); // past the original bitset

    // String-set fallback contains "debug" -> match.
    CHECK(predicate.MatchesRow(table, debugRow));

    // The pre-existing "info" rows continue to match through the bitset.
    for (size_t row = 0; row < 2; ++row)
    {
        CHECK(predicate.MatchesRow(table, row));
    }
}

// Lifetime trip-wire: the predicate must copy/index the selected
// values, not retain `string_view`s into caller-owned bytes.
// `MainWindow::UpdateFilters` builds the span on the stack and lets
// the predicate outlive it; ASan would catch a regression here.
TEST_CASE("EnumRowPredicate does not retain references into the selection span", "[log_filter][enum][lifetime]")
{
    const TestLogFile fixture("log_filter_enum_lifetime.json");
    fixture.Write("");
    // Two values so one selected value resolves (bitset) and one
    // doesn't (string-set fallback) -- exercises both storage paths.
    const LogTable table = BuildEnumTable(fixture, "level", {"info", "warn"}, 4);
    const EnumDictionary *dict = FindDictionary(table, "level");
    REQUIRE(dict != nullptr);

    // Build from views over strings that we then overwrite in place.
    // A retained view would read the mangled bytes on subsequent calls.
    auto buildPredicate = [&]() {
        std::vector<std::string> scratch = {"info", "completely_unrelated_label_that_does_not_exist"};
        const std::vector<std::string_view> views = ToViews(scratch);
        EnumRowPredicate built(0, views, dict);
        for (std::string &s : scratch)
        {
            std::ranges::fill(s, 'X');
        }
        return built;
    };
    const EnumRowPredicate predicate = buildPredicate();
    REQUIRE(predicate.IsFastPathArmed());

    // Fixture cycles [info, warn, info, warn]. "info" selected -> even
    // rows match.
    REQUIRE(table.RowCount() == 4);
    for (size_t row = 0; row < table.RowCount(); ++row)
    {
        const bool expected = (row % 2 == 0);
        INFO("row=" << row);
        CHECK(predicate.MatchesRow(table, row) == expected);
    }
}

TEST_CASE(
    "EnumRowPredicate dedupes duplicate selected values when accounting `mAllResolved`", "[log_filter][enum][dedupe]"
)
{
    // `mAllResolved` is keyed on *distinct* selected values, not raw
    // input length. Duplicate input must not flip the resolved-vs-
    // unresolved accounting (otherwise stale-predicate behaviour
    // diverges from caller-deduped vs caller-not-deduped paths).
    const TestLogFile fixture("log_filter_enum_dedupe.json");
    fixture.Write("");
    LogTable table = BuildEnumTable(fixture, "level", {"info", "warn"}, 4);
    const EnumDictionary *dict = FindDictionary(table, "level");
    REQUIRE(dict != nullptr);
    REQUIRE(dict->Size() == 2);

    SECTION("duplicates of an unresolved value still trigger the string-set fallback")
    {
        // ["info", "info", "debug"]: only "info" resolves; the
        // predicate must NOT report fully-resolved or "debug"'s
        // future id would be rejected past the bitset.
        const std::vector<std::string> selected = {"info", "info", "debug"};
        const std::vector<std::string_view> selectedViews = ToViews(selected);
        const EnumRowPredicate predicate(0, selectedViews, dict);
        REQUIRE(predicate.IsFastPathArmed());

        KeyIndex &keys = table.Keys();
        auto source = std::make_unique<FileLineSource>(std::make_unique<LogFile>(fixture.GetFilePath()));
        FileLineSource *sourcePtr = source.get();
        table.AppendStreaming(std::move(source));
        StreamedBatch batch;
        batch.firstLineNumber = table.RowCount() + 1;
        batch.lines.push_back(MakeLine(keys, *sourcePtr, {{"level", std::string("debug")}}));
        table.AppendBatch(std::move(batch));

        const size_t debugRow = table.RowCount() - 1;
        const auto debugId = table.GetEnumValueId(debugRow, 0);
        REQUIRE(debugId.has_value());
        REQUIRE(static_cast<size_t>(*debugId) >= 2); // past the bitset

        // `mAllResolved == false`, so the past-bitset branch must
        // fall through to `mSelectedStrings` and match "debug".
        CHECK(predicate.MatchesRow(table, debugRow));
    }

    SECTION("duplicates of resolved values still register as fully resolved")
    {
        // ["info", "info"]: only one distinct selected value, fully
        // resolved. Past-bitset ids must short-circuit to reject
        // (rather than fall through an empty string-set).
        const std::vector<std::string> selected = {"info", "info"};
        const std::vector<std::string_view> selectedViews = ToViews(selected);
        const EnumRowPredicate predicate(0, selectedViews, dict);
        REQUIRE(predicate.IsFastPathArmed());

        KeyIndex &keys = table.Keys();
        auto source = std::make_unique<FileLineSource>(std::make_unique<LogFile>(fixture.GetFilePath()));
        FileLineSource *sourcePtr = source.get();
        table.AppendStreaming(std::move(source));
        StreamedBatch batch;
        batch.firstLineNumber = table.RowCount() + 1;
        batch.lines.push_back(MakeLine(keys, *sourcePtr, {{"level", std::string("debug")}}));
        table.AppendBatch(std::move(batch));

        const size_t debugRow = table.RowCount() - 1;
        const auto debugId = table.GetEnumValueId(debugRow, 0);
        REQUIRE(debugId.has_value());
        REQUIRE(static_cast<size_t>(*debugId) >= 2);

        // `mAllResolved == true` (the only distinct value "info"
        // resolved), so the past-bitset id is provably unselected.
        CHECK_FALSE(predicate.MatchesRow(table, debugRow));
    }
}

TEST_CASE("TimeRangeRowPredicate accepts the inclusive range and rejects outside", "[log_filter][time]")
{
    const TestLogFile fixture("log_filter_time.json");
    fixture.Write("");
    const std::vector<int64_t> times = {100, 200, 300, 400, 500};
    const LogTable table = BuildTimeTable(fixture, "ts", times);

    const TimeRangeRowPredicate predicate(0, /*begin=*/200, /*end=*/400);
    const std::array<bool, 5> expected = {false, true, true, true, false};
    for (size_t row = 0; row < expected.size(); ++row)
    {
        INFO("row=" << row << " ts=" << times[row]);
        CHECK(predicate.MatchesRow(table, row) == expected[row]);
    }
}

TEST_CASE("TimeRangeRowPredicate rejects non-time columns", "[log_filter][time]")
{
    const TestLogFile fixture("log_filter_time_wrong_column.json");
    fixture.Write("");
    const LogTable table = BuildStringTable(fixture, "label", {"alpha", "beta"});

    const TimeRangeRowPredicate predicate(0, /*begin=*/0, /*end=*/1'000'000);
    CHECK_FALSE(predicate.MatchesRow(table, 0));
    CHECK_FALSE(predicate.MatchesRow(table, 1));
}

TEST_CASE("CallbackStringRowPredicate forwards string slots to the callback", "[log_filter][callback]")
{
    const TestLogFile fixture("log_filter_callback_string.json");
    fixture.Write("");
    const std::vector<std::string> values = {"GET /a", "POST /b", "GET /c"};
    const LogTable table = BuildStringTable(fixture, "path", values);

    int callCount = 0;
    const CallbackStringRowPredicate predicate(0, [&callCount](std::string_view bytes) {
        ++callCount;
        return bytes.starts_with("GET ");
    });
    CHECK(predicate.MatchesRow(table, 0));
    CHECK_FALSE(predicate.MatchesRow(table, 1));
    CHECK(predicate.MatchesRow(table, 2));
    CHECK(callCount == 3);
}

TEST_CASE("CallbackStringRowPredicate formats non-string slots via the column printFormat", "[log_filter][callback]")
{
    InitializeTimezoneData();
    const TestLogFile fixture("log_filter_callback_time.json");
    fixture.Write("");
    const LogTable table = BuildTimeTable(fixture, "ts", {1'700'000'000'000'000});

    std::string captured;
    const CallbackStringRowPredicate predicate(0, [&captured](std::string_view bytes) {
        captured.assign(bytes);
        return !bytes.empty();
    });
    INFO("formatted=" << captured);
    CHECK(predicate.MatchesRow(table, 0));
    CHECK_FALSE(captured.empty());
}

namespace
{

/// Single-column table seeded with one slot per row. Duplicated from
/// `test_log_compare.cpp` so the numeric / boolean tests can build
/// mixed-type tables without pulling in the bigger compare helpers.
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

} // namespace

TEST_CASE("NumericRangeRowPredicate accepts inclusive bounded ranges", "[log_filter][numeric_range]")
{
    const TestLogFile fixture("log_filter_numeric_bounded.json");
    fixture.Write("");
    const std::vector<LogValue> values = {
        int64_t{-5}, int64_t{0}, int64_t{5}, int64_t{10}, int64_t{15}, std::monostate{}
    };
    const LogTable table = BuildSingleColumnTable(fixture, "n", LogConfiguration::Type::Integer, values);

    const NumericRangeRowPredicate predicate(0, 0.0, 10.0);
    CHECK_FALSE(predicate.MatchesRow(table, 0)); // -5 < 0
    CHECK(predicate.MatchesRow(table, 1));       // 0
    CHECK(predicate.MatchesRow(table, 2));       // 5
    CHECK(predicate.MatchesRow(table, 3));       // 10
    CHECK_FALSE(predicate.MatchesRow(table, 4)); // 15 > 10
    CHECK_FALSE(predicate.MatchesRow(table, 5)); // monostate rejects
}

TEST_CASE("NumericRangeRowPredicate accepts a single-point range when min equals max", "[log_filter][numeric_range]")
{
    // `min == max` is a valid single-point filter. Inverted ranges
    // (`min > max`) are caught by the GUI; the predicate doesn't
    // enforce ordering itself.
    const TestLogFile fixture("log_filter_numeric_single_point.json");
    fixture.Write("");
    const std::vector<LogValue> values = {int64_t{4}, int64_t{5}, int64_t{6}, 5.0};
    const LogTable table = BuildSingleColumnTable(fixture, "n", LogConfiguration::Type::Number, values);

    const NumericRangeRowPredicate predicate(0, 5.0, 5.0);
    CHECK_FALSE(predicate.MatchesRow(table, 0)); // 4 != 5
    CHECK(predicate.MatchesRow(table, 1));       // int 5
    CHECK_FALSE(predicate.MatchesRow(table, 2)); // 6 != 5
    CHECK(predicate.MatchesRow(table, 3));       // double 5.0
}

TEST_CASE("NumericRangeRowPredicate handles unbounded sides", "[log_filter][numeric_range]")
{
    const TestLogFile fixture("log_filter_numeric_unbounded.json");
    fixture.Write("");
    const std::vector<LogValue> values = {int64_t{-100}, int64_t{0}, int64_t{100}, int64_t{1000}};
    const LogTable table = BuildSingleColumnTable(fixture, "n", LogConfiguration::Type::Integer, values);

    SECTION("unbounded min keeps everything <= max")
    {
        const NumericRangeRowPredicate predicate(0, std::nullopt, 100.0);
        CHECK(predicate.MatchesRow(table, 0));
        CHECK(predicate.MatchesRow(table, 1));
        CHECK(predicate.MatchesRow(table, 2));
        CHECK_FALSE(predicate.MatchesRow(table, 3));
    }

    SECTION("unbounded max keeps everything >= min")
    {
        const NumericRangeRowPredicate predicate(0, 0.0, std::nullopt);
        CHECK_FALSE(predicate.MatchesRow(table, 0));
        CHECK(predicate.MatchesRow(table, 1));
        CHECK(predicate.MatchesRow(table, 2));
        CHECK(predicate.MatchesRow(table, 3));
    }

    SECTION("both sides unbounded keeps every numeric slot")
    {
        const NumericRangeRowPredicate predicate(0, std::nullopt, std::nullopt);
        CHECK(predicate.MatchesRow(table, 0));
        CHECK(predicate.MatchesRow(table, 1));
        CHECK(predicate.MatchesRow(table, 2));
        CHECK(predicate.MatchesRow(table, 3));
    }
}

TEST_CASE("NumericRangeRowPredicate matches int / uint / double slots", "[log_filter][numeric_range]")
{
    const TestLogFile fixture("log_filter_numeric_mixed.json");
    fixture.Write("");
    const std::vector<LogValue> values = {
        int64_t{1}, uint64_t{2}, 3.5, std::numeric_limits<double>::quiet_NaN(), std::string("not-a-number")
    };
    const LogTable table = BuildSingleColumnTable(fixture, "n", LogConfiguration::Type::Number, values);

    const NumericRangeRowPredicate predicate(0, 1.0, 4.0);
    CHECK(predicate.MatchesRow(table, 0));       // int 1
    CHECK(predicate.MatchesRow(table, 1));       // uint 2
    CHECK(predicate.MatchesRow(table, 2));       // double 3.5
    CHECK_FALSE(predicate.MatchesRow(table, 3)); // NaN slot rejects
    CHECK_FALSE(predicate.MatchesRow(table, 4)); // wrong-type slot rejects
}

TEST_CASE("NumericRangeRowPredicate treats NaN bounds as unbounded", "[log_filter][numeric_range]")
{
    const TestLogFile fixture("log_filter_numeric_nan_bounds.json");
    fixture.Write("");
    const std::vector<LogValue> values = {int64_t{-50}, int64_t{0}, int64_t{50}};
    const LogTable table = BuildSingleColumnTable(fixture, "n", LogConfiguration::Type::Integer, values);

    const double nan = std::numeric_limits<double>::quiet_NaN();
    const NumericRangeRowPredicate predicate(0, nan, nan);
    // NaN bounds collapse to "unbounded", so every numeric row passes.
    CHECK(predicate.MatchesRow(table, 0));
    CHECK(predicate.MatchesRow(table, 1));
    CHECK(predicate.MatchesRow(table, 2));
}

TEST_CASE("BoolRowPredicate selects by side", "[log_filter][boolean]")
{
    const TestLogFile fixture("log_filter_boolean.json");
    fixture.Write("");
    const std::vector<LogValue> values = {true, false, true, false, std::monostate{}};
    const LogTable table = BuildSingleColumnTable(fixture, "flag", LogConfiguration::Type::Boolean, values);

    SECTION("only true")
    {
        const BoolRowPredicate predicate(0, /*includeTrue=*/true, /*includeFalse=*/false);
        CHECK(predicate.MatchesRow(table, 0));
        CHECK_FALSE(predicate.MatchesRow(table, 1));
        CHECK(predicate.MatchesRow(table, 2));
        CHECK_FALSE(predicate.MatchesRow(table, 3));
        CHECK_FALSE(predicate.MatchesRow(table, 4));
    }

    SECTION("only false")
    {
        const BoolRowPredicate predicate(0, /*includeTrue=*/false, /*includeFalse=*/true);
        CHECK_FALSE(predicate.MatchesRow(table, 0));
        CHECK(predicate.MatchesRow(table, 1));
        CHECK_FALSE(predicate.MatchesRow(table, 2));
        CHECK(predicate.MatchesRow(table, 3));
        CHECK_FALSE(predicate.MatchesRow(table, 4));
    }

    SECTION("both selected accepts every bool")
    {
        const BoolRowPredicate predicate(0, /*includeTrue=*/true, /*includeFalse=*/true);
        CHECK(predicate.MatchesRow(table, 0));
        CHECK(predicate.MatchesRow(table, 1));
        CHECK(predicate.MatchesRow(table, 2));
        CHECK(predicate.MatchesRow(table, 3));
        CHECK_FALSE(predicate.MatchesRow(table, 4)); // monostate still rejects
    }

    SECTION("neither selected rejects every row")
    {
        const BoolRowPredicate predicate(0, /*includeTrue=*/false, /*includeFalse=*/false);
        for (size_t row = 0; row < 5; ++row)
        {
            CHECK_FALSE(predicate.MatchesRow(table, row));
        }
    }
}

TEST_CASE(
    "EnumRowPredicate reused for Type::Level columns matches expanded dictionary entries",
    "[log_filter][enum_row_predicate][level]"
)
{
    // Mirrors `MainWindow::BuildRowPredicates` for Level columns:
    // canonical names ("Info", "Warn") on the UI side are translated
    // to the raw dictionary entries, then fed into `EnumRowPredicate`.
    const TestLogFile fixture("log_filter_level.json");
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
    const std::vector<std::string> rowValues = {"info", "WARN", "error", "qux", "Warning"};
    for (const auto &v : rowValues)
    {
        batch.lines.push_back(MakeLine(keys, *sourcePtr, {{"level", std::string(v)}}));
    }
    batch.newKeys.emplace_back("level");
    table.AppendBatch(std::move(batch));

    REQUIRE(table.Configuration().Configuration().columns[0].type == LogConfiguration::Type::Level);
    const auto *ranks = table.LevelRankCache(0);
    REQUIRE(ranks != nullptr);
    const auto &registry = table.EnumDictionaries();
    const KeyId kid = table.Keys().Find("level");
    REQUIRE(kid != INVALID_KEY_ID);
    const EnumDictionary *dict = registry.Find(kid);
    REQUIRE(dict != nullptr);

    // Expand canonical names to raw entries via the cache; same logic
    // `MainWindow::BuildRowPredicates` uses.
    auto expandSelection = [&](std::initializer_list<LogLevel> selectedLevels) {
        std::vector<std::string> expanded;
        for (size_t valueId = 0; valueId < ranks->size(); ++valueId)
        {
            for (const LogLevel level : selectedLevels)
            {
                if ((*ranks)[valueId] == level)
                {
                    expanded.emplace_back(dict->Resolve(static_cast<EnumValueId>(valueId)));
                    break;
                }
            }
        }
        return expanded;
    };

    {
        const auto expanded = expandSelection({LogLevel::Warn});
        std::vector<std::string_view> selectedViews;
        selectedViews.reserve(expanded.size());
        for (const auto &s : expanded)
        {
            selectedViews.emplace_back(s);
        }
        const EnumRowPredicate predicate(0, std::span<const std::string_view>(selectedViews), dict);
        CHECK_FALSE(predicate.MatchesRow(table, 0)); // info
        CHECK(predicate.MatchesRow(table, 1));       // WARN
        CHECK_FALSE(predicate.MatchesRow(table, 2)); // error
        CHECK_FALSE(predicate.MatchesRow(table, 3)); // qux
        CHECK(predicate.MatchesRow(table, 4));       // Warning -> Warn alias
    }

    {
        const auto expanded = expandSelection({LogLevel::Info, LogLevel::Error});
        std::vector<std::string_view> selectedViews;
        selectedViews.reserve(expanded.size());
        for (const auto &s : expanded)
        {
            selectedViews.emplace_back(s);
        }
        const EnumRowPredicate predicate(0, std::span<const std::string_view>(selectedViews), dict);
        CHECK(predicate.MatchesRow(table, 0));       // info
        CHECK_FALSE(predicate.MatchesRow(table, 1)); // WARN
        CHECK(predicate.MatchesRow(table, 2));       // error
        CHECK_FALSE(predicate.MatchesRow(table, 3)); // qux
        CHECK_FALSE(predicate.MatchesRow(table, 4)); // Warning
    }
}
