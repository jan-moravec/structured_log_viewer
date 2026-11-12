#include "common.hpp"
#include "table_fixture.hpp"

#include <loglib/core/enum_dictionary.hpp>
#include <loglib/core/file_line_source.hpp>
#include <loglib/core/key_index.hpp>
#include <loglib/core/log_line.hpp>
#include <loglib/core/log_value.hpp>
#include <loglib/parse/log_parse_sink.hpp>
#include <loglib/query/log_compare.hpp>
#include <loglib/schema/log_configuration.hpp>
#include <loglib/table/log_table.hpp>

#include <catch2/catch_all.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <random>
#include <span>
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
    return {std::move(sorted), source, 0};
}

/// Single-column `LogTable` of @p type seeded with one slot per row.
LogTable BuildSingleColumnTable(
    const MemoryLogFile &testFile,
    const std::string &columnKey,
    ColumnType type,
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

    loglib::LogTable table(mgr.Configuration());
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

    const EnumDictRank rank{dict};
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
    const MemoryLogFile fixture("log_compare_integer.json");
    fixture.Write("");
    const std::vector<LogValue> values = {int64_t{3}, int64_t{1}, std::monostate{}, int64_t{2}};
    const LogTable table = BuildSingleColumnTable(fixture, "n", ColumnType::Integer, values);

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
    // Regression for `CompareInteger`:
    //  - NaN -> tail (was UB via `static_cast<int64_t>`).
    //  - +inf / large positive -> clamped to `INT64_MAX`.
    //  - -inf / large negative -> clamped to `INT64_MIN`.
    const MemoryLogFile fixture("log_compare_integer_double_slot.json");
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
    const LogTable table = BuildSingleColumnTable(fixture, "n", ColumnType::Integer, values);

    // We pin: no UB, clamping is order-preserving, NaN tails after
    // every integer.

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

TEST_CASE("CompareRows: NaN-in-Int sorts equal to monostate at the tail", "[log_compare][integer][regression]")
{
    // Tail-bucket invariant: NaN-in-Int and monostate tie. Pre-fix
    // `CompareMonostateOrder` ran before `Extract` so the order was
    // `representable < NaN < monostate`.
    const MemoryLogFile fixture("log_compare_int_nan_vs_monostate.json");
    fixture.Write("");
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const std::vector<LogValue> values = {int64_t{0}, nan, std::monostate{}, int64_t{42}};
    const LogTable table = BuildSingleColumnTable(fixture, "n", ColumnType::Integer, values);

    // Tail-bucket equality: NaN <=> monostate is 0.
    CHECK(SignOf(CompareRows(table, 1, 2, 0)) == 0); // NaN == monostate
    CHECK(SignOf(CompareRows(table, 2, 1, 0)) == 0); // antisymmetric: still equal swapped
    CHECK(SignOf(CompareRows(table, 1, 1, 0)) == 0); // NaN == NaN
    CHECK(SignOf(CompareRows(table, 2, 2, 0)) == 0); // monostate == monostate

    // Representable < tail-bucket.
    CHECK(SignOf(CompareRows(table, 0, 1, 0)) == -1); // 0 < NaN
    CHECK(SignOf(CompareRows(table, 0, 2, 0)) == -1); // 0 < monostate
    CHECK(SignOf(CompareRows(table, 3, 1, 0)) == -1); // 42 < NaN
    CHECK(SignOf(CompareRows(table, 3, 2, 0)) == -1); // 42 < monostate
}

TEST_CASE(
    "CompareRows: stray-string-in-Floating sorts equal to monostate at the tail", "[log_compare][floating][regression]"
)
{
    // Tail-bucket invariant for `Floating`: a stray string slot ties
    // with monostate rather than sorting below it.
    const MemoryLogFile fixture("log_compare_float_string_vs_monostate.json");
    fixture.Write("");
    const std::vector<LogValue> values = {
        1.5,                 // 0: representable double
        std::string("oops"), // 1: stray string -> tail bucket
        std::monostate{},    // 2: monostate -> tail bucket
        -3.25,               // 3: representable double
    };
    const LogTable table = BuildSingleColumnTable(fixture, "x", ColumnType::Floating, values);

    // Tail-bucket equality.
    CHECK(SignOf(CompareRows(table, 1, 2, 0)) == 0); // stray-string == monostate
    CHECK(SignOf(CompareRows(table, 2, 1, 0)) == 0); // antisymmetric

    // Representable < tail-bucket.
    CHECK(SignOf(CompareRows(table, 0, 1, 0)) == -1); // 1.5 < stray-string
    CHECK(SignOf(CompareRows(table, 0, 2, 0)) == -1); // 1.5 < monostate
    CHECK(SignOf(CompareRows(table, 3, 1, 0)) == -1); // -3.25 < stray-string
    CHECK(SignOf(CompareRows(table, 3, 2, 0)) == -1); // -3.25 < monostate
}

TEST_CASE("CompareRows handles Floating columns with NaN at the tail", "[log_compare][floating]")
{
    const MemoryLogFile fixture("log_compare_floating.json");
    fixture.Write("");
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const std::vector<LogValue> values = {1.5, -2.0, nan, 4.25};
    const LogTable table = BuildSingleColumnTable(fixture, "x", ColumnType::Floating, values);

    CHECK(SignOf(CompareRows(table, 1, 0, 0)) == -1); // -2 < 1.5
    CHECK(SignOf(CompareRows(table, 0, 3, 0)) == -1); // 1.5 < 4.25
    CHECK(SignOf(CompareRows(table, 2, 0, 0)) == 1);  // NaN > 1.5
    CHECK(SignOf(CompareRows(table, 2, 2, 0)) == 0);  // NaN == NaN
}

TEST_CASE("CompareRows handles Time columns by microseconds-since-epoch", "[log_compare][time]")
{
    const MemoryLogFile fixture("log_compare_time.json");
    fixture.Write("");
    const TimeStamp t100{std::chrono::microseconds{100}};
    const TimeStamp t300{std::chrono::microseconds{300}};
    const TimeStamp t200{std::chrono::microseconds{200}};
    const std::vector<LogValue> values = {t100, t300, t200, std::monostate{}};
    const LogTable table = BuildSingleColumnTable(fixture, "ts", ColumnType::Time, values, "{:%FT%T}");

    CHECK(SignOf(CompareRows(table, 0, 1, 0)) == -1); // 100 < 300
    CHECK(SignOf(CompareRows(table, 1, 2, 0)) == 1);  // 300 > 200
    CHECK(SignOf(CompareRows(table, 0, 2, 0)) == -1); // 100 < 200
    CHECK(SignOf(CompareRows(table, 3, 0, 0)) == 1);  // monostate > populated
}

// Regression: `CompareTime` accepted only `TimeStamp`/`int64_t`, so a
// `uint64_t` micros slot routed to the tail. `TimeRangeRowPredicate`
// already accepted `uint64_t`; this restores parity.
TEST_CASE("CompareRows on Time column compares uint64_t slots numerically", "[log_compare][time][regression]")
{
    const MemoryLogFile fixture("log_compare_time_uint.json");
    fixture.Write("");
    const TimeStamp t100{std::chrono::microseconds{100}};
    const TimeStamp t300{std::chrono::microseconds{300}};
    const std::vector<LogValue> values = {t100, uint64_t{200}, t300};
    const LogTable table = BuildSingleColumnTable(fixture, "ts", ColumnType::Time, values, "{:%FT%T}");

    CHECK(SignOf(CompareRows(table, 0, 1, 0)) == -1); // 100 < 200(u)
    CHECK(SignOf(CompareRows(table, 1, 2, 0)) == -1); // 200(u) < 300
    CHECK(SignOf(CompareRows(table, 1, 0, 0)) == 1);  // 200(u) > 100
    CHECK(SignOf(CompareRows(table, 1, 1, 0)) == 0);  // same row

    // Oversized `uint64_t` (past `int64_t::max`) clamps so it
    // sorts strictly above every representable `TimeStamp`.
    const MemoryLogFile fixtureBig("log_compare_time_uint_big.json");
    fixtureBig.Write("");
    const uint64_t huge = static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 100;
    const std::vector<LogValue> bigValues = {t100, huge};
    const LogTable bigTable = BuildSingleColumnTable(fixtureBig, "ts", ColumnType::Time, bigValues, "{:%FT%T}");
    CHECK(SignOf(CompareRows(bigTable, 0, 1, 0)) == -1); // t100 < clamped uint
    CHECK(SignOf(CompareRows(bigTable, 1, 0, 0)) == 1);
}

TEST_CASE("CompareRows on an Enumeration column uses the rank table", "[log_compare][enum]")
{
    // Non-level key (`category`) so the column stays Enumeration even
    // though the dict happens to hold canonical level names. Pins the
    // rank-table compare path against alphabetic order.
    const MemoryLogFile fixture("log_compare_enum.json");
    fixture.Write("");

    auto source = fixture.CreateFileLineSource();
    FileLineSource *sourcePtr = source.get();
    LogConfiguration cfg;
    cfg.columns.push_back(
        {.header = "category",
         .keys = {"category"},
         .printFormat = "{}",
         .type = ColumnType::Enumeration,
         .parseFormats = {}}
    );
    const TestLogConfiguration cfgFile;
    cfgFile.Write(cfg);
    LogConfigurationManager mgr;
    mgr.Load(cfgFile.GetFilePath());
    loglib::LogTable table(mgr.Configuration());
    table.BeginStreaming(std::move(source));

    KeyIndex &keys = table.Keys();
    RowBatch batch;
    batch.firstLineNumber = 1;
    // Insertion order is the dict order: warn=0, info=1, error=2, debug=3.
    // Alphabetic rank should re-order to debug<error<info<warn.
    const std::vector<std::string> rowValues = {"warn", "info", "error", "debug", "warn"};
    for (const auto &v : rowValues)
    {
        batch.lines.push_back(MakeLine(keys, *sourcePtr, {{"category", std::string(v)}}));
    }
    batch.newKeys.emplace_back("category");
    AppendFixtureBatch(table, std::move(batch));

    REQUIRE(table.Configuration().Configuration().columns[0].type == ColumnType::Enumeration);
    const KeyId categoryKey = keys.Find("category");
    REQUIRE(categoryKey != INVALID_KEY_ID);
    const EnumDictionary *dict = table.EnumDictionaries().Find(categoryKey);
    REQUIRE(dict != nullptr);

    const EnumDictRank rank{*dict};

    // Row 0 = warn, row 1 = info; alphabetic: info < warn so (0 vs 1) > 0.
    CHECK(SignOf(CompareRows(table, 0, 1, 0, &rank)) == 1);
    // Row 3 = debug, row 2 = error; alphabetic: debug < error.
    CHECK(SignOf(CompareRows(table, 3, 2, 0, &rank)) == -1);
    // Same value tie -> 0.
    CHECK(SignOf(CompareRows(table, 0, 4, 0, &rank)) == 0);
}

// `CompareEnum` must handle non-`DictRef` slots (monostate / unpromoted)
// via the bytewise/monostate fallback, since the rank-table path needs
// both ids resolved. Triggered by sort over an unpromoted enum column.
TEST_CASE("CompareRows on enum column with a monostate row uses tail-bucket order", "[log_compare][enum][monostate]")
{
    const MemoryLogFile fixture("log_compare_enum_monostate.json");
    fixture.Write("");
    auto source = fixture.CreateFileLineSource();
    FileLineSource *sourcePtr = source.get();
    LogConfiguration cfg;
    cfg.columns.push_back(
        {.header = "level", .keys = {"level"}, .printFormat = "{}", .type = ColumnType::Enumeration, .parseFormats = {}}
    );
    const TestLogConfiguration cfgFile;
    cfgFile.Write(cfg);
    LogConfigurationManager mgr;
    mgr.Load(cfgFile.GetFilePath());
    loglib::LogTable table(mgr.Configuration());
    table.BeginStreaming(std::move(source));

    KeyIndex &keys = table.Keys();
    RowBatch batch;
    batch.firstLineNumber = 1;
    // Row 1 has no field -> monostate -> takes the fallback branch.
    batch.lines.push_back(MakeLine(keys, *sourcePtr, {{"level", std::string("info")}}));
    batch.lines.push_back(MakeLine(keys, *sourcePtr, {}));
    batch.lines.push_back(MakeLine(keys, *sourcePtr, {{"level", std::string("warn")}}));
    batch.newKeys.emplace_back("level");
    AppendFixtureBatch(table, std::move(batch));

    // monostate sits at the tail of ascending order.
    CHECK(SignOf(CompareRows(table, 0, 1, 0, /*rankForEnumColumn=*/nullptr)) == -1); // info < monostate
    CHECK(SignOf(CompareRows(table, 2, 1, 0, /*rankForEnumColumn=*/nullptr)) == -1); // warn < monostate
    CHECK(SignOf(CompareRows(table, 1, 0, 0, /*rankForEnumColumn=*/nullptr)) == 1);  // monostate > info
    CHECK(SignOf(CompareRows(table, 1, 1, 0, /*rankForEnumColumn=*/nullptr)) == 0);  // self -> equal
    // No rank table -> bytewise fallback: info < warn.
    CHECK(SignOf(CompareRows(table, 0, 2, 0, /*rankForEnumColumn=*/nullptr)) == -1);
}

// Regression: with a rank table in hand, every non-`DictRef` slot
// (monostate, unpromoted-string, wrong-type, over-cap-length) must
// collapse to the tail bucket and compare equal pairwise, matching
// `SortPermutationByColumn`'s rank-sentinel behaviour. Pre-fix the
// slow-path comparator went through `CompareLogValuesBytewise` for
// two unpromoted sides, so a streaming `lower_bound` insert
// disagreed with a bulk re-sort on where wrong-type slots landed.
TEST_CASE("CompareEnum: non-DictRef slots all sort tail-equal under a rank table", "[log_compare][enum][regression]")
{
    // Non-level key (`category`) so the column stays Enumeration and
    // exercises `CompareEnum` rather than `CompareLevel`.
    const MemoryLogFile fixture("log_compare_enum_tail_bucket.json");
    fixture.Write("");
    auto source = fixture.CreateFileLineSource();
    FileLineSource *sourcePtr = source.get();
    LogConfiguration cfg;
    cfg.columns.push_back(
        {.header = "category",
         .keys = {"category"},
         .printFormat = "{}",
         .type = ColumnType::Enumeration,
         .parseFormats = {}}
    );
    const TestLogConfiguration cfgFile;
    cfgFile.Write(cfg);
    LogConfigurationManager mgr;
    mgr.Load(cfgFile.GetFilePath());
    loglib::LogTable table(mgr.Configuration());
    table.BeginStreaming(std::move(source));

    KeyIndex &keys = table.Keys();
    RowBatch batch;
    batch.firstLineNumber = 1;
    // Row 0: dict-resolved "info".
    batch.lines.push_back(MakeLine(keys, *sourcePtr, {{"category", std::string("info")}}));
    // Row 1: dict-resolved "warn".
    batch.lines.push_back(MakeLine(keys, *sourcePtr, {{"category", std::string("warn")}}));
    // Row 2: monostate (no field) -> non-DictRef.
    batch.lines.push_back(MakeLine(keys, *sourcePtr, {}));
    // Row 3: wrong-type slot (int instead of string) -> encode pass
    // refuses to dict-encode it, leaving the slot as Int64 -> non-DictRef.
    batch.lines.push_back(MakeLine(keys, *sourcePtr, {{"category", LogValue(int64_t{42})}}));
    batch.newKeys.emplace_back("category");
    AppendFixtureBatch(table, std::move(batch));

    REQUIRE(table.Configuration().Configuration().columns[0].type == ColumnType::Enumeration);
    const KeyId categoryKey = keys.Find("category");
    REQUIRE(categoryKey != INVALID_KEY_ID);
    const EnumDictionary *dict = table.EnumDictionaries().Find(categoryKey);
    REQUIRE(dict != nullptr);
    const EnumDictRank rank{*dict};

    // Sanity: rows 0 + 1 are DictRef.
    REQUIRE(table.GetEnumValueId(0, 0).has_value());
    REQUIRE(table.GetEnumValueId(1, 0).has_value());
    // Rows 2 + 3 are non-DictRef.
    REQUIRE(!table.GetEnumValueId(2, 0).has_value());
    REQUIRE(!table.GetEnumValueId(3, 0).has_value());

    // DictRef vs non-DictRef -> DictRef sorts before tail.
    CHECK(SignOf(CompareRows(table, 0, 2, 0, &rank)) == -1); // info < monostate
    CHECK(SignOf(CompareRows(table, 0, 3, 0, &rank)) == -1); // info < int-tail
    CHECK(SignOf(CompareRows(table, 1, 2, 0, &rank)) == -1); // warn < monostate
    CHECK(SignOf(CompareRows(table, 1, 3, 0, &rank)) == -1); // warn < int-tail
    CHECK(SignOf(CompareRows(table, 2, 0, 0, &rank)) == 1);  // monostate > info
    CHECK(SignOf(CompareRows(table, 3, 1, 0, &rank)) == 1);  // int-tail > warn

    // Two non-DictRef rows (monostate vs wrong-type) MUST compare
    // equal -- this is the alignment with the fast path's sentinel
    // collapse. Pre-fix this returned -1 / +1 via byte-wise compare.
    CHECK(SignOf(CompareRows(table, 2, 3, 0, &rank)) == 0);
    CHECK(SignOf(CompareRows(table, 3, 2, 0, &rank)) == 0);

    // Cross-check vs `SortPermutationByColumn`'s fast path: the two
    // non-DictRef rows must end up adjacent at the tail, in
    // input-index order (stable tie-break).
    const std::vector<size_t> logRows = {0, 1, 2, 3};
    const std::vector<size_t> perm =
        SortPermutationByColumn(table, std::span<const size_t>{logRows}, size_t{0}, /*ascending=*/true, &rank);
    REQUIRE(perm.size() == 4);
    // Front two: dict-resolved (info < warn alphabetically).
    CHECK(perm[0] == 0); // info
    CHECK(perm[1] == 1); // warn
    // Tail two: non-DictRef rows in input-index order.
    CHECK(perm[2] == 2);
    CHECK(perm[3] == 3);
}

TEST_CASE("CompareRows on enum column without rank falls back to string compare", "[log_compare][enum][fallback]")
{
    // Non-level key (`category`) keeps the column Enumeration so we
    // hit the no-rank `CompareEnum` fallback (not `CompareLevel`).
    const MemoryLogFile fixture("log_compare_enum_no_rank.json");
    fixture.Write("");
    auto source = fixture.CreateFileLineSource();
    FileLineSource *sourcePtr = source.get();
    LogConfiguration cfg;
    cfg.columns.push_back(
        {.header = "category",
         .keys = {"category"},
         .printFormat = "{}",
         .type = ColumnType::Enumeration,
         .parseFormats = {}}
    );
    const TestLogConfiguration cfgFile;
    cfgFile.Write(cfg);
    LogConfigurationManager mgr;
    mgr.Load(cfgFile.GetFilePath());
    loglib::LogTable table(mgr.Configuration());
    table.BeginStreaming(std::move(source));

    KeyIndex &keys = table.Keys();
    RowBatch batch;
    batch.firstLineNumber = 1;
    const std::vector<std::string> rowValues = {"warn", "info", "error"};
    for (const auto &v : rowValues)
    {
        batch.lines.push_back(MakeLine(keys, *sourcePtr, {{"category", std::string(v)}}));
    }
    batch.newKeys.emplace_back("category");
    AppendFixtureBatch(table, std::move(batch));

    // Compare resolved bytes directly. error < info < warn.
    CHECK(SignOf(CompareRows(table, 2, 1, 0, /*rankForEnumColumn=*/nullptr)) == -1); // error < info
    CHECK(SignOf(CompareRows(table, 1, 0, 0, /*rankForEnumColumn=*/nullptr)) == -1); // info < warn
}

TEST_CASE("CompareRows on Boolean columns orders false < true and sinks non-bool to tail", "[log_compare][boolean]")
{
    const MemoryLogFile fixture("log_compare_boolean.json");
    fixture.Write("");
    const std::vector<LogValue> values = {
        true,
        false,
        true,
        std::monostate{},
        std::string("not-a-bool"), // wrong-type slot joins the tail bucket
    };
    const LogTable table = BuildSingleColumnTable(fixture, "flag", ColumnType::Boolean, values);

    CHECK(SignOf(CompareRows(table, 0, 1, 0)) == 1);  // true > false
    CHECK(SignOf(CompareRows(table, 1, 0, 0)) == -1); // false < true
    CHECK(CompareRows(table, 0, 2, 0) == 0);          // true == true
    CHECK(SignOf(CompareRows(table, 3, 0, 0)) == 1);  // monostate > bool
    CHECK(SignOf(CompareRows(table, 0, 3, 0)) == -1);
    CHECK(SignOf(CompareRows(table, 4, 0, 0)) == 1); // wrong-type slot > bool
    CHECK(CompareRows(table, 3, 4, 0) == 0);         // tail members compare equal pairwise
}

TEST_CASE("CompareRows on String/Any columns compares byte-wise", "[log_compare][string]")
{
    const MemoryLogFile fixture("log_compare_string.json");
    fixture.Write("");
    const std::vector<LogValue> values = {
        std::string("delta"),
        std::string("alpha"),
        std::string("charlie"),
        std::monostate{},
    };
    const LogTable table = BuildSingleColumnTable(fixture, "label", ColumnType::Any, values);

    CHECK(SignOf(CompareRows(table, 0, 1, 0)) == 1);  // delta > alpha
    CHECK(SignOf(CompareRows(table, 2, 0, 0)) == -1); // charlie < delta
    CHECK(SignOf(CompareRows(table, 3, 0, 0)) == 1);  // monostate > populated
    CHECK(SignOf(CompareRows(table, 0, 3, 0)) == -1);
}

TEST_CASE("CompareRows is total: a<b iff b>a, and a=a", "[log_compare][properties]")
{
    const MemoryLogFile fixture("log_compare_total_order.json");
    fixture.Write("");
    const std::vector<LogValue> values = {int64_t{42}, int64_t{-7}, std::monostate{}, int64_t{42}};
    const LogTable table = BuildSingleColumnTable(fixture, "n", ColumnType::Integer, values);

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

TEST_CASE(
    "CompareRows on Type::Level columns orders rows by canonical severity rank, not byte order", "[log_compare][level]"
)
{
    const MemoryLogFile fixture("log_compare_level.json");
    fixture.Write("");
    // Pinned `Type::Level` with mixed-case aliases. Sort must use
    // canonical rank (Info < Warn < Error < Fatal), not bytes
    // ("ERROR" < "INFO" alphabetically).
    auto source = fixture.CreateFileLineSource();
    FileLineSource *sourcePtr = source.get();
    LogConfiguration cfg;
    cfg.columns.push_back(
        {.header = "level", .keys = {"level"}, .printFormat = "{}", .type = ColumnType::Level, .parseFormats = {}}
    );
    const TestLogConfiguration cfgFile;
    cfgFile.Write(cfg);
    LogConfigurationManager mgr;
    mgr.Load(cfgFile.GetFilePath());

    loglib::LogTable table(mgr.Configuration());
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

    REQUIRE(table.Configuration().Configuration().columns[0].type == ColumnType::Level);
    // Info(1) < Warn(0) < Error(2) < Fatal(3); the qux row joins the tail.
    CHECK(SignOf(CompareRows(table, 1, 0, 0)) == -1); // INFO < WARN
    CHECK(SignOf(CompareRows(table, 0, 2, 0)) == -1); // WARN < ERROR
    CHECK(SignOf(CompareRows(table, 2, 3, 0)) == -1); // ERROR < FATAL
    // Unmapped value sinks to the tail.
    CHECK(SignOf(CompareRows(table, 3, 4, 0)) == -1); // FATAL < qux (tail)
    CHECK(SignOf(CompareRows(table, 4, 0, 0)) == 1);  // qux (tail) > WARN
}

TEST_CASE(
    "SortPermutationByColumn fast path on Type::Level honours canonical rank in both directions",
    "[log_compare][level][sort_permutation]"
)
{
    // Regression: the `Type::Level` fast path in
    // `SortPermutationByColumn` (pre-materialised `uint8_t` ranks +
    // sentinel for unmapped slots) must agree with `CompareLevel` for
    // both ascending and descending sorts.
    const MemoryLogFile fixture("log_compare_level_sort_perm.json");
    fixture.Write("");
    auto source = fixture.CreateFileLineSource();
    FileLineSource *sourcePtr = source.get();
    LogConfiguration cfg;
    cfg.columns.push_back(
        {.header = "level", .keys = {"level"}, .printFormat = "{}", .type = ColumnType::Level, .parseFormats = {}}
    );
    const TestLogConfiguration cfgFile;
    cfgFile.Write(cfg);
    LogConfigurationManager mgr;
    mgr.Load(cfgFile.GetFilePath());

    loglib::LogTable table(mgr.Configuration());
    table.BeginStreaming(std::move(source));
    KeyIndex &keys = table.Keys();

    RowBatch batch;
    batch.firstLineNumber = 1;
    // Insertion order: info, warn, error, fatal, qux. Ranks come out
    // `Info(3) < Warn(4) < Error(5) < Fatal(6) < sentinel(255)`, so:
    //   ascending  -> [0, 1, 2, 3, 4]
    //   descending -> [4, 3, 2, 1, 0]   (sentinel sorts FIRST in
    //     descending because the primary key is `rankA > rankB`;
    //     matches the enum fast-path convention.)
    const std::vector<std::string> rowValues = {"info", "warn", "error", "fatal", "qux"};
    for (const auto &v : rowValues)
    {
        batch.lines.push_back(MakeLine(keys, *sourcePtr, {{"level", std::string(v)}}));
    }
    batch.newKeys.emplace_back("level");
    AppendFixtureBatch(table, std::move(batch));

    REQUIRE(table.Configuration().Configuration().columns[0].type == ColumnType::Level);

    const std::vector<size_t> logRows = {0, 1, 2, 3, 4};
    {
        const std::vector<size_t> perm = SortPermutationByColumn(
            table, std::span<const size_t>{logRows}, size_t{0}, /*ascending=*/true, /*rankForEnumColumn=*/nullptr
        );
        REQUIRE(perm.size() == 5);
        CHECK(perm[0] == 0); // info
        CHECK(perm[1] == 1); // warn
        CHECK(perm[2] == 2); // error
        CHECK(perm[3] == 3); // fatal
        CHECK(perm[4] == 4); // qux (tail / sentinel)
    }
    {
        const std::vector<size_t> perm = SortPermutationByColumn(
            table, std::span<const size_t>{logRows}, size_t{0}, /*ascending=*/false, /*rankForEnumColumn=*/nullptr
        );
        REQUIRE(perm.size() == 5);
        // Sentinel sorts first in descending: matches the generic
        // path (`CompareLevel` returns +1 for an unresolved lhs and
        // the generic descending comparator uses `cmp > 0`).
        CHECK(perm[0] == 4); // qux (tail / sentinel)
        CHECK(perm[1] == 3); // fatal
        CHECK(perm[2] == 2); // error
        CHECK(perm[3] == 1); // warn
        CHECK(perm[4] == 0); // info
    }
}

namespace
{

/**
 * A random cell for a column of @p type. Many duplicates, empty cells and cells of other types, so every tail group has
 * rows.
 */
LogValue RandomCell(std::mt19937 &rng, ColumnType type)
{
    const auto pick = [&rng](uint32_t bound) { return static_cast<uint32_t>(rng() % bound); };
    if (pick(20) == 0)
    {
        return std::monostate{};
    }
    switch (type)
    {
    case ColumnType::Boolean:
        return pick(25) == 0 ? LogValue{std::string("yes")} : LogValue{pick(2) == 0};
    case ColumnType::Integer:
    {
        switch (pick(12))
        {
        case 0:
            return std::numeric_limits<double>::quiet_NaN();
        case 1:
            return std::numeric_limits<uint64_t>::max() - pick(3);
        case 2:
            return static_cast<double>(pick(50)) * 1e19;
        case 3:
            return static_cast<double>(pick(10)) - 5.7;
        case 4:
            return std::string("text");
        case 5:
            return static_cast<uint64_t>(pick(40));
        default:
            return static_cast<int64_t>(pick(60)) - 30;
        }
    }
    case ColumnType::Floating:
    case ColumnType::Number:
    {
        switch (pick(12))
        {
        case 0:
            return std::numeric_limits<double>::quiet_NaN();
        case 1:
            return pick(2) == 0 ? 0.0 : -0.0;
        case 2:
            return std::numeric_limits<double>::infinity() * (pick(2) == 0 ? 1.0 : -1.0);
        case 3:
            return static_cast<int64_t>(pick(20)) - 10;
        case 4:
            return static_cast<uint64_t>(pick(20));
        case 5:
            return std::string("text");
        default:
            return (static_cast<double>(pick(200)) - 100.0) / 8.0;
        }
    }
    case ColumnType::Time:
    {
        switch (pick(10))
        {
        case 0:
            return static_cast<int64_t>(pick(30)) - 10;
        case 1:
            return static_cast<uint64_t>(pick(30));
        case 2:
            return std::numeric_limits<uint64_t>::max();
        case 3:
            return 2.5;
        default:
            return TimeStamp{std::chrono::microseconds{static_cast<int64_t>(pick(100)) - 20}};
        }
    }
    case ColumnType::Any:
    {
        // Strings next to numbers and booleans, which compare by their formatted text.
        switch (pick(6))
        {
        case 0:
            return static_cast<int64_t>(pick(30)) - 10;
        case 1:
            return static_cast<double>(pick(40)) / 4.0;
        case 2:
            return pick(2) == 0;
        default:
            return std::string(pick(2) == 0 ? "[" : "") + std::to_string(pick(25));
        }
    }
    default:
    {
        std::string text;
        const uint32_t length = pick(14);
        // Few distinct bytes and a shared prefix make ties and long equal prefixes common.
        const std::string prefix = pick(3) == 0 ? "common-prefix-" : "";
        for (uint32_t i = 0; i < length; ++i)
        {
            text.push_back(static_cast<char>("abc\x01\x80\xFF"[pick(6)]));
        }
        return prefix + text;
    }
    }
}

/**
 * A cell reduced to what the sort order of a column type looks at: a group (0 for a cell that fits the type, 1 for NaN,
 * 2 for every other cell) and, in group 0, a number or a text.
 */
struct ReferenceKey
{
    int group = 2;
    int64_t integer = 0;
    double floating = 0.0;
    std::string text;
};

/**
 * The key of a cell, written from the documented order of each column type and not from the library's comparison, so
 * the sort is checked against an independent rule.
 */
ReferenceKey ReferenceKeyOf(const LogTable &table, size_t row, size_t column, ColumnType type)
{
    const LogValue value = table.GetValue(row, column);
    ReferenceKey key;
    if (std::holds_alternative<std::monostate>(value))
    {
        return key;
    }
    constexpr auto INT_MAX_VALUE = std::numeric_limits<int64_t>::max();
    const auto clampUnsigned = [](uint64_t u) {
        return u > static_cast<uint64_t>(INT_MAX_VALUE) ? INT_MAX_VALUE : static_cast<int64_t>(u);
    };
    const auto ordinary = [&key](int64_t integer, double floating = 0.0) {
        key.group = 0;
        key.integer = integer;
        key.floating = floating;
    };
    switch (type)
    {
    case ColumnType::Boolean:
        if (const auto *b = std::get_if<bool>(&value))
        {
            ordinary(*b ? 1 : 0);
        }
        break;
    case ColumnType::Integer:
        if (const auto *i = std::get_if<int64_t>(&value))
        {
            ordinary(*i);
        }
        else if (const auto *u = std::get_if<uint64_t>(&value))
        {
            ordinary(clampUnsigned(*u));
        }
        else if (const auto *d = std::get_if<double>(&value); d != nullptr && !std::isnan(*d))
        {
            // Clamped to the int64 range, and truncated toward zero inside it.
            constexpr auto INT_MIN_VALUE = std::numeric_limits<int64_t>::min();
            if (*d >= static_cast<double>(INT_MAX_VALUE))
            {
                ordinary(INT_MAX_VALUE);
            }
            else if (*d <= static_cast<double>(INT_MIN_VALUE))
            {
                ordinary(INT_MIN_VALUE);
            }
            else
            {
                ordinary(static_cast<int64_t>(*d));
            }
        }
        break;
    case ColumnType::Floating:
    case ColumnType::Number:
    {
        std::optional<double> number;
        if (const auto *d = std::get_if<double>(&value))
        {
            number = *d;
        }
        else if (const auto *i = std::get_if<int64_t>(&value))
        {
            number = static_cast<double>(*i);
        }
        else if (const auto *u = std::get_if<uint64_t>(&value))
        {
            number = static_cast<double>(*u);
        }
        if (number.has_value())
        {
            if (std::isnan(*number))
            {
                key.group = 1;
            }
            else
            {
                ordinary(0, *number);
            }
        }
        break;
    }
    case ColumnType::Time:
        if (const auto *t = std::get_if<TimeStamp>(&value))
        {
            ordinary(t->time_since_epoch().count());
        }
        else if (const auto *i = std::get_if<int64_t>(&value))
        {
            ordinary(*i);
        }
        else if (const auto *u = std::get_if<uint64_t>(&value))
        {
            ordinary(clampUnsigned(*u));
        }
        break;
    default:
        // A string by its bytes, any other cell by its formatted text.
        key.group = 0;
        if (const auto *view = std::get_if<std::string_view>(&value))
        {
            key.text = std::string(*view);
        }
        else if (const auto *str = std::get_if<std::string>(&value))
        {
            key.text = *str;
        }
        else
        {
            key.text = table.GetFormattedValue(row, column);
        }
        break;
    }
    return key;
}

int CompareReferenceKeys(const ReferenceKey &lhs, const ReferenceKey &rhs)
{
    if (lhs.group != rhs.group)
    {
        return lhs.group < rhs.group ? -1 : 1;
    }
    if (lhs.group != 0)
    {
        return 0;
    }
    if (lhs.integer != rhs.integer)
    {
        return lhs.integer < rhs.integer ? -1 : 1;
    }
    // Zero and negative zero are equal here, as in the floating order.
    if (lhs.floating != rhs.floating)
    {
        return lhs.floating < rhs.floating ? -1 : 1;
    }
    const int cmp = lhs.text.compare(rhs.text);
    return cmp < 0 ? -1 : (cmp > 0 ? 1 : 0);
}

/**
 * The order of a stable sort on the reference keys, so rows with equal cells keep their input order. A descending sort
 * reverses the order of the keys only.
 */
std::vector<size_t> ReferencePermutation(
    const LogTable &table, const std::vector<size_t> &logRows, size_t column, ColumnType type, bool ascending
)
{
    std::vector<ReferenceKey> keys;
    keys.reserve(logRows.size());
    for (const size_t row : logRows)
    {
        keys.push_back(ReferenceKeyOf(table, row, column, type));
    }
    std::vector<size_t> permutation(logRows.size());
    std::iota(permutation.begin(), permutation.end(), size_t{0});
    std::ranges::stable_sort(permutation, [&](size_t a, size_t b) {
        const int cmp = CompareReferenceKeys(keys[a], keys[b]);
        return ascending ? cmp < 0 : cmp > 0;
    });
    return permutation;
}

} // namespace

TEST_CASE(
    "SortPermutationByColumn gives the order of a stable comparison sort for every column type",
    "[log_compare][sort_permutation][keys]"
)
{
    constexpr size_t ROWS = 10'000;
    const std::array<ColumnType, 7> types = {
        ColumnType::Boolean,
        ColumnType::Integer,
        ColumnType::Floating,
        ColumnType::Number,
        ColumnType::Time,
        ColumnType::String,
        ColumnType::Any,
    };
    for (const ColumnType type : types)
    {
        DYNAMIC_SECTION("column type " << static_cast<int>(type))
        {
            // NOLINTNEXTLINE(cert-msc32-c, cert-msc51-cpp, bugprone-random-generator-seed)
            std::mt19937 rng{0x5EED0000U + static_cast<uint32_t>(type)};
            std::vector<LogValue> cells;
            cells.reserve(ROWS);
            for (size_t i = 0; i < ROWS; ++i)
            {
                cells.push_back(RandomCell(rng, type));
            }
            const MemoryLogFile fixture("log_compare_sort_keys.json");
            fixture.Write("");
            // In a text column, a print format that changes strings too, so a string must compare by its bytes and
            // every other cell by its formatted text.
            const bool textColumn = type == ColumnType::String || type == ColumnType::Any;
            const LogTable table = BuildSingleColumnTable(fixture, "v", type, cells, textColumn ? "[{}]" : "{}");
            REQUIRE(table.RowCount() == ROWS);

            std::vector<size_t> all(ROWS);
            std::iota(all.begin(), all.end(), size_t{0});
            std::vector<size_t> shuffledSubset = all;
            std::ranges::shuffle(shuffledSubset, rng);
            shuffledSubset.resize(ROWS * 7 / 10);
            // Rows repeated, so equal positions of different input slots must keep their input order.
            std::vector<size_t> withRepeats = shuffledSubset;
            withRepeats.insert(withRepeats.end(), shuffledSubset.begin(), shuffledSubset.begin() + 500);

            // The comparison of two rows, which inserts rows into a sorted view one by one, follows the same order.
            for (size_t i = 0; i < 2'000; ++i)
            {
                const size_t lhs = rng() % ROWS;
                const size_t rhs = rng() % ROWS;
                INFO("rows " << lhs << " and " << rhs);
                CHECK(
                    SignOf(CompareRows(table, lhs, rhs, 0)) ==
                    CompareReferenceKeys(ReferenceKeyOf(table, lhs, 0, type), ReferenceKeyOf(table, rhs, 0, type))
                );
            }

            for (const bool ascending : {true, false})
            {
                for (const auto *rows : {&all, &shuffledSubset, &withRepeats})
                {
                    const std::vector<size_t> expected = ReferencePermutation(table, *rows, 0, type, ascending);
                    const std::vector<size_t> actual =
                        SortPermutationByColumn(table, std::span<const size_t>{*rows}, 0, ascending);
                    CHECK(actual == expected);

                    // A part of the order, selected without sorting the rest, is the same part of the full order.
                    const size_t count = rows->size();
                    const std::vector<std::pair<size_t, size_t>> windows = {
                        {0, 100},
                        {count - 100, count},
                        {500, 1'500},
                        {0, 0},
                        {count, count},
                        {count - 5, count + 10},
                        {3, 4},
                        {0, count},
                        {2'000, 1'000},
                    };
                    for (const auto &[begin, end] : windows)
                    {
                        INFO("window " << begin << " to " << end);
                        const size_t from = std::min(begin, count);
                        const size_t to = std::max(from, std::min(end, count));
                        const std::vector<size_t> part(
                            expected.begin() + static_cast<std::ptrdiff_t>(from),
                            expected.begin() + static_cast<std::ptrdiff_t>(to)
                        );
                        CHECK(
                            SortPermutationWindowByColumn(
                                table, std::span<const size_t>{*rows}, 0, ascending, begin, end
                            ) == part
                        );
                    }
                }
            }
        }
    }
}

TEST_CASE(
    "SortPermutationByColumn handles keys that are already in order or exactly reversed",
    "[log_compare][sort_permutation][keys]"
)
{
    constexpr size_t ROWS = 2'000;
    const std::array<ColumnType, 2> types = {ColumnType::Integer, ColumnType::Time};
    for (const ColumnType type : types)
    {
        for (const int shape : {0, 1, 2, 3})
        {
            DYNAMIC_SECTION("type " << static_cast<int>(type) << " shape " << shape)
            {
                std::vector<LogValue> cells;
                for (size_t i = 0; i < ROWS; ++i)
                {
                    // 0 ascending with ties, 1 strictly ascending, 2 strictly descending, 3 all equal.
                    int64_t value = 0;
                    switch (shape)
                    {
                    case 0:
                        value = static_cast<int64_t>(i / 3);
                        break;
                    case 1:
                        value = static_cast<int64_t>(i);
                        break;
                    case 2:
                        value = static_cast<int64_t>(ROWS - i);
                        break;
                    default:
                        value = 7;
                        break;
                    }
                    cells.emplace_back(
                        type == ColumnType::Time ? LogValue{TimeStamp{std::chrono::microseconds{value}}}
                                                 : LogValue{value}
                    );
                }
                cells[ROWS / 2] = std::monostate{};
                const MemoryLogFile fixture("log_compare_sort_keys_shape.json");
                fixture.Write("");
                const LogTable table = BuildSingleColumnTable(fixture, "v", type, cells);
                std::vector<size_t> all(ROWS);
                std::iota(all.begin(), all.end(), size_t{0});
                for (const bool ascending : {true, false})
                {
                    CHECK(
                        SortPermutationByColumn(table, std::span<const size_t>{all}, 0, ascending) ==
                        ReferencePermutation(table, all, 0, type, ascending)
                    );
                }
            }
        }
    }
}
