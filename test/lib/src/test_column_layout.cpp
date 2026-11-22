#include "common.hpp"

#include <loglib/config/filter_expression.hpp>
#include <loglib/config/internal/level_column_slot.hpp>
#include <loglib/config/internal/log_configuration_glaze_meta.hpp>
#include <loglib/config/log_configuration.hpp>
#include <loglib/core/key_index.hpp>
#include <loglib/core/log_data.hpp>
#include <loglib/core/log_level.hpp>
#include <loglib/core/log_line.hpp>

#include <test_common/temp_dir.hpp>

#include <catch2/catch_all.hpp>
#include <glaze/glaze.hpp>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

using namespace loglib;
using namespace loglib::internal;

namespace
{

/// Test helper: extract every top-level `Leaf` from an expression.
/// Empty when the expression is default-constructed (empty `And`
/// = "match every row") or when the top-level node is neither
/// `Leaf` nor `And`. Sufficient for the legacy filters-vector-
/// style assertions -- the historical simple-mode UX only ever
/// wrote flat AND trees.
[[nodiscard]] std::vector<FilterCondition> LegacyLeavesOf(const FilterExpression &expression)
{
    std::vector<FilterCondition> out;
    if (const auto *leaf = std::get_if<FilterExpression::Leaf>(&expression.node); leaf != nullptr)
    {
        out.push_back(leaf->condition);
        return out;
    }
    if (const auto *asAnd = std::get_if<FilterExpression::And>(&expression.node); asAnd != nullptr)
    {
        for (const auto &child : asAnd->children)
        {
            if (const auto *childLeaf = std::get_if<FilterExpression::Leaf>(&child.node); childLeaf != nullptr)
            {
                out.push_back(childLeaf->condition);
            }
        }
    }
    return out;
}

/// Test helper: install @p leaves as a top-level `And` expression.
[[nodiscard]] FilterExpression LeavesAsExpression(std::vector<FilterCondition> leaves)
{
    FilterExpression::And node;
    node.children.reserve(leaves.size());
    for (auto &rule : leaves)
    {
        FilterExpression child;
        child.node = FilterExpression::Leaf{std::move(rule)};
        node.children.push_back(std::move(child));
    }
    FilterExpression expression;
    expression.node = std::move(node);
    return expression;
}

/// Test helper: resolve the first column whose `keys` subset-matches @p keys.
/// Returns -1 when nothing matches or @p keys is empty. Local copy of the
/// subset-match rule so this fixture does not call `FindColumnByKeys`.
[[nodiscard]] int LegacyColumnKeysToRow(const std::vector<std::string> &keys, const std::vector<Column> &columns)
{
    if (keys.empty())
    {
        return -1;
    }
    for (size_t i = 0; i < columns.size(); ++i)
    {
        const auto &columnKeys = columns[i].keys;
        bool ok = true;
        for (const auto &k : keys)
        {
            if (std::ranges::find(columnKeys, k) == columnKeys.end())
            {
                ok = false;
                break;
            }
        }
        if (ok)
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}

} // namespace

TEST_CASE("AppendKeys with no keys should not modify configuration", "[ColumnLayout]")
{
    LogConfiguration logConfiguration;
    const Column defaultColumn = {
        .header = "test",
        .keys = {"test"},
        .printFormat = "{}",
        .type = ColumnType::Any,
        .parseFormats = {},
    };
    logConfiguration.columns.push_back(defaultColumn);
    ColumnLayout manager(logConfiguration.columns);

    const size_t initialColumnCount = manager.Columns().size();

    manager.AppendKeys({});

    CHECK(manager.Columns().size() == initialColumnCount);
    CHECK(manager.Columns()[0].header == defaultColumn.header);
    CHECK(manager.Columns()[0].keys == defaultColumn.keys);
    CHECK(manager.Columns()[0].parseFormats == defaultColumn.parseFormats);
    CHECK(manager.Columns()[0].printFormat == defaultColumn.printFormat);
    CHECK(manager.Columns()[0].type == defaultColumn.type);
}

TEST_CASE(
    "AppendKeys recognises common timestamp aliases as Type::Time and ignores bare 't'",
    "[ColumnLayout][timestamp_keys][regression]"
)
{
    // Regression: bare `t` used to be auto-promoted to a date column.
    // The well-known aliases (`@timestamp`, `datetime`, `created_at`,
    // `ts`) now go through the same promotion path.
    ColumnLayout manager;
    manager.AppendKeys({"timestamp", "time", "ts", "@timestamp", "datetime", "created_at", "t", "tag"});

    const auto &columns = manager.Columns();
    REQUIRE(columns.size() == 8);

    auto findColumn = [&columns](std::string_view header) {
        return std::ranges::find_if(columns, [header](const auto &c) { return c.header == header; });
    };

    const auto checkType = [&](std::string_view header, ColumnType expected) {
        const auto it = findColumn(header);
        REQUIRE(it != columns.end());
        CHECK(it->type == expected);
    };

    checkType("timestamp", ColumnType::Time);
    checkType("time", ColumnType::Time);
    checkType("ts", ColumnType::Time);
    checkType("@timestamp", ColumnType::Time);
    checkType("datetime", ColumnType::Time);
    checkType("created_at", ColumnType::Time);

    checkType("t", ColumnType::Any);
    checkType("tag", ColumnType::Any);
}

TEST_CASE("AppendKeys with mixed keys seeds the timestamp column", "[ColumnLayout]")
{
    LogConfiguration logConfiguration;
    logConfiguration.columns.push_back(
        {.header = "regular", .keys = {"regular"}, .printFormat = "{}", .type = ColumnType::Any, .parseFormats = {}}
    );
    ColumnLayout manager(logConfiguration.columns);

    manager.AppendKeys({"regular", "newKey", "timestamp"});

    REQUIRE(manager.Columns().size() == 3);

    CHECK(manager.Columns()[0].header == "regular");
    CHECK(manager.Columns()[1].header == "newKey");
    CHECK(manager.Columns()[2].header == "timestamp");
    CHECK(manager.Columns()[2].type == ColumnType::Time);

    CHECK(manager.Columns()[2].printFormat == "%F %H:%M:%S");
    // Default seed keeps ISO 8601 first (fast path via
    // `Iso8601T` / `Iso8601Space`) and appends non-ISO shapes
    // (Apache CLF at present) so shipped regex-template
    // timestamps promote without an editor round-trip. See
    // `DefaultTimeParseFormats` for the authoritative list.
    const auto &seeded = manager.Columns()[2].parseFormats;
    REQUIRE(seeded == loglib::DefaultTimeParseFormats());
    CHECK(seeded.front() == "%FT%T%Ez");
    CHECK(std::ranges::find(seeded, std::string("%d/%b/%Y:%H:%M:%S %z")) != seeded.end());
}

// `IsKeyInAnyColumn` cache-invalidation tests: every mutating path
// (`Load`, `AppendKeys`) must invalidate the cache.

TEST_CASE("Cache: AppendKeys after a query sees the freshly appended key", "[ColumnLayout][cache_invalidation]")
{
    ColumnLayout manager;

    manager.AppendKeys({"alpha"});
    REQUIRE(manager.Columns().size() == 1);
    CHECK(manager.Columns()[0].header == "alpha");

    // Duplicate "alpha" inside the call must not double-add.
    manager.AppendKeys({"alpha", "beta", "alpha"});
    REQUIRE(manager.Columns().size() == 2);
    CHECK(manager.Columns()[0].header == "alpha");
    CHECK(manager.Columns()[1].header == "beta");
}

TEST_CASE(
    "Cache: a second AppendKeys auto-promotes timestamps with the cache fresh", "[ColumnLayout][cache_invalidation]"
)
{
    ColumnLayout manager;

    manager.AppendKeys({"regular"});
    REQUIRE(manager.Columns().size() == 1);

    // The second call must skip "regular" and add "timestamp" as Type::Time.
    manager.AppendKeys({"regular", "timestamp"});

    REQUIRE(manager.Columns().size() == 2);
    CHECK(manager.Columns()[0].header == "regular");
    CHECK(manager.Columns()[1].header == "timestamp");
    CHECK(manager.Columns()[1].type == ColumnType::Time);
}

TEST_CASE("Cache: parity with the pre-cache O(M*K) walk on the existing fixture", "[ColumnLayout][cache_invalidation]")
{
    // Regression: the cached path matches the old O(M*K) walk on the
    // "timestamp first" fixture.
    LogConfiguration logConfiguration;
    logConfiguration.columns.push_back(
        {.header = "regular", .keys = {"regular"}, .printFormat = "{}", .type = ColumnType::Any, .parseFormats = {}}
    );
    ColumnLayout manager(logConfiguration.columns);

    manager.AppendKeys({"regular", "newKey", "timestamp"});

    // New keys go to the end in the order they are given.
    REQUIRE(manager.Columns().size() == 3);
    CHECK(manager.Columns()[0].header == "regular");
    CHECK(manager.Columns()[1].header == "newKey");
    CHECK(manager.Columns()[2].header == "timestamp");
}

TEST_CASE("Cache: a column whose `keys` differs from `header` is matched by key", "[ColumnLayout][cache_invalidation]")
{
    // Cache keys on `column.keys`, not `column.header`: AppendKeys must
    // skip keys already listed under any column's `keys`.
    LogConfiguration logConfiguration;
    logConfiguration.columns.push_back({
        .header = "display",
        .keys = {"raw_key", "alias"},
        .printFormat = "{}",
        .type = ColumnType::Any,
        .parseFormats = {},
    });
    ColumnLayout manager(logConfiguration.columns);

    manager.AppendKeys({"display", "raw_key", "alias", "fresh"});

    // Only "display" and "fresh" are added ("raw_key"/"alias" hit the cache).
    REQUIRE(manager.Columns().size() == 3);
    CHECK(manager.Columns()[0].header == "display");
    CHECK(manager.Columns()[1].header == "display"); // header reused; keys differ
    CHECK(manager.Columns()[2].header == "fresh");
    CHECK(manager.Columns()[1].keys == std::vector<std::string>{"display"});
    CHECK(manager.Columns()[2].keys == std::vector<std::string>{"fresh"});
}

TEST_CASE(
    "Newly-discovered keys default to Type::Any with autoDetect=true so the auto-detector scans them",
    "[log_configuration][auto_detect_default]"
)
{
    // Provenance is carried by the `(type, autoDetect)` pair: a fresh
    // key starts as `(Type::Any, autoDetect=true)`, i.e. the
    // auto-detector candidate state.

    SECTION("AppendKeys assigns the candidate state to fresh keys")
    {
        ColumnLayout manager;
        manager.AppendKeys({"level"});
        REQUIRE(manager.Columns().size() == 1);
        CHECK(manager.Columns()[0].type == ColumnType::Any);
        CHECK(manager.Columns()[0].autoDetect);
    }

    SECTION("AppendKeys assigns the candidate state to every freshly-added non-time key")
    {
        ColumnLayout manager;
        manager.AppendKeys({"k1", "k2"});

        REQUIRE(manager.Columns().size() == 2);
        for (const auto &column : manager.Columns())
        {
            CHECK(column.type == ColumnType::Any);
            CHECK(column.autoDetect);
        }
    }
}

TEST_CASE(
    "ColumnLayout::SetColumnHeader renames the display label without touching keys", "[ColumnLayout][column_header]"
)
{
    // `header` is the display string the GUI shows; `keys` are the
    // parser identifiers and must stay untouched so the existing
    // column binding survives. The mutator is the only path the
    // Column Editor takes to rename a column, so a hand-on-the-wheel
    // test pins that one-way invariant.
    ColumnLayout manager;
    manager.AppendKeys({"abc", "xyz"});
    REQUIRE(manager.Columns().size() == 2);

    manager.SetColumnHeader(0, "Alpha");
    CHECK(manager.Columns()[0].header == "Alpha");
    CHECK(manager.Columns()[0].keys == std::vector<std::string>{"abc"});
    CHECK(manager.Columns()[1].header == "xyz");

    // Empty rename is intentionally allowed -- users can clear the
    // header without us second-guessing them; the table falls back
    // to the keys via `LogTable::GetHeader`.
    manager.SetColumnHeader(1, "");
    CHECK(manager.Columns()[1].header.empty());

    // Out-of-range index must be a silent no-op (matches the rest of
    // the SetColumn* family). The valid columns stay untouched.
    manager.SetColumnHeader(99, "Bogus");
    REQUIRE(manager.Columns().size() == 2);
    CHECK(manager.Columns()[0].header == "Alpha");
}

namespace
{
void BubbleLevelColumnToCanonicalPosition(ColumnLayout &layout, size_t columnIndex)
{
    if (ShouldBubbleLevelColumn(layout, columnIndex))
    {
        layout.MoveColumn(columnIndex, CANONICAL_LEVEL_COLUMN_INDEX);
    }
}
} // namespace

TEST_CASE("Bubbling the Level column mirrors the Time bubble", "[ColumnLayout][level_bubble]")
{
    // Manager-level coverage of the free helper. Catches rotation
    // regressions independent of `LogTable::MaybePromoteToLevel`
    // (which is covered in `test_log_table.cpp`).
    SECTION("Move from end to canonical position 1")
    {
        ColumnLayout manager;
        manager.AppendKeys({"col_zero", "col_one", "level"});
        REQUIRE(manager.Columns().size() == 3);
        REQUIRE(manager.Columns()[2].header == "level");

        BubbleLevelColumnToCanonicalPosition(manager, 2);

        REQUIRE(manager.Columns().size() == 3);
        CHECK(manager.Columns()[0].header == "col_zero");
        CHECK(manager.Columns()[1].header == "level");
        CHECK(manager.Columns()[2].header == "col_one");
    }

    SECTION("No-op when already at canonical position")
    {
        ColumnLayout manager;
        manager.AppendKeys({"col_zero", "level", "col_two"});

        BubbleLevelColumnToCanonicalPosition(manager, 1);

        CHECK(manager.Columns()[0].header == "col_zero");
        CHECK(manager.Columns()[1].header == "level");
        CHECK(manager.Columns()[2].header == "col_two");
    }

    SECTION("No-op for a single-column configuration")
    {
        // Only column -- nothing to swap with; must not throw.
        ColumnLayout manager;
        manager.AppendKeys({"level"});

        BubbleLevelColumnToCanonicalPosition(manager, 0);

        REQUIRE(manager.Columns().size() == 1);
        CHECK(manager.Columns()[0].header == "level");
    }

    SECTION("Move from position 0 to position 1 in a two-column config")
    {
        ColumnLayout manager;
        manager.AppendKeys({"level", "col_one"});

        BubbleLevelColumnToCanonicalPosition(manager, 0);

        REQUIRE(manager.Columns().size() == 2);
        CHECK(manager.Columns()[0].header == "col_one");
        CHECK(manager.Columns()[1].header == "level");
    }

    SECTION("Out-of-range index is a no-op")
    {
        ColumnLayout manager;
        manager.AppendKeys({"col_zero", "col_one"});

        BubbleLevelColumnToCanonicalPosition(manager, 99);

        REQUIRE(manager.Columns().size() == 2);
        CHECK(manager.Columns()[0].header == "col_zero");
        CHECK(manager.Columns()[1].header == "col_one");
    }
}

TEST_CASE("IsLogLevelKey recognises canonical level aliases case-insensitively", "[ColumnLayout][log_level]")
{
    // Long-form / classic.
    CHECK(IsLogLevelKey("level"));
    CHECK(IsLogLevelKey("Level"));
    CHECK(IsLogLevelKey("LEVEL"));
    CHECK(IsLogLevelKey("severity"));
    CHECK(IsLogLevelKey("Severity"));
    CHECK(IsLogLevelKey("loglevel"));
    CHECK(IsLogLevelKey("LogLevel"));
    CHECK(IsLogLevelKey("log_level"));
    CHECK(IsLogLevelKey("LOG_LEVEL"));
    CHECK(IsLogLevelKey("log.level"));
    CHECK(IsLogLevelKey("Log.Level"));
    CHECK(IsLogLevelKey("lvl"));
    CHECK(IsLogLevelKey("LVL"));
    CHECK(IsLogLevelKey("levelname"));
    CHECK(IsLogLevelKey("LevelName"));
    CHECK(IsLogLevelKey("priority"));
    CHECK(IsLogLevelKey("Priority"));

    // Short forms. The dictionary-content check in
    // `MaybePromoteToLevel` is the safety net against false positives.
    CHECK(IsLogLevelKey("l"));
    CHECK(IsLogLevelKey("L"));
    CHECK(IsLogLevelKey("lv"));
    CHECK(IsLogLevelKey("LV"));
    CHECK(IsLogLevelKey("lev"));
    CHECK(IsLogLevelKey("Lev"));
    CHECK(IsLogLevelKey("sev"));
    CHECK(IsLogLevelKey("SEV"));
    CHECK(IsLogLevelKey("s"));
    CHECK(IsLogLevelKey("S"));
    CHECK(IsLogLevelKey("loglvl"));
    CHECK(IsLogLevelKey("LogLvl"));

    // OpenTelemetry / ECS / GCP.
    CHECK(IsLogLevelKey("severity_text"));
    CHECK(IsLogLevelKey("Severity_Text"));
    CHECK(IsLogLevelKey("severity.text"));
    CHECK(IsLogLevelKey("severitytext"));
    CHECK(IsLogLevelKey("SeverityText"));
    CHECK(IsLogLevelKey("log_severity"));
    CHECK(IsLogLevelKey("log.severity"));
    CHECK(IsLogLevelKey("logseverity"));
    CHECK(IsLogLevelKey("LogSeverity"));

    // Separator variants of `levelname`.
    CHECK(IsLogLevelKey("level_name"));
    CHECK(IsLogLevelKey("Level_Name"));
    CHECK(IsLogLevelKey("level.name"));

    // Structured-JSON convention.
    CHECK(IsLogLevelKey("@level"));
    CHECK(IsLogLevelKey("@LEVEL"));

    // Non-matches: exact equality, not prefix / substring.
    CHECK_FALSE(IsLogLevelKey("status"));
    CHECK_FALSE(IsLogLevelKey("timestamp"));
    CHECK_FALSE(IsLogLevelKey(""));
    CHECK_FALSE(IsLogLevelKey("levelish"));
    CHECK_FALSE(IsLogLevelKey("la"));
    CHECK_FALSE(IsLogLevelKey("lengthy"));
    CHECK_FALSE(IsLogLevelKey("severities"));
    CHECK_FALSE(IsLogLevelKey("ll"));
    CHECK_FALSE(IsLogLevelKey(" level"));
    CHECK_FALSE(IsLogLevelKey("level "));
}

TEST_CASE("FirstTimeColumnIndex returns the first Type::Time column or nullopt", "[ColumnLayout][time_column]")
{
    using Type = ColumnType;

    const LogConfiguration empty;
    CHECK(FirstTimeColumnIndex(ColumnLayout(empty.columns)) == std::nullopt);

    LogConfiguration noTime;
    noTime.columns.push_back({.header = "msg", .type = Type::String});
    noTime.columns.push_back({.header = "level", .type = Type::Level});
    CHECK(FirstTimeColumnIndex(ColumnLayout(noTime.columns)) == std::nullopt);

    LogConfiguration single;
    single.columns.push_back({.header = "msg", .type = Type::String});
    single.columns.push_back({.header = "ts", .type = Type::Time});
    single.columns.push_back({.header = "level", .type = Type::Level});
    CHECK(FirstTimeColumnIndex(ColumnLayout(single.columns)) == 1);

    // First wins: callers rely on this "canonical column" behaviour.
    LogConfiguration twoTimes;
    twoTimes.columns.push_back({.header = "started_at", .type = Type::Time});
    twoTimes.columns.push_back({.header = "msg", .type = Type::String});
    twoTimes.columns.push_back({.header = "ended_at", .type = Type::Time});
    CHECK(FirstTimeColumnIndex(ColumnLayout(twoTimes.columns)) == 0);

    // Index 0 is a real result, not the "not found" sentinel.
    LogConfiguration headTime;
    headTime.columns.push_back({.header = "ts", .type = Type::Time});
    CHECK(FirstTimeColumnIndex(ColumnLayout(headTime.columns)) == 0);
}

TEST_CASE("ParseLevelName matches the documented alias table", "[log_level]")
{
    // Every alias in `BUILTIN_ALIASES`, plus mixed-case spellings for
    // each new short-form/library-specific entry. Keeps the table in
    // sync with the header docstring.
    using Expected = std::pair<std::string_view, LogLevel>;
    constexpr std::array<Expected, 52> CASES = {
        {
            // Trace
            {"trace", LogLevel::Trace},
            {"TRACE", LogLevel::Trace},
            {"trc", LogLevel::Trace},
            {"Trc", LogLevel::Trace},
            {"t", LogLevel::Trace},
            {"T", LogLevel::Trace},
            {"finer", LogLevel::Trace},
            {"Finer", LogLevel::Trace},
            {"finest", LogLevel::Trace},
            {"FINEST", LogLevel::Trace},
            {"silly", LogLevel::Trace},
            {"Silly", LogLevel::Trace},
            // Debug
            {"debug", LogLevel::Debug},
            {"dbg", LogLevel::Debug},
            {"d", LogLevel::Debug},
            {"D", LogLevel::Debug},
            {"verbose", LogLevel::Trace},
            {"vrb", LogLevel::Trace},
            {"VRB", LogLevel::Trace},
            {"v", LogLevel::Trace},
            {"V", LogLevel::Trace},
            {"fine", LogLevel::Debug},
            {"Fine", LogLevel::Debug},
            // Info
            {"info", LogLevel::Info},
            {"inf", LogLevel::Info},
            {"INF", LogLevel::Info},
            {"i", LogLevel::Info},
            {"I", LogLevel::Info},
            {"Information", LogLevel::Info},
            {"informational", LogLevel::Info},
            {"notice", LogLevel::Info},
            // Warn
            {"warn", LogLevel::Warn},
            {"WARNING", LogLevel::Warn},
            {"wrn", LogLevel::Warn},
            {"WRN", LogLevel::Warn},
            {"w", LogLevel::Warn},
            {"W", LogLevel::Warn},
            // Error
            {"error", LogLevel::Error},
            {"ERR", LogLevel::Error},
            {"severe", LogLevel::Error},
            {"e", LogLevel::Error},
            {"E", LogLevel::Error},
            // Fatal
            {"fatal", LogLevel::Fatal},
            {"critical", LogLevel::Fatal},
            {"crit", LogLevel::Fatal},
            {"emerg", LogLevel::Fatal},
            {"ftl", LogLevel::Fatal},
            {"FTL", LogLevel::Fatal},
            {"f", LogLevel::Fatal},
            {"F", LogLevel::Fatal},
            {"fault", LogLevel::Fatal},
            {"Fault", LogLevel::Fatal},
        },
    };
    for (const auto &[input, expected] : CASES)
    {
        CAPTURE(input);
        const auto actual = ParseLevelName(input);
        REQUIRE(actual.has_value());
        CHECK(*actual == expected);
    }

    CHECK_FALSE(ParseLevelName("").has_value());
    CHECK_FALSE(ParseLevelName("INFOZ").has_value()); // near-miss of "info"; alias table is exact-match only
    CHECK_FALSE(ParseLevelName("unknown").has_value());
    CHECK_FALSE(ParseLevelName("\\t info").has_value());
    // Deliberately *not* built-in (see header + README). Catches
    // drift if they're added without updating the docs.
    CHECK_FALSE(ParseLevelName("config").has_value());
    CHECK_FALSE(ParseLevelName("default").has_value());
    CHECK_FALSE(ParseLevelName("http").has_value());
    // Numeric-string levels: handled via `levelMapping`, not built-in.
    CHECK_FALSE(ParseLevelName("10").has_value());
    CHECK_FALSE(ParseLevelName("30").has_value());
    CHECK_FALSE(ParseLevelName("0").has_value());
}

TEST_CASE("CanonicalLevelName mirrors the LogLevel enum", "[log_level]")
{
    CHECK(CanonicalLevelName(LogLevel::Trace) == "Trace");
    CHECK(CanonicalLevelName(LogLevel::Debug) == "Debug");
    CHECK(CanonicalLevelName(LogLevel::Info) == "Info");
    CHECK(CanonicalLevelName(LogLevel::Warn) == "Warn");
    CHECK(CanonicalLevelName(LogLevel::Error) == "Error");
    CHECK(CanonicalLevelName(LogLevel::Fatal) == "Fatal");
    CHECK(CanonicalLevelName(LogLevel::Unknown) == "Unknown");
}

TEST_CASE("ResolveLevel honours per-column alias overrides", "[log_level]")
{
    const std::vector<std::pair<std::string, std::string>> overrides{
        {"PANIC", "Fatal"},
        // Bogus canonical name -- ignored.
        {"WAT", "NotARealLevel"},
        // Re-map a built-in alias to a different level.
        {"warn", "Error"},
        // Numeric mapping: numbers are deliberately *not* in the
        // built-in table, but `levelMapping` makes them work when the
        // producer emits them as JSON strings.
        {"30", "Info"},
        {"40", "Warn"},
    };

    CHECK(ResolveLevel("PANIC", overrides) == LogLevel::Fatal);
    // Override takes precedence over built-in.
    CHECK(ResolveLevel("warn", overrides) == LogLevel::Error);
    // Invalid override falls through to built-in (no match here).
    CHECK_FALSE(ResolveLevel("WAT", overrides).has_value());
    // Built-in alias still works for entries not in overrides.
    CHECK(ResolveLevel("info", overrides) == LogLevel::Info);
    // Numeric strings only resolve through the per-column mapping.
    CHECK(ResolveLevel("30", overrides) == LogLevel::Info);
    CHECK(ResolveLevel("40", overrides) == LogLevel::Warn);
    CHECK_FALSE(ResolveLevel("50", overrides).has_value());
    // New short-form built-ins still resolve when no override matches.
    CHECK(ResolveLevel("inf", overrides) == LogLevel::Info);
    CHECK(ResolveLevel("i", overrides) == LogLevel::Info);
    CHECK(ResolveLevel("ftl", overrides) == LogLevel::Fatal);
}

// -----------------------------------------------------------------------------
// Schema back/forward-compat tests pinning `error_on_unknown_keys=false`
// (configured in glaze_options.hpp). Without that
// option every pre-widening session JSON (using the old `"locator"`
// field) would throw on load.
// -----------------------------------------------------------------------------

TEST_CASE("MoveColumn rotates the columns and keeps their keys", "[ColumnLayout][move_column]")
{
    ColumnLayout layout;
    layout.AppendKeys({"a", "b", "c", "d"});

    layout.MoveColumn(2, 0);
    CHECK(layout.Columns()[0].keys == std::vector<std::string>{"c"});
    CHECK(layout.Columns()[1].keys == std::vector<std::string>{"a"});
    CHECK(layout.Columns()[2].keys == std::vector<std::string>{"b"});
    CHECK(layout.Columns()[3].keys == std::vector<std::string>{"d"});

    layout.MoveColumn(0, 3);
    CHECK(layout.Columns()[0].keys == std::vector<std::string>{"a"});
    CHECK(layout.Columns()[3].keys == std::vector<std::string>{"c"});

    // An index out of range or an unchanged position does nothing.
    layout.MoveColumn(1, 1);
    layout.MoveColumn(9, 0);
    layout.MoveColumn(0, 9);
    CHECK(layout.Columns().size() == 4);
    CHECK(layout.Columns()[0].keys == std::vector<std::string>{"a"});
}
