// Port of the leaf-rule compile QTest. Covers absent leaves and combinator propagation.
#include <loglib/filter_expression.hpp>
#include <loglib/leaf_rule_compile.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_filter.hpp>
#include <loglib/log_table.hpp>

#include <catch2/catch_all.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace
{

using Column = loglib::Column;
using Leaf = loglib::LeafRule;

/// One-column fixture bound to @p key.
[[nodiscard]] Column MakeColumn(std::string header, std::string key, loglib::ColumnType type)
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

TEST_CASE("ResolveEmptyKeysReturnsMinusOne", "[leaf_rule_compile]")
{
    const std::vector<Column> columns{MakeColumn("level", "level", loglib::ColumnType::String)};
    REQUIRE(loglib::ResolveLeafColumnByKeys({}, columns) == -1);
}

TEST_CASE("ResolveKeysNoMatchReturnsMinusOne", "[leaf_rule_compile]")
{
    const std::vector<Column> columns{MakeColumn("level", "level", loglib::ColumnType::String)};
    REQUIRE(loglib::ResolveLeafColumnByKeys({"missing"}, columns) == -1);
}

TEST_CASE("ResolveKeysSubsetMatchesFirstColumn", "[leaf_rule_compile]")
{
    Column withAlias;
    withAlias.header = "level";
    withAlias.keys = {"level", "log.level"};
    withAlias.type = loglib::ColumnType::String;
    const std::vector<Column> columns{withAlias};
    REQUIRE(loglib::ResolveLeafColumnByKeys({"level"}, columns) == 0);
}

TEST_CASE("ResolveKeysReturnsFirstMatchIndex", "[leaf_rule_compile]")
{
    const std::vector<Column> columns{
        MakeColumn("service", "service", loglib::ColumnType::String),
        MakeColumn("level", "level", loglib::ColumnType::String),
    };
    REQUIRE(loglib::ResolveLeafColumnByKeys({"level"}, columns) == 1);
}

TEST_CASE("CompileLeafUnresolvedColumnAbsent", "[leaf_rule_compile]")
{
    const std::vector<Column> columns{MakeColumn("level", "level", loglib::ColumnType::String)};
    const Leaf rule = MakeStringLeaf("level", Leaf::Match::Contains, "warn");
    REQUIRE_FALSE(loglib::CompileLeaf(rule, /*resolvedColumn=*/-1, columns, /*table=*/nullptr).has_value());
}

TEST_CASE("CompileTimeLeafNoBoundsAbsent", "[leaf_rule_compile]")
{
    const std::vector<Column> columns{MakeColumn("ts", "ts", loglib::ColumnType::Time)};
    const Leaf rule = MakeTimeLeaf("ts", std::nullopt, std::nullopt);
    REQUIRE_FALSE(loglib::CompileLeaf(rule, 0, columns, /*table=*/nullptr).has_value());
}

TEST_CASE("CompileTimeLeafLowerOnlyProduces", "[leaf_rule_compile]")
{
    const std::vector<Column> columns{MakeColumn("ts", "ts", loglib::ColumnType::Time)};
    const Leaf rule = MakeTimeLeaf("ts", std::int64_t{0}, std::nullopt);
    const auto compiled = loglib::CompileLeaf(rule, 0, columns, /*table=*/nullptr);
    REQUIRE(compiled.has_value());
    REQUIRE(std::holds_alternative<loglib::TimeRangeRowPredicate>(*compiled));
}

TEST_CASE("CompileTimeLeafUpperOnlyProduces", "[leaf_rule_compile]")
{
    const std::vector<Column> columns{MakeColumn("ts", "ts", loglib::ColumnType::Time)};
    const Leaf rule = MakeTimeLeaf("ts", std::nullopt, std::int64_t{1000});
    const auto compiled = loglib::CompileLeaf(rule, 0, columns, /*table=*/nullptr);
    REQUIRE(compiled.has_value());
}

TEST_CASE("CompileTimeLeafBothBoundsProduces", "[leaf_rule_compile]")
{
    const std::vector<Column> columns{MakeColumn("ts", "ts", loglib::ColumnType::Time)};
    const Leaf rule = MakeTimeLeaf("ts", std::int64_t{0}, std::int64_t{1000});
    const auto compiled = loglib::CompileLeaf(rule, 0, columns, /*table=*/nullptr);
    REQUIRE(compiled.has_value());
}

TEST_CASE("CompileNumberLeafNoBoundsAbsent", "[leaf_rule_compile]")
{
    const std::vector<Column> columns{MakeColumn("latency", "latency", loglib::ColumnType::Number)};
    const Leaf rule = MakeNumberLeaf("latency", std::nullopt, std::nullopt);
    REQUIRE_FALSE(loglib::CompileLeaf(rule, 0, columns, /*table=*/nullptr).has_value());
}

TEST_CASE("CompileNumberLeafLowerOnlyProduces", "[leaf_rule_compile]")
{
    const std::vector<Column> columns{MakeColumn("latency", "latency", loglib::ColumnType::Number)};
    const Leaf rule = MakeNumberLeaf("latency", 100.0, std::nullopt);
    const auto compiled = loglib::CompileLeaf(rule, 0, columns, /*table=*/nullptr);
    REQUIRE(compiled.has_value());
    REQUIRE(std::holds_alternative<loglib::NumericRangeRowPredicate>(*compiled));
}

TEST_CASE("CompileNumberLeafUpperOnlyProduces", "[leaf_rule_compile]")
{
    const std::vector<Column> columns{MakeColumn("latency", "latency", loglib::ColumnType::Number)};
    const Leaf rule = MakeNumberLeaf("latency", std::nullopt, 100.0);
    const auto compiled = loglib::CompileLeaf(rule, 0, columns, /*table=*/nullptr);
    REQUIRE(compiled.has_value());
}

TEST_CASE("CompileBooleanLeafEmptyValuesAbsent", "[leaf_rule_compile]")
{
    const std::vector<Column> columns{MakeColumn("ok", "ok", loglib::ColumnType::Boolean)};
    const Leaf rule = MakeBooleanLeaf("ok", {});
    REQUIRE_FALSE(loglib::CompileLeaf(rule, 0, columns, /*table=*/nullptr).has_value());
}

TEST_CASE("CompileBooleanLeafOnlyGarbageValuesAbsent", "[leaf_rule_compile]")
{
    const std::vector<Column> columns{MakeColumn("ok", "ok", loglib::ColumnType::Boolean)};
    const Leaf rule = MakeBooleanLeaf("ok", {"maybe", "yes", "no"});
    REQUIRE_FALSE(loglib::CompileLeaf(rule, 0, columns, /*table=*/nullptr).has_value());
}

TEST_CASE("CompileBooleanLeafTrueOnlyProduces", "[leaf_rule_compile]")
{
    const std::vector<Column> columns{MakeColumn("ok", "ok", loglib::ColumnType::Boolean)};
    const Leaf rule = MakeBooleanLeaf("ok", {"true"});
    const auto compiled = loglib::CompileLeaf(rule, 0, columns, /*table=*/nullptr);
    REQUIRE(compiled.has_value());
    REQUIRE(std::holds_alternative<loglib::BoolRowPredicate>(*compiled));
}

TEST_CASE("CompileBooleanLeafCaseInsensitive", "[leaf_rule_compile]")
{
    const std::vector<Column> columns{MakeColumn("ok", "ok", loglib::ColumnType::Boolean)};
    const Leaf rule = MakeBooleanLeaf("ok", {"True", "FALSE"});
    REQUIRE(loglib::CompileLeaf(rule, 0, columns, /*table=*/nullptr).has_value());
}

TEST_CASE("CompileEnumLeafEmptyValuesAbsent", "[leaf_rule_compile]")
{
    const std::vector<Column> columns{MakeColumn("level", "level", loglib::ColumnType::Enumeration)};
    const Leaf rule = MakeEnumLeaf("level", {});
    const loglib::LogTable table;
    REQUIRE_FALSE(loglib::CompileLeaf(rule, 0, columns, &table).has_value());
}

TEST_CASE("CompileEnumLeafNullTableAbsent", "[leaf_rule_compile]")
{
    const std::vector<Column> columns{MakeColumn("level", "level", loglib::ColumnType::Enumeration)};
    const Leaf rule = MakeEnumLeaf("level", {"Info", "Warn"});
    REQUIRE_FALSE(loglib::CompileLeaf(rule, 0, columns, /*table=*/nullptr).has_value());
}

TEST_CASE("CompileEnumLeafNonLevelWithEmptyTableProduces", "[leaf_rule_compile]")
{
    const std::vector<Column> columns{MakeColumn("level", "level", loglib::ColumnType::Enumeration)};
    const Leaf rule = MakeEnumLeaf("level", {"Info", "Warn"});
    const loglib::LogTable table;
    const auto compiled = loglib::CompileLeaf(rule, 0, columns, &table);
    REQUIRE(compiled.has_value());
    REQUIRE(std::holds_alternative<loglib::EnumRowPredicate>(*compiled));
}

TEST_CASE("CompileEnumLeafLevelColumnNullRankCacheAbsent", "[leaf_rule_compile]")
{
    const std::vector<Column> columns{MakeColumn("level", "level", loglib::ColumnType::Level)};
    const Leaf rule = MakeEnumLeaf("level", {"Info"});
    const loglib::LogTable table;
    REQUIRE_FALSE(loglib::CompileLeaf(rule, 0, columns, &table).has_value());
}

TEST_CASE("CompileStringLeafNoNeedleAbsent", "[leaf_rule_compile]")
{
    const std::vector<Column> columns{MakeColumn("msg", "msg", loglib::ColumnType::String)};
    Leaf rule;
    rule.type = Leaf::Type::String;
    rule.columnKeys = {"msg"};
    rule.matchType = Leaf::Match::Contains;
    // `filterString` deliberately left `nullopt`.
    REQUIRE_FALSE(loglib::CompileLeaf(rule, 0, columns, /*table=*/nullptr).has_value());
}

TEST_CASE("CompileStringLeafNoMatchTypeAbsent", "[leaf_rule_compile]")
{
    const std::vector<Column> columns{MakeColumn("msg", "msg", loglib::ColumnType::String)};
    Leaf rule;
    rule.type = Leaf::Type::String;
    rule.columnKeys = {"msg"};
    rule.filterString = "warn";
    // `matchType` deliberately left `nullopt`.
    REQUIRE_FALSE(loglib::CompileLeaf(rule, 0, columns, /*table=*/nullptr).has_value());
}

TEST_CASE("CompileStringLeafContainsEmptyAbsent", "[leaf_rule_compile]")
{
    const std::vector<Column> columns{MakeColumn("msg", "msg", loglib::ColumnType::String)};
    const Leaf rule = MakeStringLeaf("msg", Leaf::Match::Contains, std::string{});
    REQUIRE_FALSE(loglib::CompileLeaf(rule, 0, columns, /*table=*/nullptr).has_value());
}

TEST_CASE("CompileStringLeafWildcardEmptyAbsent", "[leaf_rule_compile]")
{
    const std::vector<Column> columns{MakeColumn("msg", "msg", loglib::ColumnType::String)};
    const Leaf rule = MakeStringLeaf("msg", Leaf::Match::Wildcard, std::string{});
    REQUIRE_FALSE(loglib::CompileLeaf(rule, 0, columns, /*table=*/nullptr).has_value());
}

TEST_CASE("CompileStringLeafRegexEmptyAbsent", "[leaf_rule_compile]")
{
    const std::vector<Column> columns{MakeColumn("msg", "msg", loglib::ColumnType::String)};
    const Leaf rule = MakeStringLeaf("msg", Leaf::Match::RegularExpression, std::string{});
    REQUIRE_FALSE(loglib::CompileLeaf(rule, 0, columns, /*table=*/nullptr).has_value());
}

TEST_CASE("CompileStringLeafExactlyEmptyProduces", "[leaf_rule_compile]")
{
    const std::vector<Column> columns{MakeColumn("msg", "msg", loglib::ColumnType::String)};
    const Leaf rule = MakeStringLeaf("msg", Leaf::Match::Exactly, std::string{});
    const auto compiled = loglib::CompileLeaf(rule, 0, columns, /*table=*/nullptr);
    REQUIRE(compiled.has_value());
}

TEST_CASE("CompileStringLeafContainsProduces", "[leaf_rule_compile]")
{
    const std::vector<Column> columns{MakeColumn("msg", "msg", loglib::ColumnType::String)};
    const Leaf rule = MakeStringLeaf("msg", Leaf::Match::Contains, std::string{"warn"});
    const auto compiled = loglib::CompileLeaf(rule, 0, columns, /*table=*/nullptr);
    REQUIRE(compiled.has_value());
    REQUIRE(std::holds_alternative<loglib::CallbackStringRowPredicate>(*compiled));
}

TEST_CASE("CompileExprEmptyInputAndIsMatchAll", "[leaf_rule_compile]")
{
    const std::vector<Column> columns{MakeColumn("level", "level", loglib::ColumnType::String)};
    const loglib::FilterExpression expr = loglib::MakeAnd({});
    const auto compiled = loglib::CompileExpression(expr, columns, /*table=*/nullptr);
    REQUIRE(loglib::IsMatchAllCompiled(compiled));
}

TEST_CASE("CompileExprAllAbsentAndAtRootRendersMatchAll", "[leaf_rule_compile]")
{
    const std::vector<Column> columns{MakeColumn("level", "level", loglib::ColumnType::String)};
    // Both leaves reference an unknown column; the entire
    // `And` compiles absent.
    std::vector<loglib::FilterExpression> children;
    children.push_back(loglib::MakeLeaf(MakeStringLeaf("missing_a", Leaf::Match::Contains, "x")));
    children.push_back(loglib::MakeLeaf(MakeStringLeaf("missing_b", Leaf::Match::Contains, "y")));
    const loglib::FilterExpression expr = loglib::MakeAnd(std::move(children));
    const auto compiled = loglib::CompileExpression(expr, columns, /*table=*/nullptr);
    REQUIRE(loglib::IsMatchAllCompiled(compiled));
}

TEST_CASE("CompileExprAllAbsentOrAtRootRendersMatchAll", "[leaf_rule_compile]")
{
    const std::vector<Column> columns{MakeColumn("level", "level", loglib::ColumnType::String)};
    std::vector<loglib::FilterExpression> children;
    children.push_back(loglib::MakeLeaf(MakeStringLeaf("missing_a", Leaf::Match::Contains, "x")));
    children.push_back(loglib::MakeLeaf(MakeStringLeaf("missing_b", Leaf::Match::Contains, "y")));
    const loglib::FilterExpression expr = loglib::MakeOr(std::move(children));
    const auto compiled = loglib::CompileExpression(expr, columns, /*table=*/nullptr);
    REQUIRE(loglib::IsMatchAllCompiled(compiled));
}

TEST_CASE("CompileExprNotOverAbsentChildAtRootRendersMatchAll", "[leaf_rule_compile]")
{
    const std::vector<Column> columns{MakeColumn("level", "level", loglib::ColumnType::String)};
    const loglib::FilterExpression expr =
        loglib::MakeNot(loglib::MakeLeaf(MakeStringLeaf("missing", Leaf::Match::Contains, "x")));
    const auto compiled = loglib::CompileExpression(expr, columns, /*table=*/nullptr);
    REQUIRE(loglib::IsMatchAllCompiled(compiled));
}

TEST_CASE("CompileExprNotNullChildIsMatchAll", "[leaf_rule_compile]")
{
    const std::vector<Column> columns{MakeColumn("level", "level", loglib::ColumnType::String)};
    loglib::FilterExpression expr;
    expr.node = loglib::FilterExpression::Not{};
    const auto compiled = loglib::CompileExpression(expr, columns, /*table=*/nullptr);
    REQUIRE(loglib::IsMatchAllCompiled(compiled));
}

TEST_CASE("CompileExprAndDropsAbsentChild", "[leaf_rule_compile]")
{
    const std::vector<Column> columns{MakeColumn("svc", "svc", loglib::ColumnType::String)};
    std::vector<loglib::FilterExpression> children;
    children.push_back(loglib::MakeLeaf(MakeStringLeaf("svc", Leaf::Match::Contains, "auth")));
    children.push_back(loglib::MakeLeaf(MakeStringLeaf("missing", Leaf::Match::Contains, "x")));
    const loglib::FilterExpression expr = loglib::MakeAnd(std::move(children));
    const auto compiled = loglib::CompileExpression(expr, columns, /*table=*/nullptr);
    const auto *andNode = AsCompiledAnd(compiled);
    REQUIRE(andNode != nullptr);
    // The `missing` leaf dropped; the `svc` leaf survived.
    REQUIRE(andNode->children.size() == std::size_t{1});
    REQUIRE(AsCompiledLeaf(andNode->children.front()) != nullptr);
}

TEST_CASE("CompileExprOrDropsAbsentChild", "[leaf_rule_compile]")
{
    const std::vector<Column> columns{MakeColumn("svc", "svc", loglib::ColumnType::String)};
    std::vector<loglib::FilterExpression> children;
    children.push_back(loglib::MakeLeaf(MakeStringLeaf("svc", Leaf::Match::Contains, "auth")));
    children.push_back(loglib::MakeLeaf(MakeStringLeaf("missing", Leaf::Match::Contains, "x")));
    const loglib::FilterExpression expr = loglib::MakeOr(std::move(children));
    const auto compiled = loglib::CompileExpression(expr, columns, /*table=*/nullptr);
    const auto *orNode = AsCompiledOr(compiled);
    REQUIRE(orNode != nullptr);
    REQUIRE(orNode->children.size() == std::size_t{1});
    REQUIRE(AsCompiledLeaf(orNode->children.front()) != nullptr);
}

TEST_CASE("CompileExprNestedAllAbsentOrDropsFromAnd", "[leaf_rule_compile]")
{
    const std::vector<Column> columns{MakeColumn("svc", "svc", loglib::ColumnType::String)};
    std::vector<loglib::FilterExpression> orChildren;
    orChildren.push_back(loglib::MakeLeaf(MakeStringLeaf("missing_a", Leaf::Match::Contains, "x")));
    orChildren.push_back(loglib::MakeLeaf(MakeStringLeaf("missing_b", Leaf::Match::Contains, "y")));

    std::vector<loglib::FilterExpression> topChildren;
    topChildren.push_back(loglib::MakeLeaf(MakeStringLeaf("svc", Leaf::Match::Contains, "auth")));
    topChildren.push_back(loglib::MakeOr(std::move(orChildren)));

    const loglib::FilterExpression expr = loglib::MakeAnd(std::move(topChildren));
    const auto compiled = loglib::CompileExpression(expr, columns, /*table=*/nullptr);
    const auto *andNode = AsCompiledAnd(compiled);
    REQUIRE(andNode != nullptr);
    REQUIRE(andNode->children.size() == std::size_t{1});
}

TEST_CASE("CompileExprReferencedColumnsAreDeduped", "[leaf_rule_compile]")
{
    const std::vector<Column> columns{MakeColumn("svc", "svc", loglib::ColumnType::String)};
    std::vector<loglib::FilterExpression> children;
    children.push_back(loglib::MakeLeaf(MakeStringLeaf("svc", Leaf::Match::Contains, "auth")));
    children.push_back(loglib::MakeLeaf(MakeStringLeaf("svc", Leaf::Match::Contains, "backend")));
    const loglib::FilterExpression expr = loglib::MakeAnd(std::move(children));
    const auto compiled = loglib::CompileExpression(expr, columns, /*table=*/nullptr);
    REQUIRE(compiled.referencedColumns.size() == std::size_t{1});
    REQUIRE(compiled.referencedColumns.front() == std::size_t{0});
}

TEST_CASE("CompileExprReferencedColumnsAreSortedAndDistinct", "[leaf_rule_compile]")
{
    const std::vector<Column> columns{
        MakeColumn("svc", "svc", loglib::ColumnType::String),
        MakeColumn("level", "level", loglib::ColumnType::String),
    };
    std::vector<loglib::FilterExpression> children;
    // Deliberately reverse the "natural" order so a bug that
    // used input-order would fail this test.
    children.push_back(loglib::MakeLeaf(MakeStringLeaf("level", Leaf::Match::Contains, "warn")));
    children.push_back(loglib::MakeLeaf(MakeStringLeaf("svc", Leaf::Match::Contains, "auth")));
    const loglib::FilterExpression expr = loglib::MakeAnd(std::move(children));
    const auto compiled = loglib::CompileExpression(expr, columns, /*table=*/nullptr);
    REQUIRE(compiled.referencedColumns.size() == std::size_t{2});
    REQUIRE(compiled.referencedColumns[0] == std::size_t{0});
    REQUIRE(compiled.referencedColumns[1] == std::size_t{1});
}

TEST_CASE("CompileExprSingleLeafStaysLeaf", "[leaf_rule_compile]")
{
    const std::vector<Column> columns{MakeColumn("svc", "svc", loglib::ColumnType::String)};
    const loglib::FilterExpression expr = loglib::MakeLeaf(MakeStringLeaf("svc", Leaf::Match::Contains, "auth"));
    const auto compiled = loglib::CompileExpression(expr, columns, /*table=*/nullptr);
    REQUIRE(AsCompiledLeaf(compiled) != nullptr);
}

TEST_CASE("CompileExprNotOverResolvableLeafProducesNot", "[leaf_rule_compile]")
{
    const std::vector<Column> columns{MakeColumn("svc", "svc", loglib::ColumnType::String)};
    const loglib::FilterExpression expr =
        loglib::MakeNot(loglib::MakeLeaf(MakeStringLeaf("svc", Leaf::Match::Contains, "auth")));
    const auto compiled = loglib::CompileExpression(expr, columns, /*table=*/nullptr);
    const auto *notNode = std::get_if<loglib::CompiledFilterExpression::Not>(&compiled.node);
    REQUIRE(notNode != nullptr);
    REQUIRE(notNode->child != nullptr);
    REQUIRE(AsCompiledLeaf(*notNode->child) != nullptr);
}
