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
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
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

TEST_CASE(
    "CompareRows on an Integer column clamps out-of-range doubles and tails NaN as non-numeric",
    "[log_compare][integer][regression]"
)
{
    // Regression: `CompareInteger`'s `toInt` lambda
    // used `static_cast<int64_t>(double)`, which is UB on NaN / values
    // outside the signed range. Post-fix:
    //  - NaN -> `std::nullopt` (sorts after every integer slot).
    //  - +inf / large positive -> clamped to `INT64_MAX`.
    //  - -inf / large negative -> clamped to `INT64_MIN`.
    const TestLogFile fixture("log_compare_integer_double_slot.json");
    fixture.Write("");
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double posInf = std::numeric_limits<double>::infinity();
    const double negInf = -posInf;
    const double huge = 1e30;
    const double nhuge = -1e30;
    const std::vector<LogValue> values = {
        int64_t{0}, // 0: a plain int (sorts between negative and positive doubles)
        nan,        // 1: NaN -> sorts after every integer-shaped slot
        posInf,     // 2: clamps to INT64_MAX
        negInf,     // 3: clamps to INT64_MIN
        huge,       // 4: clamps to INT64_MAX (== row 2 numerically)
        nhuge,      // 5: clamps to INT64_MIN (== row 3 numerically)
        int64_t{42} // 6: a plain int sandwich
    };
    LogTable table = BuildSingleColumnTable(fixture, "n", LogConfiguration::Type::Integer, values);

    // Ordering invariants. We don't pin a total order across NaN/int
    // (the "numeric < non-numeric" rule already promises NaN > any int);
    // we just pin the three things M2 fixes: no UB, clamping is
    // order-preserving, NaN tails after every integer.

    // Plain ints compare normally.
    CHECK(SignOf(CompareRows(table, 0, 6, 0)) == -1); // 0 < 42

    // NaN routes to the non-numeric tail: tails after every int slot.
    CHECK(SignOf(CompareRows(table, 1, 0, 0)) == 1); // NaN > 0
    CHECK(SignOf(CompareRows(table, 1, 6, 0)) == 1); // NaN > 42
    CHECK(SignOf(CompareRows(table, 1, 1, 0)) == 0); // NaN == NaN

    // +inf / huge both clamp to INT64_MAX -> tied numerically, and both
    // are >= the plain ints 0 / 42.
    CHECK(SignOf(CompareRows(table, 2, 0, 0)) == 1); // +inf > 0
    CHECK(SignOf(CompareRows(table, 4, 0, 0)) == 1); // 1e30 > 0
    CHECK(SignOf(CompareRows(table, 2, 4, 0)) == 0); // +inf == 1e30 (both clamped)
    CHECK(SignOf(CompareRows(table, 2, 6, 0)) == 1); // +inf > 42

    // -inf / -huge both clamp to INT64_MIN -> tied, and both < 0.
    CHECK(SignOf(CompareRows(table, 3, 0, 0)) == -1); // -inf < 0
    CHECK(SignOf(CompareRows(table, 5, 0, 0)) == -1); // -1e30 < 0
    CHECK(SignOf(CompareRows(table, 3, 5, 0)) == 0);  // -inf == -1e30
    CHECK(SignOf(CompareRows(table, 5, 6, 0)) == -1); // -1e30 < 42

    // NaN tails after both clamped-extreme rows too.
    CHECK(SignOf(CompareRows(table, 1, 2, 0)) == 1); // NaN > +inf clamp
    CHECK(SignOf(CompareRows(table, 1, 3, 0)) == 1); // NaN > -inf clamp

    // Antisymmetry on every pair we asserted (sanity for `CompareRows is total`).
    constexpr std::array<std::pair<size_t, size_t>, 7> PAIRS = {
        {{0, 6}, {1, 0}, {1, 6}, {2, 0}, {2, 4}, {3, 5}, {1, 3}}
    };
    for (const auto &[a, b] : PAIRS)
    {
        const int forward = SignOf(CompareRows(table, a, b, 0));
        const int reverse = SignOf(CompareRows(table, b, a, 0));
        INFO("a=" << a << " b=" << b);
        CHECK(forward == -reverse);
    }
}

TEST_CASE(
    "CompareRows: NaN-in-Int sorts equal to monostate at the tail", "[log_compare][integer][regression]"
)
{
    // Regression: pre-fix `CompareTyped` ran
    // `CompareMonostateOrder` before `Extract`, so NaN-in-Int compared
    // strictly less than monostate (tail order was
    // `representable < NaN < monostate`). Post-fix the unrepresentable
    // tail bucket compares equal to monostate (header doc invariant).
    const TestLogFile fixture("log_compare_int_nan_vs_monostate.json");
    fixture.Write("");
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const std::vector<LogValue> values = {int64_t{0}, nan, std::monostate{}, int64_t{42}};
    LogTable table = BuildSingleColumnTable(fixture, "n", LogConfiguration::Type::Integer, values);

    // Tail-bucket equality: NaN <=> monostate is 0.
    CHECK(SignOf(CompareRows(table, 1, 2, 0)) == 0);  // NaN == monostate
    CHECK(SignOf(CompareRows(table, 2, 1, 0)) == 0);  // antisymmetric: still equal swapped
    CHECK(SignOf(CompareRows(table, 1, 1, 0)) == 0);  // NaN == NaN
    CHECK(SignOf(CompareRows(table, 2, 2, 0)) == 0);  // monostate == monostate

    // Representable < tail-bucket.
    CHECK(SignOf(CompareRows(table, 0, 1, 0)) == -1); // 0 < NaN
    CHECK(SignOf(CompareRows(table, 0, 2, 0)) == -1); // 0 < monostate
    CHECK(SignOf(CompareRows(table, 3, 1, 0)) == -1); // 42 < NaN
    CHECK(SignOf(CompareRows(table, 3, 2, 0)) == -1); // 42 < monostate
}

TEST_CASE(
    "CompareRows: stray-string-in-Floating sorts equal to monostate at the tail",
    "[log_compare][floating][regression]"
)
{
    // Parallel of the Integer regression above: a string slot in a
    // `Floating` column is unrepresentable, so it must tie with
    // monostate rather than sort strictly below it.
    const TestLogFile fixture("log_compare_float_string_vs_monostate.json");
    fixture.Write("");
    const std::vector<LogValue> values = {
        1.5,                 // 0: representable double
        std::string("oops"), // 1: stray string -> tail bucket
        std::monostate{},    // 2: monostate -> tail bucket
        -3.25,               // 3: representable double
    };
    LogTable table = BuildSingleColumnTable(fixture, "x", LogConfiguration::Type::Floating, values);

    // Tail-bucket equality.
    CHECK(SignOf(CompareRows(table, 1, 2, 0)) == 0);  // stray-string == monostate
    CHECK(SignOf(CompareRows(table, 2, 1, 0)) == 0);  // antisymmetric

    // Representable < tail-bucket.
    CHECK(SignOf(CompareRows(table, 0, 1, 0)) == -1); // 1.5 < stray-string
    CHECK(SignOf(CompareRows(table, 0, 2, 0)) == -1); // 1.5 < monostate
    CHECK(SignOf(CompareRows(table, 3, 1, 0)) == -1); // -3.25 < stray-string
    CHECK(SignOf(CompareRows(table, 3, 2, 0)) == -1); // -3.25 < monostate
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

// Regression: when a `Type::Enumeration` column contains a row whose
// slot is NOT a `DictRef` (e.g. `std::monostate` for a missing field,
// or a slot value that hasn't been promoted yet), `CompareEnum` must
// fall through to the monostate / bytewise compare path -- the
// id-pair / rank-table path requires both ids resolvable.
// `LogFilterModel::lessThan` calls `CompareRows` for every visible
// pair when the user sorts by an unpromoted enum column. Without
// this branch, the third clause in `CompareEnum` (the
// "one-or-both-sides-not-DictRef" path) would never get exercised.
TEST_CASE("CompareRows on enum column with a monostate row uses tail-bucket order", "[log_compare][enum][monostate]")
{
    const TestLogFile fixture("log_compare_enum_monostate.json");
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
    // Three rows: "info", monostate (missing field), "warn".
    // Row 0 and row 2 promote to dict refs; row 1 is monostate so
    // `GetEnumValueId(row=1)` is nullopt and we drop into the
    // monostate-handling branch.
    batch.lines.push_back(MakeLine(keys, *sourcePtr, {{"level", std::string("info")}}));
    batch.lines.push_back(MakeLine(keys, *sourcePtr, {})); // missing field -> monostate
    batch.lines.push_back(MakeLine(keys, *sourcePtr, {{"level", std::string("warn")}}));
    batch.newKeys.emplace_back("level");
    table.AppendBatch(std::move(batch));

    // monostate ends up at the tail of ascending order, so it
    // compares as "greater" than any concrete value.
    CHECK(SignOf(CompareRows(table, 0, 1, 0, /*rankForEnumColumn=*/nullptr)) == -1); // info < monostate
    CHECK(SignOf(CompareRows(table, 2, 1, 0, /*rankForEnumColumn=*/nullptr)) == -1); // warn < monostate
    CHECK(SignOf(CompareRows(table, 1, 0, 0, /*rankForEnumColumn=*/nullptr)) == 1);  // monostate > info
    CHECK(SignOf(CompareRows(table, 1, 1, 0, /*rankForEnumColumn=*/nullptr)) == 0);  // self -> equal
    // The concrete pair still routes through CompareLogValuesBytewise
    // (rank == nullptr), so info < warn.
    CHECK(SignOf(CompareRows(table, 0, 2, 0, /*rankForEnumColumn=*/nullptr)) == -1);
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
