// Focused tests for the shared `leaf_rule_compile` translation unit
// (`ResolveLeafColumnByKeys`, `CompileLeaf`, `CompileExpression`).
//
// The monolithic `apptest` covers the compiler indirectly via the
// filter model rebuild path; this binary drills into the per-`LeafRule`
// branches so a regression in an "absent leaf" arm shows up as a
// focused failure instead of a distant model-rebuild diff.
//
// Covered: every `CompileLeaf` arm's absent-branch (unresolved column,
// missing bounds/values/needle, String empty-needle rejection for non-
// `Exactly` match kinds, Enumeration without a table, Level column
// without a rank cache), plus the `CompileExpression` combinator
// propagation rules (`And` drops absent children, empty input `And`
// stays match-all, all-absent input `And`/`Or`/`Not` propagates absent
// -> the CompileExpression fall-through renders match-all, and the
// null-child `Not` sugar renders match-all).

#include "leaf_rule_compile.hpp"

#include <loglib/filter_expression.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_filter.hpp>
#include <loglib/log_table.hpp>

#include <QtTest/QtTest>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace
{

using Column = loglib::LogConfiguration::Column;
using Leaf = loglib::LeafRule;

/// One-column fixture bound to @p key.
[[nodiscard]] Column MakeColumn(std::string header, std::string key, loglib::LogConfiguration::Type type)
{
    Column col;
    col.header = std::move(header);
    col.keys = {std::move(key)};
    col.type = type;
    return col;
}

[[nodiscard]] Leaf MakeStringLeaf(std::string key, loglib::LeafRule::Match matchType, std::optional<std::string> needle)
{
    Leaf rule;
    rule.type = Leaf::Type::String;
    rule.columnKeys = {std::move(key)};
    rule.matchType = matchType;
    rule.filterString = std::move(needle);
    return rule;
}

[[nodiscard]] Leaf MakeNumberLeaf(std::string key, std::optional<double> minValue, std::optional<double> maxValue)
{
    Leaf rule;
    rule.type = Leaf::Type::Number;
    rule.columnKeys = {std::move(key)};
    rule.filterMinValue = minValue;
    rule.filterMaxValue = maxValue;
    return rule;
}

[[nodiscard]] Leaf MakeTimeLeaf(std::string key, std::optional<std::int64_t> begin, std::optional<std::int64_t> end)
{
    Leaf rule;
    rule.type = Leaf::Type::Time;
    rule.columnKeys = {std::move(key)};
    rule.filterBegin = begin;
    rule.filterEnd = end;
    return rule;
}

[[nodiscard]] Leaf MakeBooleanLeaf(std::string key, std::vector<std::string> values)
{
    Leaf rule;
    rule.type = Leaf::Type::Boolean;
    rule.columnKeys = {std::move(key)};
    rule.filterValues = std::move(values);
    return rule;
}

[[nodiscard]] Leaf MakeEnumLeaf(std::string key, std::vector<std::string> values)
{
    Leaf rule;
    rule.type = Leaf::Type::Enumeration;
    rule.columnKeys = {std::move(key)};
    rule.filterValues = std::move(values);
    return rule;
}

[[nodiscard]] const loglib::CompiledFilterExpression::Leaf *AsCompiledLeaf(const loglib::CompiledFilterExpression &expr)
{
    return std::get_if<loglib::CompiledFilterExpression::Leaf>(&expr.node);
}

[[nodiscard]] const loglib::CompiledFilterExpression::And *AsCompiledAnd(const loglib::CompiledFilterExpression &expr)
{
    return std::get_if<loglib::CompiledFilterExpression::And>(&expr.node);
}

[[nodiscard]] const loglib::CompiledFilterExpression::Or *AsCompiledOr(const loglib::CompiledFilterExpression &expr)
{
    return std::get_if<loglib::CompiledFilterExpression::Or>(&expr.node);
}

} // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage): `Q_OBJECT` QtTest fixture.
class LeafRuleCompileTest : public QObject
{
    Q_OBJECT

private slots:
    // ---- ResolveLeafColumnByKeys ------------------------------------------

    /// Empty key list is invalid -> -1. Callers guard against empty
    /// keys so the compile step doesn't accidentally bind to the
    /// first column with an empty key vector.
    void ResolveEmptyKeysReturnsMinusOne()
    {
        const std::vector<Column> columns{MakeColumn("level", "level", loglib::LogConfiguration::Type::String)};
        QCOMPARE(ResolveLeafColumnByKeys({}, columns), -1);
    }

    /// No column carries the requested key -> -1.
    void ResolveKeysNoMatchReturnsMinusOne()
    {
        const std::vector<Column> columns{MakeColumn("level", "level", loglib::LogConfiguration::Type::String)};
        QCOMPARE(ResolveLeafColumnByKeys({"missing"}, columns), -1);
    }

    /// First column whose keys are a superset of the rule's keys wins.
    /// The rule stores canonical keys ("level"), the column may carry
    /// aliases too ("level", "log.level"); the subset check accepts.
    void ResolveKeysSubsetMatchesFirstColumn()
    {
        Column withAlias;
        withAlias.header = "level";
        withAlias.keys = {"level", "log.level"};
        withAlias.type = loglib::LogConfiguration::Type::String;
        const std::vector<Column> columns{withAlias};
        QCOMPARE(ResolveLeafColumnByKeys({"level"}, columns), 0);
    }

    /// Multiple columns; return the first index that matches.
    void ResolveKeysReturnsFirstMatchIndex()
    {
        const std::vector<Column> columns{
            MakeColumn("service", "service", loglib::LogConfiguration::Type::String),
            MakeColumn("level", "level", loglib::LogConfiguration::Type::String),
        };
        QCOMPARE(ResolveLeafColumnByKeys({"level"}, columns), 1);
    }

    // ---- CompileLeaf: resolvedColumn guard --------------------------------

    /// A negative resolved-column short-circuits every arm; callers
    /// pass `-1` when `ResolveLeafColumnByKeys` returned no match.
    void CompileLeafUnresolvedColumnAbsent()
    {
        const std::vector<Column> columns{MakeColumn("level", "level", loglib::LogConfiguration::Type::String)};
        const Leaf rule = MakeStringLeaf("level", Leaf::Match::Contains, "warn");
        QVERIFY(!CompileLeaf(rule, /*resolvedColumn=*/-1, columns, /*table=*/nullptr).has_value());
    }

    // ---- CompileLeaf: Time -------------------------------------------------

    /// Time with neither begin nor end is inert; the visitor would
    /// otherwise accept every row (INT64_MIN..INT64_MAX).
    void CompileTimeLeafNoBoundsAbsent()
    {
        const std::vector<Column> columns{MakeColumn("ts", "ts", loglib::LogConfiguration::Type::Time)};
        const Leaf rule = MakeTimeLeaf("ts", std::nullopt, std::nullopt);
        QVERIFY(!CompileLeaf(rule, 0, columns, /*table=*/nullptr).has_value());
    }

    /// Lower bound only -> concrete predicate; upper defaults to
    /// INT64_MAX so the visitor stays a simple two-sided compare.
    void CompileTimeLeafLowerOnlyProduces()
    {
        const std::vector<Column> columns{MakeColumn("ts", "ts", loglib::LogConfiguration::Type::Time)};
        const Leaf rule = MakeTimeLeaf("ts", std::int64_t{0}, std::nullopt);
        const auto compiled = CompileLeaf(rule, 0, columns, /*table=*/nullptr);
        QVERIFY(compiled.has_value());
        QVERIFY(std::holds_alternative<loglib::TimeRangeRowPredicate>(*compiled));
    }

    /// Upper bound only -> same predicate shape; lower defaults to
    /// INT64_MIN.
    void CompileTimeLeafUpperOnlyProduces()
    {
        const std::vector<Column> columns{MakeColumn("ts", "ts", loglib::LogConfiguration::Type::Time)};
        const Leaf rule = MakeTimeLeaf("ts", std::nullopt, std::int64_t{1000});
        const auto compiled = CompileLeaf(rule, 0, columns, /*table=*/nullptr);
        QVERIFY(compiled.has_value());
    }

    /// Both bounds set -> the canonical case.
    void CompileTimeLeafBothBoundsProduces()
    {
        const std::vector<Column> columns{MakeColumn("ts", "ts", loglib::LogConfiguration::Type::Time)};
        const Leaf rule = MakeTimeLeaf("ts", std::int64_t{0}, std::int64_t{1000});
        const auto compiled = CompileLeaf(rule, 0, columns, /*table=*/nullptr);
        QVERIFY(compiled.has_value());
    }

    // ---- CompileLeaf: Number ----------------------------------------------

    /// Number with neither min nor max is inert (analogous to Time).
    void CompileNumberLeafNoBoundsAbsent()
    {
        const std::vector<Column> columns{MakeColumn("latency", "latency", loglib::LogConfiguration::Type::Number)};
        const Leaf rule = MakeNumberLeaf("latency", std::nullopt, std::nullopt);
        QVERIFY(!CompileLeaf(rule, 0, columns, /*table=*/nullptr).has_value());
    }

    /// Lower bound only -> `NumericRangeRowPredicate`.
    void CompileNumberLeafLowerOnlyProduces()
    {
        const std::vector<Column> columns{MakeColumn("latency", "latency", loglib::LogConfiguration::Type::Number)};
        const Leaf rule = MakeNumberLeaf("latency", 100.0, std::nullopt);
        const auto compiled = CompileLeaf(rule, 0, columns, /*table=*/nullptr);
        QVERIFY(compiled.has_value());
        QVERIFY(std::holds_alternative<loglib::NumericRangeRowPredicate>(*compiled));
    }

    /// Upper bound only.
    void CompileNumberLeafUpperOnlyProduces()
    {
        const std::vector<Column> columns{MakeColumn("latency", "latency", loglib::LogConfiguration::Type::Number)};
        const Leaf rule = MakeNumberLeaf("latency", std::nullopt, 100.0);
        const auto compiled = CompileLeaf(rule, 0, columns, /*table=*/nullptr);
        QVERIFY(compiled.has_value());
    }

    // ---- CompileLeaf: Boolean ---------------------------------------------

    /// Empty `filterValues` is inert (the parser rejects `IN []`
    /// too, but hand-edited configs can produce this shape).
    void CompileBooleanLeafEmptyValuesAbsent()
    {
        const std::vector<Column> columns{MakeColumn("ok", "ok", loglib::LogConfiguration::Type::Boolean)};
        const Leaf rule = MakeBooleanLeaf("ok", {});
        QVERIFY(!CompileLeaf(rule, 0, columns, /*table=*/nullptr).has_value());
    }

    /// Populated `filterValues` but no `true` / `false` after
    /// case-insensitive decode -> inert (a config typo like
    /// `filterValues={"maybe"}` decodes to
    /// `!includeTrue && !includeFalse`).
    void CompileBooleanLeafOnlyGarbageValuesAbsent()
    {
        const std::vector<Column> columns{MakeColumn("ok", "ok", loglib::LogConfiguration::Type::Boolean)};
        const Leaf rule = MakeBooleanLeaf("ok", {"maybe", "yes", "no"});
        QVERIFY(!CompileLeaf(rule, 0, columns, /*table=*/nullptr).has_value());
    }

    /// Only "true" selected -> `BoolRowPredicate` accepting only
    /// true rows.
    void CompileBooleanLeafTrueOnlyProduces()
    {
        const std::vector<Column> columns{MakeColumn("ok", "ok", loglib::LogConfiguration::Type::Boolean)};
        const Leaf rule = MakeBooleanLeaf("ok", {"true"});
        const auto compiled = CompileLeaf(rule, 0, columns, /*table=*/nullptr);
        QVERIFY(compiled.has_value());
        QVERIFY(std::holds_alternative<loglib::BoolRowPredicate>(*compiled));
    }

    /// Case-insensitive decode -> `True` / `FALSE` still map to
    /// `true` / `false`. Guards against a hand-edited configuration
    /// silently falling through to inert.
    void CompileBooleanLeafCaseInsensitive()
    {
        const std::vector<Column> columns{MakeColumn("ok", "ok", loglib::LogConfiguration::Type::Boolean)};
        const Leaf rule = MakeBooleanLeaf("ok", {"True", "FALSE"});
        QVERIFY(CompileLeaf(rule, 0, columns, /*table=*/nullptr).has_value());
    }

    // ---- CompileLeaf: Enumeration -----------------------------------------

    /// Empty `filterValues` -> inert (parser rejects `IN []` at
    /// enum columns too, but hand-edited configs can produce it).
    void CompileEnumLeafEmptyValuesAbsent()
    {
        const std::vector<Column> columns{MakeColumn("level", "level", loglib::LogConfiguration::Type::Enumeration)};
        const Leaf rule = MakeEnumLeaf("level", {});
        const loglib::LogTable table;
        QVERIFY(!CompileLeaf(rule, 0, columns, &table).has_value());
    }

    /// Populated `filterValues` but `table == nullptr` -> inert. The
    /// enum-set predicate needs the dictionary registry the table
    /// owns; without a table there is nothing to intern against.
    void CompileEnumLeafNullTableAbsent()
    {
        const std::vector<Column> columns{MakeColumn("level", "level", loglib::LogConfiguration::Type::Enumeration)};
        const Leaf rule = MakeEnumLeaf("level", {"Info", "Warn"});
        QVERIFY(!CompileLeaf(rule, 0, columns, /*table=*/nullptr).has_value());
    }

    /// Non-Level enum column with a populated rule + a default-
    /// constructed LogTable -> the `ResolveEnumColumn` lookup
    /// returns a null dictionary but the code still produces a
    /// predicate (the enum predicate transparently falls back to
    /// the string-set path when the dictionary isn't populated).
    /// This pins the "no dict yet, still compile" branch that
    /// distinguishes non-Level enum columns from Level columns.
    void CompileEnumLeafNonLevelWithEmptyTableProduces()
    {
        const std::vector<Column> columns{MakeColumn("level", "level", loglib::LogConfiguration::Type::Enumeration)};
        const Leaf rule = MakeEnumLeaf("level", {"Info", "Warn"});
        const loglib::LogTable table;
        const auto compiled = CompileLeaf(rule, 0, columns, &table);
        QVERIFY(compiled.has_value());
        QVERIFY(std::holds_alternative<loglib::EnumRowPredicate>(*compiled));
    }

    /// Level column with no promoted data yet -> the rank cache is
    /// empty (`LevelRankCache` returns nullptr) so the compile step
    /// bails absent. Rebuilds on the next `Grew` batch, so a mid-
    /// stream config edit doesn't silently over-accept.
    void CompileEnumLeafLevelColumnNullRankCacheAbsent()
    {
        const std::vector<Column> columns{MakeColumn("level", "level", loglib::LogConfiguration::Type::Level)};
        const Leaf rule = MakeEnumLeaf("level", {"Info"});
        const loglib::LogTable table;
        QVERIFY(!CompileLeaf(rule, 0, columns, &table).has_value());
    }

    // ---- CompileLeaf: String ----------------------------------------------

    /// Missing `filterString` -> inert (config half-serialised).
    void CompileStringLeafNoNeedleAbsent()
    {
        const std::vector<Column> columns{MakeColumn("msg", "msg", loglib::LogConfiguration::Type::String)};
        Leaf rule;
        rule.type = Leaf::Type::String;
        rule.columnKeys = {"msg"};
        rule.matchType = Leaf::Match::Contains;
        // `filterString` deliberately left `nullopt`.
        QVERIFY(!CompileLeaf(rule, 0, columns, /*table=*/nullptr).has_value());
    }

    /// Missing `matchType` -> inert (matchType is required so the
    /// exact vs contains vs wildcard vs regex dispatch is
    /// unambiguous).
    void CompileStringLeafNoMatchTypeAbsent()
    {
        const std::vector<Column> columns{MakeColumn("msg", "msg", loglib::LogConfiguration::Type::String)};
        Leaf rule;
        rule.type = Leaf::Type::String;
        rule.columnKeys = {"msg"};
        rule.filterString = "warn";
        // `matchType` deliberately left `nullopt`.
        QVERIFY(!CompileLeaf(rule, 0, columns, /*table=*/nullptr).has_value());
    }

    /// Empty needle + `Contains` -> inert. An empty Contains would
    /// match every row and silently paint / blank the whole view.
    void CompileStringLeafContainsEmptyAbsent()
    {
        const std::vector<Column> columns{MakeColumn("msg", "msg", loglib::LogConfiguration::Type::String)};
        const Leaf rule = MakeStringLeaf("msg", Leaf::Match::Contains, std::string{});
        QVERIFY(!CompileLeaf(rule, 0, columns, /*table=*/nullptr).has_value());
    }

    /// Empty needle + `Wildcard` -> inert (same reason).
    void CompileStringLeafWildcardEmptyAbsent()
    {
        const std::vector<Column> columns{MakeColumn("msg", "msg", loglib::LogConfiguration::Type::String)};
        const Leaf rule = MakeStringLeaf("msg", Leaf::Match::Wildcard, std::string{});
        QVERIFY(!CompileLeaf(rule, 0, columns, /*table=*/nullptr).has_value());
    }

    /// Empty needle + `RegularExpression` -> inert.
    void CompileStringLeafRegexEmptyAbsent()
    {
        const std::vector<Column> columns{MakeColumn("msg", "msg", loglib::LogConfiguration::Type::String)};
        const Leaf rule = MakeStringLeaf("msg", Leaf::Match::RegularExpression, std::string{});
        QVERIFY(!CompileLeaf(rule, 0, columns, /*table=*/nullptr).has_value());
    }

    /// Empty needle + `Exactly` -> genuine predicate (match rows
    /// whose column value is literally empty). The empty-needle
    /// guard deliberately spares this case.
    void CompileStringLeafExactlyEmptyProduces()
    {
        const std::vector<Column> columns{MakeColumn("msg", "msg", loglib::LogConfiguration::Type::String)};
        const Leaf rule = MakeStringLeaf("msg", Leaf::Match::Exactly, std::string{});
        const auto compiled = CompileLeaf(rule, 0, columns, /*table=*/nullptr);
        QVERIFY(compiled.has_value());
    }

    /// Non-empty needle -> the happy path.
    void CompileStringLeafContainsProduces()
    {
        const std::vector<Column> columns{MakeColumn("msg", "msg", loglib::LogConfiguration::Type::String)};
        const Leaf rule = MakeStringLeaf("msg", Leaf::Match::Contains, std::string{"warn"});
        const auto compiled = CompileLeaf(rule, 0, columns, /*table=*/nullptr);
        QVERIFY(compiled.has_value());
        QVERIFY(std::holds_alternative<loglib::CallbackStringRowPredicate>(*compiled));
    }

    // ---- CompileExpression: combinator propagation ------------------------

    /// An input `And{}` with no children stays match-all -- the
    /// identity of `And` -- and compiles to the default empty
    /// `CompiledFilterExpression::And{}`.
    void CompileExprEmptyInputAndIsMatchAll()
    {
        const std::vector<Column> columns{MakeColumn("level", "level", loglib::LogConfiguration::Type::String)};
        const loglib::FilterExpression expr = loglib::MakeAnd({});
        const auto compiled = CompileExpression(expr, columns, /*table=*/nullptr);
        QVERIFY(loglib::IsMatchAllCompiled(compiled));
    }

    /// An input `And` where every child compiles absent must NOT
    /// fold to match-all inside the tree -- the `CompileNode`
    /// returns absent, which propagates up. At the top level the
    /// `CompileExpression` wrapper defaults to match-all in that
    /// case (over-accept, not blank), but nested cases below check
    /// the absence-propagation directly.
    void CompileExprAllAbsentAndAtRootRendersMatchAll()
    {
        const std::vector<Column> columns{MakeColumn("level", "level", loglib::LogConfiguration::Type::String)};
        // Both leaves reference an unknown column; the entire
        // `And` compiles absent.
        std::vector<loglib::FilterExpression> children;
        children.push_back(loglib::MakeLeaf(MakeStringLeaf("missing_a", Leaf::Match::Contains, "x")));
        children.push_back(loglib::MakeLeaf(MakeStringLeaf("missing_b", Leaf::Match::Contains, "y")));
        const loglib::FilterExpression expr = loglib::MakeAnd(std::move(children));
        const auto compiled = CompileExpression(expr, columns, /*table=*/nullptr);
        QVERIFY(loglib::IsMatchAllCompiled(compiled));
    }

    /// Same story for `Or`: all-absent children propagate absent,
    /// top-level fall-through renders match-all.
    void CompileExprAllAbsentOrAtRootRendersMatchAll()
    {
        const std::vector<Column> columns{MakeColumn("level", "level", loglib::LogConfiguration::Type::String)};
        std::vector<loglib::FilterExpression> children;
        children.push_back(loglib::MakeLeaf(MakeStringLeaf("missing_a", Leaf::Match::Contains, "x")));
        children.push_back(loglib::MakeLeaf(MakeStringLeaf("missing_b", Leaf::Match::Contains, "y")));
        const loglib::FilterExpression expr = loglib::MakeOr(std::move(children));
        const auto compiled = CompileExpression(expr, columns, /*table=*/nullptr);
        QVERIFY(loglib::IsMatchAllCompiled(compiled));
    }

    /// `Not` over an absent child stays absent (never folds to
    /// match-all; folding would over-accept inside an `Or` sibling).
    /// At the root, the wrapper renders match-all.
    void CompileExprNotOverAbsentChildAtRootRendersMatchAll()
    {
        const std::vector<Column> columns{MakeColumn("level", "level", loglib::LogConfiguration::Type::String)};
        const loglib::FilterExpression expr =
            loglib::MakeNot(loglib::MakeLeaf(MakeStringLeaf("missing", Leaf::Match::Contains, "x")));
        const auto compiled = CompileExpression(expr, columns, /*table=*/nullptr);
        QVERIFY(loglib::IsMatchAllCompiled(compiled));
    }

    /// A bare `Not{child == nullptr}` (hand-edited config; the
    /// `MakeNot` factory always seeds `child`) is deliberately
    /// different from `Not(absent)`: it compiles to match-all so
    /// the compile path lines up with the visit path's own null-
    /// child handling in `EvaluateExpression`.
    void CompileExprNotNullChildIsMatchAll()
    {
        const std::vector<Column> columns{MakeColumn("level", "level", loglib::LogConfiguration::Type::String)};
        loglib::FilterExpression expr;
        expr.node = loglib::FilterExpression::Not{};
        const auto compiled = CompileExpression(expr, columns, /*table=*/nullptr);
        QVERIFY(loglib::IsMatchAllCompiled(compiled));
    }

    /// Sibling `And` next to a `Not(all-absent-And)` keeps the
    /// sibling; the absent `Not` gets dropped. Distinguishes the
    /// "propagate absence" fix from the naive "fold to match-none"
    /// bug that would blank the view.
    void CompileExprAndDropsAbsentChild()
    {
        const std::vector<Column> columns{MakeColumn("svc", "svc", loglib::LogConfiguration::Type::String)};
        std::vector<loglib::FilterExpression> children;
        children.push_back(loglib::MakeLeaf(MakeStringLeaf("svc", Leaf::Match::Contains, "auth")));
        children.push_back(loglib::MakeLeaf(MakeStringLeaf("missing", Leaf::Match::Contains, "x")));
        const loglib::FilterExpression expr = loglib::MakeAnd(std::move(children));
        const auto compiled = CompileExpression(expr, columns, /*table=*/nullptr);
        const auto *andNode = AsCompiledAnd(compiled);
        QVERIFY(andNode != nullptr);
        // The `missing` leaf dropped; the `svc` leaf survived.
        QCOMPARE(andNode->children.size(), std::size_t{1});
        QVERIFY(AsCompiledLeaf(andNode->children.front()) != nullptr);
    }

    /// Same story for `Or` -- absent children drop; the surviving
    /// child stays.
    void CompileExprOrDropsAbsentChild()
    {
        const std::vector<Column> columns{MakeColumn("svc", "svc", loglib::LogConfiguration::Type::String)};
        std::vector<loglib::FilterExpression> children;
        children.push_back(loglib::MakeLeaf(MakeStringLeaf("svc", Leaf::Match::Contains, "auth")));
        children.push_back(loglib::MakeLeaf(MakeStringLeaf("missing", Leaf::Match::Contains, "x")));
        const loglib::FilterExpression expr = loglib::MakeOr(std::move(children));
        const auto compiled = CompileExpression(expr, columns, /*table=*/nullptr);
        const auto *orNode = AsCompiledOr(compiled);
        QVERIFY(orNode != nullptr);
        QCOMPARE(orNode->children.size(), std::size_t{1});
        QVERIFY(AsCompiledLeaf(orNode->children.front()) != nullptr);
    }

    /// Nested combinator: `And(svcLeaf, Or(missing_a, missing_b))`.
    /// The inner `Or` compiles absent (both children unresolved)
    /// and drops out of the outer `And`, leaving a single-child
    /// `And`.
    void CompileExprNestedAllAbsentOrDropsFromAnd()
    {
        const std::vector<Column> columns{MakeColumn("svc", "svc", loglib::LogConfiguration::Type::String)};
        std::vector<loglib::FilterExpression> orChildren;
        orChildren.push_back(loglib::MakeLeaf(MakeStringLeaf("missing_a", Leaf::Match::Contains, "x")));
        orChildren.push_back(loglib::MakeLeaf(MakeStringLeaf("missing_b", Leaf::Match::Contains, "y")));

        std::vector<loglib::FilterExpression> topChildren;
        topChildren.push_back(loglib::MakeLeaf(MakeStringLeaf("svc", Leaf::Match::Contains, "auth")));
        topChildren.push_back(loglib::MakeOr(std::move(orChildren)));

        const loglib::FilterExpression expr = loglib::MakeAnd(std::move(topChildren));
        const auto compiled = CompileExpression(expr, columns, /*table=*/nullptr);
        const auto *andNode = AsCompiledAnd(compiled);
        QVERIFY(andNode != nullptr);
        QCOMPARE(andNode->children.size(), std::size_t{1});
    }

    /// `CompileExpression` builds a sorted, deduped list of
    /// referenced columns for the model-side change filter. Two
    /// leaves against the same column collapse to one entry.
    void CompileExprReferencedColumnsAreDeduped()
    {
        const std::vector<Column> columns{MakeColumn("svc", "svc", loglib::LogConfiguration::Type::String)};
        std::vector<loglib::FilterExpression> children;
        children.push_back(loglib::MakeLeaf(MakeStringLeaf("svc", Leaf::Match::Contains, "auth")));
        children.push_back(loglib::MakeLeaf(MakeStringLeaf("svc", Leaf::Match::Contains, "backend")));
        const loglib::FilterExpression expr = loglib::MakeAnd(std::move(children));
        const auto compiled = CompileExpression(expr, columns, /*table=*/nullptr);
        QCOMPARE(compiled.referencedColumns.size(), std::size_t{1});
        QCOMPARE(compiled.referencedColumns.front(), std::size_t{0});
    }

    /// Two leaves against distinct columns produce two entries,
    /// sorted ascending.
    void CompileExprReferencedColumnsAreSortedAndDistinct()
    {
        const std::vector<Column> columns{
            MakeColumn("svc", "svc", loglib::LogConfiguration::Type::String),
            MakeColumn("level", "level", loglib::LogConfiguration::Type::String),
        };
        std::vector<loglib::FilterExpression> children;
        // Deliberately reverse the "natural" order so a bug that
        // used input-order would fail this test.
        children.push_back(loglib::MakeLeaf(MakeStringLeaf("level", Leaf::Match::Contains, "warn")));
        children.push_back(loglib::MakeLeaf(MakeStringLeaf("svc", Leaf::Match::Contains, "auth")));
        const loglib::FilterExpression expr = loglib::MakeAnd(std::move(children));
        const auto compiled = CompileExpression(expr, columns, /*table=*/nullptr);
        QCOMPARE(compiled.referencedColumns.size(), std::size_t{2});
        QCOMPARE(compiled.referencedColumns[0], std::size_t{0});
        QCOMPARE(compiled.referencedColumns[1], std::size_t{1});
    }

    /// Top-level `Leaf` that resolves to a concrete predicate
    /// survives compile; the compiled tree is a `Leaf` node, not
    /// wrapped in `And{Leaf}`.
    void CompileExprSingleLeafStaysLeaf()
    {
        const std::vector<Column> columns{MakeColumn("svc", "svc", loglib::LogConfiguration::Type::String)};
        const loglib::FilterExpression expr = loglib::MakeLeaf(MakeStringLeaf("svc", Leaf::Match::Contains, "auth"));
        const auto compiled = CompileExpression(expr, columns, /*table=*/nullptr);
        QVERIFY(AsCompiledLeaf(compiled) != nullptr);
    }

    /// `Not` over a resolvable leaf produces a `Not` compiled node
    /// wrapping the leaf's compiled predicate. Pins the happy path
    /// alongside the absent / null-child variants above.
    void CompileExprNotOverResolvableLeafProducesNot()
    {
        const std::vector<Column> columns{MakeColumn("svc", "svc", loglib::LogConfiguration::Type::String)};
        const loglib::FilterExpression expr =
            loglib::MakeNot(loglib::MakeLeaf(MakeStringLeaf("svc", Leaf::Match::Contains, "auth")));
        const auto compiled = CompileExpression(expr, columns, /*table=*/nullptr);
        const auto *notNode = std::get_if<loglib::CompiledFilterExpression::Not>(&compiled.node);
        QVERIFY(notNode != nullptr);
        QVERIFY(notNode->child != nullptr);
        QVERIFY(AsCompiledLeaf(*notNode->child) != nullptr);
    }
};

QTEST_MAIN(LeafRuleCompileTest)

#include "test_leaf_rule_compile.moc"
