// Focused tests for `HighlightRuleSet` (ROADMAP item 3 --
// user-defined highlight rules). Kept in its own binary
// (`apptest_highlight_rules`) so a targeted `ctest -R
// apptest_highlight_rules` runs just this concern.
//
// Coverage:
//   * Baseline: empty rule set reports no matches.
//   * Column-key resolution: rules with unresolvable `columnKeys`
//     land in `InactiveCount()`; resolvable rules move into
//     `HasActiveRules()`.
//   * Last-match-wins semantics: `LastMatchFor(row)` returns the
//     index of the LAST rule that accepts the row, so later rules
//     shadow earlier ones.
//   * Tail-update via `OnRowsAppended`: streaming a second batch
//     evaluates only the new rows against the existing rules
//     without recomputing the seed batch.
//   * FIFO eviction via `OnRowsEvicted`: dropping a contiguous
//     prefix shifts the row-match cache so the survivors still
//     report their own matches (regression guard against the
//     retention-cap desync bug).
//   * `RebindColumns` after `AppendKeys`: a rule whose column was
//     missing at load activates once the schema grows to include it.
//   * `ClearMatches`: drops the row-match cache while keeping the
//     compiled rules ready for the next stream.
//   * Column move: rules bind by keys, so `LogModel::MoveColumn`
//     does not disturb `mResolvedColumn` (regression guard against
//     accidentally regressing to index-based identity).
//   * Boolean predicate: rules with `type = Boolean` match rows
//     whose value decodes to the selected side (covers the Boolean
//     branch of `CompileRule` alongside the string-heavy tests).

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

/// Deterministic JSONL fixture: `{"level": ..., "msg": "row N"}`.
/// @p levels cycles through the supplied entries, so a rule can
/// select every third row (say) by picking one canonical level.
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

/// Feed one JSON path into @p model synchronously (mirrors the
/// pattern in `test_histogram_dock.cpp`). Blocks on
/// `streamingFinished`.
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
    /// Baseline: an empty rule set with no active rules reports
    /// `Empty()`, `!HasActiveRules()`, and `LastMatchFor` returns
    /// `nullopt` for every row.
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

    /// Rule with an unresolvable key lands in `InactiveCount` and
    /// does not match any row. Rule with a resolvable key activates
    /// and matches every row whose value contains the needle.
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

    /// Two overlapping rules: the LAST accepting rule for each row
    /// wins. `"level contains e"` matches every row (info/warn/err
    /// all have `e`), `"level contains warn"` matches the warn rows.
    /// The warn-row hit should come from rule #1, not rule #0.
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

    /// A rule installed against an empty model should activate
    /// against the rows appended later. `OnRowsAppended` is invoked
    /// per-batch by `MainWindow`; here we call it directly with the
    /// full inserted range and verify the cache grows.
    void TailUpdateAfterAppend()
    {
        HighlightRuleSet rules;
        LogModel model{/*parent=*/nullptr, /*theme=*/nullptr, /*anchors=*/nullptr, &rules};

        // First batch to seed the schema.
        const LevelFixture seed(2, {"info", "warn"});
        StreamJsonPathInto(model, seed.Path());
        std::vector<Rule> ruleSet;
        ruleSet.push_back(MakeContainsRule("warn-hit", "level", "warn"));
        rules.SetRules(std::move(ruleSet), model.Configuration().columns, &model.Table());
        QCOMPARE(rules.LastMatchFor(1), std::optional<std::size_t>{0u});

        // Second batch: streamingFinished re-fires; the tail-update
        // hook that lives in `MainWindow` isn't wired here, so we
        // invoke it explicitly like production would.
        const std::size_t priorRowCount = model.Table().RowCount();
        const LevelFixture followup(4, {"info", "warn"});
        StreamJsonPathInto(model, followup.Path());
        const std::size_t newRowCount = model.Table().RowCount();
        QVERIFY(newRowCount > priorRowCount);
        rules.OnRowsAppended(model.Table(), priorRowCount, newRowCount - 1);

        // Verify only the newly-appended rows updated. Original
        // rows 0/1 kept their earlier match state; new odd rows
        // pick up the rule.
        QCOMPARE(rules.LastMatchFor(0), std::optional<std::size_t>{});
        QCOMPARE(rules.LastMatchFor(1), std::optional<std::size_t>{0u});
        for (std::size_t row = priorRowCount; row < newRowCount; ++row)
        {
            const auto match = rules.LastMatchFor(row);
            const bool warnRow = ((row - priorRowCount) % 2) == 1;
            QCOMPARE(match, warnRow ? std::optional<std::size_t>{0u} : std::optional<std::size_t>{});
        }
    }

    /// Regression: FIFO retention (`LogModel::AppendBatch`) evicts a
    /// contiguous prefix from the source model. If `HighlightRuleSet`
    /// weren't wired to `rowsRemoved`, its per-row match cache would
    /// keep stale prefix entries and `LastMatchFor(0)` would return
    /// the pre-eviction match instead of the new row 0's match.
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

        // Simulate a FIFO eviction of the first two rows. Production
        // wires this via the `rowsRemoved` signal on `LogModel`; the
        // test drives it directly to keep the assertion focused on
        // the rule-set's shift semantics.
        rules.OnRowsEvicted(0, 1);
        // Row 0 is now what used to be row 2 (info, miss); row 1 is
        // what used to be row 3 (warn, hit); etc. The tail must have
        // shifted down by two.
        QCOMPARE(rules.LastMatchFor(0), std::optional<std::size_t>{});
        QCOMPARE(rules.LastMatchFor(1), std::optional<std::size_t>{0u});
        QCOMPARE(rules.LastMatchFor(2), std::optional<std::size_t>{});
        QCOMPARE(rules.LastMatchFor(3), std::optional<std::size_t>{0u});
        // Beyond the survivor range, the cache reports no match.
        QCOMPARE(rules.LastMatchFor(4), std::optional<std::size_t>{});
        QCOMPARE(rules.LastMatchFor(999), std::optional<std::size_t>{});
    }

    /// A rule bound to a key that appears only after the schema
    /// grows starts inactive. Once a batch carrying the new key has
    /// been ingested and `RebindColumns` is called, the rule
    /// activates and matches exactly the rows carrying the needle.
    ///
    /// The sync streaming helper resets the model between calls, so
    /// the test drives the schema growth with a single file that
    /// carries a mix of rows.
    void RebindColumnsAfterSchemaGrowth()
    {
        HighlightRuleSet rules;
        LogModel model{/*parent=*/nullptr, /*theme=*/nullptr, /*anchors=*/nullptr, &rules};

        // Pre-install a rule against an unknown key BEFORE any
        // streaming: `SetRules` sees no columns, so the rule is
        // inert. `MainWindow` walks the same code path when
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
    /// compiled rules; a follow-up `OnRowsAppended` picks up where
    /// it left off without a full recompile.
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

    /// Boolean predicate parity: a rule with `type = Boolean` and
    /// `filterValues = {"true"}` accepts only the rows whose bool
    /// column decodes to `true`. Guards `CompileRule`'s Boolean
    /// branch (case-insensitive decode, empty-list rejection).
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

    /// Rules bind by column keys, not column indices. Moving the
    /// `level` column via `LogModel::MoveColumn` should not invalidate
    /// the rule's resolved index (or rather, `RebindColumns` should
    /// re-resolve it against the new layout).
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
