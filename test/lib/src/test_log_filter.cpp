#include "common.hpp"

#include <loglib/enum_dictionary.hpp>
#include <loglib/file_line_source.hpp>
#include <loglib/key_index.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_filter.hpp>
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
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace loglib;

namespace
{

/// Build a `LogLine` bound to @p keys, inserting each `(key, value)`
/// pair via `GetOrInsert` so the caller can skip explicit KeyIds.
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

/// Build a single-column `Type::Enumeration` `LogTable` seeded with the
/// rows in @p values. Each row gets one field on `columnKey` with the
/// next value (cycling).
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

/// Build a single-column string `LogTable` for fallback tests. Column
/// type stays `Any` so values land as strings without promotion.
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
        batch.lines.push_back(MakeLine(keys, *sourcePtr, {{columnKey, std::string(v)}}));
    }
    if (!perRowValues.empty())
    {
        batch.newKeys.emplace_back(columnKey);
    }
    table.AppendBatch(std::move(batch));
    return table;
}

/// Build a `Type::Time` `LogTable`. Times are passed in as
/// `loglib::TimeStamp` (microseconds since epoch).
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
    const TestLogFile fixture("log_filter_enum_accept.json");
    fixture.Write("");
    const std::vector<std::string> levels = {"info", "warn", "error"};
    LogTable table = BuildEnumTable(fixture, "level", levels, 12);
    REQUIRE(table.Configuration().Configuration().columns[0].type == LogConfiguration::Type::Enumeration);
    const EnumDictionary *dict = FindDictionary(table, "level");
    REQUIRE(dict != nullptr);

    const std::vector<std::string> selected = {"warn", "error"};
    const std::vector<std::string_view> selectedViews = ToViews(selected);
    const EnumRowPredicate predicate(0, selectedViews, dict);
    REQUIRE(predicate.IsFastPathArmed());

    // Rows cycle [info, warn, error, info, warn, error, ...].
    for (size_t row = 0; row < table.RowCount(); ++row)
    {
        const std::string_view expected = levels[row % levels.size()];
        const bool selectedRow = expected != "info";
        INFO("row=" << row << " value=" << expected);
        CHECK(predicate.MatchesRow(table, row) == selectedRow);
    }
}

TEST_CASE("EnumRowPredicate rejects every row on an empty selection", "[log_filter][enum]")
{
    const TestLogFile fixture("log_filter_enum_empty.json");
    fixture.Write("");
    LogTable table = BuildEnumTable(fixture, "level", {"info", "warn"}, 6);
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
    // The GUI gate rebuilds the predicate when a selected value gains a
    // new id, so any id past `mSelectedIds.size()` we see at match time
    // is by invariant for an *unselected* value. Confirm the predicate
    // rejects rather than falling through to the string set.
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
    // Regression: when a *selected* value is
    // unresolved at construction time and is later interned past the
    // bitset, a stale predicate evaluated against the new id must NOT
    // reject -- it must fall through to the string-set fallback so the
    // newly-interned row still matches. The GUI's `enumColumnsChanged`
    // gate keeps this from happening in practice, but the predicate
    // owes correctness regardless of that gate.
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

    // Pre-fix: returned `false` because the past-bitset branch
    // unconditionally rejected. Post-fix: falls through to the string
    // set, which contains "debug" -> `true`.
    CHECK(predicate.MatchesRow(table, debugRow));

    // The pre-existing "info" rows continue to match through the bitset.
    for (size_t row = 0; row < 2; ++row)
    {
        CHECK(predicate.MatchesRow(table, row));
    }
}

// Lifetime trip-wire: the `EnumRowPredicate` constructor takes a
// `std::span<const std::string_view>`. The `string_view`s themselves
// point at caller-owned bytes. The predicate must NOT retain the
// `string_view`s past construction -- it either records the bytes
// (copied into `mSelectedStrings`) or marks an `EnumValueId` in the
// bitset. `MainWindow::UpdateFilters` relies on this: the span is
// built from short-lived `std::string`s on the stack frame of
// `UpdateFilters`, and the predicate lives much longer (until the
// next `SetFilterRules` call). If a future refactor accidentally
// caches the view instead of copying the bytes, this test will catch
// it under ASan / use-after-free.
TEST_CASE("EnumRowPredicate does not retain references into the selection span", "[log_filter][enum][lifetime]")
{
    const TestLogFile fixture("log_filter_enum_lifetime.json");
    fixture.Write("");
    // Two-value dictionary so one selected value is resolved at
    // construction (id path) and one falls through to the string-set
    // fallback. That covers both storage strategies.
    LogTable table = BuildEnumTable(fixture, "level", {"info", "warn"}, 4);
    const EnumDictionary *dict = FindDictionary(table, "level");
    REQUIRE(dict != nullptr);

    // Build the predicate from views over strings that we deliberately
    // overwrite right after construction. If the predicate kept the
    // `string_view`s, every subsequent `MatchesRow` call would walk
    // garbage bytes and trip ASan (or sometimes silently match the
    // wrong rows).
    auto buildPredicate = [&]() {
        std::vector<std::string> scratch = {"info", "completely_unrelated_label_that_does_not_exist"};
        const std::vector<std::string_view> views = ToViews(scratch);
        EnumRowPredicate built(0, views, dict);
        // Overwrite the backing bytes in place so any retained
        // `string_view` would now read different content. The scratch
        // vector + views are destructed at the end of the lambda's
        // scope; we return the predicate by value.
        for (std::string &s : scratch)
        {
            std::fill(s.begin(), s.end(), 'X');
        }
        return built;
    };
    const EnumRowPredicate predicate = buildPredicate();
    REQUIRE(predicate.IsFastPathArmed());

    // The fixture cycles [info, warn, info, warn]. With "info"
    // selected only the even rows should match. If the predicate
    // had retained the now-mangled view, the bitset would still
    // hold the resolved id (id-path is byte-stable), but the
    // unresolved value's string-set entry would either be "XXXX..."
    // or freed memory.
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
    // Regression: pre-fix `mAllResolved` was computed against
    // `selectedValues.size()`, so a caller passing duplicates would
    // bump the resolved counter once per duplicate. With ["info",
    // "info"] both resolving, `resolvedCount == 2 == size`, fine. But
    // with ["info", "info", "debug"] where "debug" is unresolved,
    // `resolvedCount == 2`, `size == 3`, `mAllResolved == false` --
    // also fine. The bug surfaces when a future call site dedupes
    // upstream and the predicate's counter starts disagreeing with
    // the externally-visible "distinct selected" semantics. Post-fix
    // the predicate dedupes its input first, so the accounting is
    // identical for both shapes.
    const TestLogFile fixture("log_filter_enum_dedupe.json");
    fixture.Write("");
    LogTable table = BuildEnumTable(fixture, "level", {"info", "warn"}, 4);
    const EnumDictionary *dict = FindDictionary(table, "level");
    REQUIRE(dict != nullptr);
    REQUIRE(dict->Size() == 2);

    SECTION("duplicates of an unresolved value still trigger the string-set fallback")
    {
        // ["info", "info", "debug"]: only "info" resolves. The
        // predicate must remain *not* fully resolved so a future
        // "debug" id (past the bitset) falls through to the string
        // set instead of being rejected by the `mAllResolved == true`
        // short-circuit.
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
        // ["info", "info"]: only "info" selected, twice. The
        // predicate must short-circuit out-of-range ids via
        // `mAllResolved == true` instead of falling through to the
        // string-set fallback (`mSelectedStrings` is empty in this
        // case anyway, so the visible behaviour is identical; we
        // exercise the path through a stale-bitset id to confirm
        // the short-circuit.)
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
    LogTable table = BuildTimeTable(fixture, "ts", times);

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
    LogTable table = BuildStringTable(fixture, "label", {"alpha", "beta"});

    const TimeRangeRowPredicate predicate(0, /*begin=*/0, /*end=*/1'000'000);
    CHECK_FALSE(predicate.MatchesRow(table, 0));
    CHECK_FALSE(predicate.MatchesRow(table, 1));
}

TEST_CASE("CallbackStringRowPredicate forwards string slots to the callback", "[log_filter][callback]")
{
    const TestLogFile fixture("log_filter_callback_string.json");
    fixture.Write("");
    const std::vector<std::string> values = {"GET /a", "POST /b", "GET /c"};
    LogTable table = BuildStringTable(fixture, "path", values);

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
    LogTable table = BuildTimeTable(fixture, "ts", {1'700'000'000'000'000});

    std::string captured;
    const CallbackStringRowPredicate predicate(0, [&captured](std::string_view bytes) {
        captured.assign(bytes);
        return !bytes.empty();
    });
    INFO("formatted=" << captured);
    CHECK(predicate.MatchesRow(table, 0));
    CHECK_FALSE(captured.empty());
}
