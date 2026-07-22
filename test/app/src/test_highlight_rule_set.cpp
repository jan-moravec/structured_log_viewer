// Focused tests for `HighlightRuleSet` (ROADMAP item 3). Its own
// binary so `ctest -R apptest_highlight_rules` runs just this
// concern.
//
// Covered: baseline empty set, column-key resolution +
// active/inactive counts, last-match-wins, tail append via
// `OnRowsAppended`, FIFO eviction shift via `OnRowsEvicted`,
// `RebindColumns` after schema growth, `ClearMatches`, key-based
// identity survives `MoveColumn`, Boolean / Number predicates,
// and empty-needle rejection.

#include "highlight_rule_set.hpp"
#include "log_model.hpp"
#include "qt_streaming_log_sink.hpp"

#include <loglib/file_line_source.hpp>
#include <loglib/internal/advanced_parser_options.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_file.hpp>
#include <loglib/log_parse_sink.hpp>
#include <loglib/log_table.hpp>
#include <loglib/parser_options.hpp>
#include <loglib/parsers/json_parser.hpp>
#include <loglib/stop_token.hpp>

#include <QSignalSpy>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <cstddef>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using Rule = loglib::LogConfiguration::HighlightRule;

namespace
{

/// JSONL fixture: `{"level": ..., "msg": "row N"}`, @p levels
/// cycled row-by-row so tests can target a specific subset by
/// picking a level.
class LevelFixture
{
public:
    LevelFixture(int rows, const QStringList &levels)
    {
        QVERIFY2(mDir.isValid(), "QTemporaryDir creation must succeed");
        mPath = mDir.filePath("highlight_rules.jsonl");
        std::ofstream stream(mPath.toStdString(), std::ios::binary);
        QVERIFY2(stream.is_open(), "fixture must open for writing");
        for (int i = 0; i < rows; ++i)
        {
            const QString line =
                QStringLiteral(R"({"level": "%1", "msg": "row %2"})").arg(levels[i % levels.size()]).arg(i);
            stream << line.toStdString() << '\n';
        }
    }

    [[nodiscard]] QString Path() const
    {
        return mPath;
    }

private:
    QTemporaryDir mDir;
    QString mPath;
};

/// Stream one JSON path into @p model synchronously; blocks on
/// `streamingFinished`. Mirrors `test_histogram_dock.cpp`.
void StreamJsonPathInto(LogModel &model, const QString &path)
{
    QSignalSpy finishedSpy(&model, &LogModel::streamingFinished);
    QVERIFY(finishedSpy.isValid());

    auto file = std::make_unique<loglib::LogFile>(path.toStdString());
    auto fileSource = std::make_unique<loglib::FileLineSource>(std::move(file));
    loglib::FileLineSource *parseSource = fileSource.get();
    const loglib::StopToken stopToken = model.BeginStreamingForSyncTest(std::move(fileSource));

    loglib::ParserOptions options;
    options.stopToken = stopToken;
    loglib::internal::AdvancedParserOptions advanced;
    advanced.threads = 1;
    loglib::JsonParser::ParseStreaming(*parseSource, *model.Sink(), options, advanced);

    const bool finished = finishedSpy.count() > 0 || finishedSpy.wait(5000);
    QVERIFY2(finished, "streamingFinished must arrive within the timeout");
    model.EndStreaming(false);
}

/// Build a String / Contains rule bound to @p key.
[[nodiscard]] Rule MakeContainsRule(const QString &name, const std::string &key, const std::string &needle)
{
    Rule r;
    r.name = name.toStdString();
    r.enabled = true;
    r.columnKeys = {key};
    r.type = Rule::Type::String;
    r.matchType = Rule::Match::Contains;
    r.filterString = needle;
    return r;
}

} // namespace

class HighlightRuleSetTest : public QObject
{
    Q_OBJECT

private slots:
    /// Baseline: empty set reports `Empty()`, `!HasActiveRules()`,
    /// and `nullopt` for every row.
    void EmptyRuleSetHasNoMatches()
    {
        HighlightRuleSet rules;
        LogModel model{/*parent=*/nullptr, /*theme=*/nullptr, /*anchors=*/nullptr, &rules};
        const LevelFixture fixture(5, {"info", "warn"});
        StreamJsonPathInto(model, fixture.Path());

        QVERIFY(rules.Empty());
        QVERIFY(!rules.HasActiveRules());
        for (std::size_t row = 0; row < 5; ++row)
        {
            QCOMPARE(rules.LastMatchFor(row), std::optional<std::size_t>{});
        }
    }

    /// Unresolvable keys land in `InactiveCount`; resolvable ones
    /// activate and match every row containing the needle.
    void ResolveByKeys()
    {
        HighlightRuleSet rules;
        LogModel model{/*parent=*/nullptr, /*theme=*/nullptr, /*anchors=*/nullptr, &rules};
        const LevelFixture fixture(6, {"info", "warn"});
        StreamJsonPathInto(model, fixture.Path());

        std::vector<Rule> ruleSet;
        ruleSet.push_back(MakeContainsRule("warn-hit", "level", "warn"));
        ruleSet.push_back(MakeContainsRule("bogus", "does-not-exist", "warn"));
        rules.SetRules(std::move(ruleSet), model.Configuration().columns, &model.Table());

        QCOMPARE(rules.InactiveCount(), 1u);
        QVERIFY(rules.HasActiveRules());
        const auto &resolved = rules.ResolvedColumnsForTest();
        QCOMPARE(resolved.size(), 2u);
        QVERIFY(resolved[0] >= 0);
        QCOMPARE(resolved[1], -1);

        // Fixture alternates info/warn starting at index 0, so the
        // odd rows match `warn-hit`.
        for (std::size_t row = 0; row < 6; ++row)
        {
            const auto match = rules.LastMatchFor(row);
            if (row % 2 == 1)
            {
                QCOMPARE(match, std::optional<std::size_t>{0u});
            }
            else
            {
                QCOMPARE(match, std::optional<std::size_t>{});
            }
        }
    }

    /// With two overlapping rules the LAST accepting rule per row
    /// wins. Catch-all on `n` covers info/warn; warn-only overlays
    /// warn rows and should surface as rule #1, not rule #0.
    void LastMatchWins()
    {
        HighlightRuleSet rules;
        LogModel model{/*parent=*/nullptr, /*theme=*/nullptr, /*anchors=*/nullptr, &rules};
        const LevelFixture fixture(4, {"info", "warn"});
        StreamJsonPathInto(model, fixture.Path());

        std::vector<Rule> ruleSet;
        ruleSet.push_back(MakeContainsRule("catch-all", "level", "n"));
        ruleSet.push_back(MakeContainsRule("warn-only", "level", "warn"));
        rules.SetRules(std::move(ruleSet), model.Configuration().columns, &model.Table());

        // "info" contains 'n' -> catch-all (index 0).
        // "warn" contains 'n' AND "warn" -> warn-only wins (index 1).
        QCOMPARE(rules.LastMatchFor(0), std::optional<std::size_t>{0u});
        QCOMPARE(rules.LastMatchFor(1), std::optional<std::size_t>{1u});
        QCOMPARE(rules.LastMatchFor(2), std::optional<std::size_t>{0u});
        QCOMPARE(rules.LastMatchFor(3), std::optional<std::size_t>{1u});
    }

    /// A rule installed early activates on rows appended later.
    /// `MainWindow` calls `OnRowsAppended` per batch; the test
    /// invokes it directly with the full inserted range.
    void TailUpdateAfterAppend()
    {
        HighlightRuleSet rules;
        LogModel model{/*parent=*/nullptr, /*theme=*/nullptr, /*anchors=*/nullptr, &rules};

        // Seed batch.
        const LevelFixture seed(2, {"info", "warn"});
        StreamJsonPathInto(model, seed.Path());
        std::vector<Rule> ruleSet;
        ruleSet.push_back(MakeContainsRule("warn-hit", "level", "warn"));
        rules.SetRules(std::move(ruleSet), model.Configuration().columns, &model.Table());
        QCOMPARE(rules.LastMatchFor(1), std::optional<std::size_t>{0u});

        // Second batch. The `MainWindow` tail-update hook isn't
        // wired in this fixture; invoke `OnRowsAppended` directly.
        const std::size_t priorRowCount = model.Table().RowCount();
        const LevelFixture followup(4, {"info", "warn"});
        StreamJsonPathInto(model, followup.Path());
        const std::size_t newRowCount = model.Table().RowCount();
        QVERIFY(newRowCount > priorRowCount);
        rules.OnRowsAppended(model.Table(), priorRowCount, newRowCount - 1);

        // Seed rows 0/1 keep their earlier match state; only the
        // new rows update.
        QCOMPARE(rules.LastMatchFor(0), std::optional<std::size_t>{});
        QCOMPARE(rules.LastMatchFor(1), std::optional<std::size_t>{0u});
        for (std::size_t row = priorRowCount; row < newRowCount; ++row)
        {
            const auto match = rules.LastMatchFor(row);
            const bool warnRow = ((row - priorRowCount) % 2) == 1;
            QCOMPARE(match, warnRow ? std::optional<std::size_t>{0u} : std::optional<std::size_t>{});
        }
    }

    /// Regression: FIFO retention evicts a contiguous prefix.
    /// Without the `rowsRemoved` wiring, `LastMatchFor(0)` would
    /// return the pre-eviction match instead of the new row 0's.
    void EvictionShiftsRowMatchCache()
    {
        HighlightRuleSet rules;
        LogModel model{/*parent=*/nullptr, /*theme=*/nullptr, /*anchors=*/nullptr, &rules};

        // Alternating info/warn: even rows miss, odd rows hit.
        const LevelFixture fixture(6, {"info", "warn"});
        StreamJsonPathInto(model, fixture.Path());
        std::vector<Rule> ruleSet;
        ruleSet.push_back(MakeContainsRule("warn-hit", "level", "warn"));
        rules.SetRules(std::move(ruleSet), model.Configuration().columns, &model.Table());
        QCOMPARE(rules.LastMatchFor(0), std::optional<std::size_t>{});
        QCOMPARE(rules.LastMatchFor(1), std::optional<std::size_t>{0u});
        QCOMPARE(rules.LastMatchFor(2), std::optional<std::size_t>{});
        QCOMPARE(rules.LastMatchFor(3), std::optional<std::size_t>{0u});

        // Simulate a two-row eviction. Production drives this via
        // `LogModel::rowsRemoved`; the test calls directly to keep
        // the focus on the shift semantics.
        rules.OnRowsEvicted(0, 1);
        // Rows shift down by two: new row 0 is old row 2 (info,
        // miss), new row 1 is old row 3 (warn, hit), etc.
        QCOMPARE(rules.LastMatchFor(0), std::optional<std::size_t>{});
        QCOMPARE(rules.LastMatchFor(1), std::optional<std::size_t>{0u});
        QCOMPARE(rules.LastMatchFor(2), std::optional<std::size_t>{});
        QCOMPARE(rules.LastMatchFor(3), std::optional<std::size_t>{0u});
        // Beyond the survivor range, the cache reports no match.
        QCOMPARE(rules.LastMatchFor(4), std::optional<std::size_t>{});
        QCOMPARE(rules.LastMatchFor(999), std::optional<std::size_t>{});
    }

    /// A rule bound to a key not yet in the schema starts inactive.
    /// After a batch introduces the key and `RebindColumns` runs,
    /// the rule activates and matches the rows carrying the needle.
    void RebindColumnsAfterSchemaGrowth()
    {
        HighlightRuleSet rules;
        LogModel model{/*parent=*/nullptr, /*theme=*/nullptr, /*anchors=*/nullptr, &rules};

        // Pre-install a rule against an unknown key before any
        // streaming; the rule starts inert. Mirrors `MainWindow`
        // applying a saved configuration to an empty session.
        std::vector<Rule> preLoad;
        preLoad.push_back(MakeContainsRule("service-hit", "service", "billing"));
        rules.SetRules(std::move(preLoad), model.Configuration().columns, /*table=*/nullptr);
        QCOMPARE(rules.InactiveCount(), 1u);
        QVERIFY(!rules.HasActiveRules());

        // Now stream a file with the `service` column populated.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString grown = dir.filePath("with_service.jsonl");
        {
            std::ofstream stream(grown.toStdString(), std::ios::binary);
            QVERIFY(stream.is_open());
            stream << R"({"level": "info", "service": "billing", "msg": "hit"})" << '\n';
            stream << R"({"level": "info", "service": "auth",    "msg": "miss"})" << '\n';
            stream << R"({"level": "info", "service": "billing", "msg": "hit2"})" << '\n';
        }
        StreamJsonPathInto(model, grown);
        rules.RebindColumns(model.Configuration().columns, &model.Table());

        QCOMPARE(rules.InactiveCount(), 0u);
        QVERIFY(rules.HasActiveRules());

        std::size_t hitCount = 0;
        for (std::size_t row = 0; row < model.Table().RowCount(); ++row)
        {
            if (rules.LastMatchFor(row) == std::optional<std::size_t>{0u})
            {
                ++hitCount;
            }
        }
        QCOMPARE(hitCount, 2u);
    }

    /// `ClearMatches` drops the row-match cache but keeps the
    /// compiled rules ready for the next stream.
    void ClearMatchesResetsCache()
    {
        HighlightRuleSet rules;
        LogModel model{/*parent=*/nullptr, /*theme=*/nullptr, /*anchors=*/nullptr, &rules};

        const LevelFixture fixture(4, {"info", "warn"});
        StreamJsonPathInto(model, fixture.Path());
        std::vector<Rule> ruleSet;
        ruleSet.push_back(MakeContainsRule("warn-hit", "level", "warn"));
        rules.SetRules(std::move(ruleSet), model.Configuration().columns, &model.Table());

        QVERIFY(rules.HasActiveRules());
        QCOMPARE(rules.LastMatchFor(1), std::optional<std::size_t>{0u});

        rules.ClearMatches();
        QVERIFY(rules.HasActiveRules()); // compiled rules retained
        // Every row is now "no data" -- returns nullopt.
        for (std::size_t row = 0; row < model.Table().RowCount(); ++row)
        {
            QCOMPARE(rules.LastMatchFor(row), std::optional<std::size_t>{});
        }
    }

    /// A Boolean rule with `filterValues = {"true"}` accepts only
    /// the rows whose bool column decodes to `true`. Guards
    /// `CompileRule`'s Boolean branch.
    void BooleanRuleMatchesTrueRows()
    {
        HighlightRuleSet rules;
        LogModel model{/*parent=*/nullptr, /*theme=*/nullptr, /*anchors=*/nullptr, &rules};

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("bool.jsonl");
        {
            std::ofstream stream(path.toStdString(), std::ios::binary);
            QVERIFY(stream.is_open());
            stream << R"({"level": "info", "handled": true,  "msg": "a"})" << '\n';
            stream << R"({"level": "info", "handled": false, "msg": "b"})" << '\n';
            stream << R"({"level": "info", "handled": true,  "msg": "c"})" << '\n';
            stream << R"({"level": "info", "handled": false, "msg": "d"})" << '\n';
        }
        StreamJsonPathInto(model, path);

        Rule boolRule;
        boolRule.name = "handled=true";
        boolRule.enabled = true;
        boolRule.columnKeys = {"handled"};
        boolRule.type = Rule::Type::Boolean;
        boolRule.filterValues = {"true"};
        std::vector<Rule> ruleSet;
        ruleSet.push_back(std::move(boolRule));
        rules.SetRules(std::move(ruleSet), model.Configuration().columns, &model.Table());

        QVERIFY(rules.HasActiveRules());
        QCOMPARE(rules.InactiveCount(), 0u);
        QCOMPARE(rules.LastMatchFor(0), std::optional<std::size_t>{0u});
        QCOMPARE(rules.LastMatchFor(1), std::optional<std::size_t>{});
        QCOMPARE(rules.LastMatchFor(2), std::optional<std::size_t>{0u});
        QCOMPARE(rules.LastMatchFor(3), std::optional<std::size_t>{});
    }

    /// Regression: an empty `filterString` used to compile into a
    /// matcher that returned true for every row -- the whole table
    /// lit up on a fresh rule the user hadn't finished typing.
    /// `CompileRule` now treats empty as inactive.
    void EmptyNeedleLeavesRuleInactive()
    {
        HighlightRuleSet rules;
        LogModel model{/*parent=*/nullptr, /*theme=*/nullptr, /*anchors=*/nullptr, &rules};
        const LevelFixture fixture(4, {"info", "warn"});
        StreamJsonPathInto(model, fixture.Path());

        std::vector<Rule> ruleSet;
        // Empty needle across all four string match types.
        for (const auto matchType :
             {Rule::Match::Exactly, Rule::Match::Contains, Rule::Match::RegularExpression, Rule::Match::Wildcard})
        {
            Rule r;
            r.name = "empty";
            r.enabled = true;
            r.columnKeys = {"level"};
            r.type = Rule::Type::String;
            r.matchType = matchType;
            r.filterString = std::string{};
            ruleSet.push_back(std::move(r));
        }
        rules.SetRules(std::move(ruleSet), model.Configuration().columns, &model.Table());

        QCOMPARE(rules.InactiveCount(), 4u);
        QVERIFY(!rules.HasActiveRules());
        for (std::size_t row = 0; row < 4; ++row)
        {
            QCOMPARE(rules.LastMatchFor(row), std::optional<std::size_t>{});
        }
    }

    /// Number-range parity: covers min-only, max-only, both
    /// bounds, and the rejection of the bounds-less case.
    void NumericRangeRule()
    {
        HighlightRuleSet rules;
        LogModel model{/*parent=*/nullptr, /*theme=*/nullptr, /*anchors=*/nullptr, &rules};

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("numbers.jsonl");
        {
            std::ofstream stream(path.toStdString(), std::ios::binary);
            QVERIFY(stream.is_open());
            for (int i = 0; i < 5; ++i)
            {
                stream << R"({"level": "info", "latency_ms": )" << (i * 100) << R"(, "msg": "row"})" << '\n';
            }
        }
        StreamJsonPathInto(model, path);

        // Min-only: highlight rows with latency >= 200 (rows 2, 3, 4).
        Rule minOnly;
        minOnly.name = "slow";
        minOnly.enabled = true;
        minOnly.columnKeys = {"latency_ms"};
        minOnly.type = Rule::Type::Number;
        minOnly.filterMinValue = 200.0;

        // Bounds-less: rejected at compile time (no min, no max).
        Rule empty;
        empty.name = "no-bounds";
        empty.enabled = true;
        empty.columnKeys = {"latency_ms"};
        empty.type = Rule::Type::Number;

        std::vector<Rule> ruleSet;
        ruleSet.push_back(std::move(minOnly));
        ruleSet.push_back(std::move(empty));
        rules.SetRules(std::move(ruleSet), model.Configuration().columns, &model.Table());

        QCOMPARE(rules.InactiveCount(), 1u);
        QVERIFY(rules.HasActiveRules());
        // Rule index 0 matches; index 1 is inert.
        QCOMPARE(rules.LastMatchFor(0), std::optional<std::size_t>{});
        QCOMPARE(rules.LastMatchFor(1), std::optional<std::size_t>{});
        QCOMPARE(rules.LastMatchFor(2), std::optional<std::size_t>{0u});
        QCOMPARE(rules.LastMatchFor(3), std::optional<std::size_t>{0u});
        QCOMPARE(rules.LastMatchFor(4), std::optional<std::size_t>{0u});
    }

    /// Rules bind by keys, so `MoveColumn` + `RebindColumns`
    /// re-resolves the rule against the new layout without losing
    /// matches.
    void MoveColumnPreservesKeyBinding()
    {
        HighlightRuleSet rules;
        LogModel model{/*parent=*/nullptr, /*theme=*/nullptr, /*anchors=*/nullptr, &rules};

        const LevelFixture fixture(4, {"info", "warn"});
        StreamJsonPathInto(model, fixture.Path());

        std::vector<Rule> ruleSet;
        ruleSet.push_back(MakeContainsRule("warn-hit", "level", "warn"));
        rules.SetRules(std::move(ruleSet), model.Configuration().columns, &model.Table());
        const int levelBefore = rules.ResolvedColumnsForTest().front();
        QVERIFY(levelBefore >= 0);

        // Move the level column to the last visible index.
        const int columnCount = static_cast<int>(model.Configuration().columns.size());
        QVERIFY(columnCount >= 2);
        const int lastIndex = columnCount - 1;
        if (levelBefore != lastIndex)
        {
            const bool moved = model.MoveColumn(levelBefore, lastIndex);
            QVERIFY(moved);
        }
        rules.RebindColumns(model.Configuration().columns, &model.Table());
        const int levelAfter = rules.ResolvedColumnsForTest().front();
        QVERIFY(levelAfter >= 0);

        // Matches should track the column even after the move.
        QCOMPARE(rules.LastMatchFor(1), std::optional<std::size_t>{0u});
        QCOMPARE(rules.LastMatchFor(3), std::optional<std::size_t>{0u});
        QCOMPARE(rules.LastMatchFor(0), std::optional<std::size_t>{});
        QCOMPARE(rules.LastMatchFor(2), std::optional<std::size_t>{});
    }
};

QTEST_MAIN(HighlightRuleSetTest)

#include "test_highlight_rule_set.moc"
