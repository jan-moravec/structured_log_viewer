// Direct `LogSession` unit tests (task 1.9). Phase 1 seeds a
// minimal binary so `test/app/CMakeLists.txt` and `ctest`
// register the target; Phase 2 grows this file into the primary
// coverage venue for source, filter, sort, worker, and
// autosave behaviours as they migrate out of `MainWindow`.

#include "anchor_manager.hpp"
#include "highlight_rule_set.hpp"
#include "log_filter_model.hpp"
#include "log_model.hpp"
#include "log_session.hpp"
#include "log_session_presentation.hpp"
#include "row_order_proxy_model.hpp"

#include <Qt>

#include <loglib/filter_expression.hpp>
#include <loglib/log_configuration.hpp>

#include <QObject>
#include <QSettings>
#include <QSignalSpy>
#include <QTest>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>

// NOLINTNEXTLINE(misc-use-internal-linkage): `Q_OBJECT` QtTest fixture.
class LogSessionTest : public QObject
{
    Q_OBJECT

private slots:
    static void TestConstructsWithoutServices()
    {
        const LogSession session;
        QVERIFY(session.Theme() == nullptr);
        QVERIFY(session.HistoryManager() == nullptr);
        QVERIFY(session.RegexTemplates() == nullptr);
    }

    static void TestParentedSessionIsDestroyedWithParent()
    {
        // Parented heap allocation is the canonical Qt ownership
        // pattern: when `parent` goes out of scope its destructor
        // walks `QObject::children()` and `delete`s each entry, so
        // `child` is reaped automatically without a manual delete.
        // The analyzer cannot see that inter-object cleanup;
        // suppress `NewDeleteLeaks` across the block with a
        // matching end-marker at the closing brace.
        QObject parent;
        // NOLINTBEGIN(clang-analyzer-cplusplus.NewDeleteLeaks)
        auto *child = new LogSession(nullptr, nullptr, nullptr, &parent);
        QVERIFY(child->parent() == &parent);
        // NOLINTEND(clang-analyzer-cplusplus.NewDeleteLeaks)
    }

    static void TestInstanceIdIsUniqueAndMonotonic()
    {
        // Identity handles are the pin PRD §8.1 requires so that
        // async callbacks can resolve to the originating session
        // even if the active tab has moved on. The counter is
        // process-scoped, so two sessions built back-to-back must
        // land on strictly increasing ids and never collide with
        // the default sentinel.
        const LogSession first;
        const LogSession second;
        QVERIFY(first.InstanceId().isValid());
        QVERIFY(second.InstanceId().isValid());
        QVERIFY(first.InstanceId() != second.InstanceId());
        QVERIFY(first.InstanceId() < second.InstanceId());
        QVERIFY(SessionInstanceId{} != first.InstanceId());
        QVERIFY(!SessionInstanceId{}.isValid());
    }

    static void TestOwnsModelQuintetAsChildrenInLifetimeOrder()
    {
        // Task 2.1 pins the model quintet into `LogSession`. The
        // accessors must return non-null instances for the entire
        // session lifetime and each must be a QObject child of the
        // session so parent-driven teardown handles cleanup without
        // any manual deletes in later phases.
        const LogSession session;

        QVERIFY(session.Anchors() != nullptr);
        QVERIFY(session.Highlights() != nullptr);
        QVERIFY(session.Model() != nullptr);
        QVERIFY(session.RowOrderProxy() != nullptr);
        QVERIFY(session.FilterProxy() != nullptr);

        // The proxy models override `parent(QModelIndex,int)` from
        // `QAbstractItemModel`, which shadows `QObject::parent()`;
        // qualify the call so we compare against the QObject parent.
        QCOMPARE(session.Anchors()->QObject::parent(), &session);
        QCOMPARE(session.Highlights()->QObject::parent(), &session);
        QCOMPARE(session.Model()->QObject::parent(), &session);
        QCOMPARE(session.RowOrderProxy()->QObject::parent(), &session);
        QCOMPARE(session.FilterProxy()->QObject::parent(), &session);
    }

    static void TestProxyChainIsWiredToOwnedModel()
    {
        // The two proxies must chain onto the owned `LogModel` at
        // construction so a session is immediately usable by a view.
        // Re-wiring in later subtasks would be an invisible
        // regression without this check.
        const LogSession session;

        QCOMPARE(session.RowOrderProxy()->sourceModel(), session.Model());
        QCOMPARE(session.FilterProxy()->sourceModel(), session.RowOrderProxy());
    }

    static void TestHighlightCacheClearsOnModelReset()
    {
        // `LogSession` owns the `modelReset` → `ClearMatches` wire
        // that used to live in the `MainWindow` constructor. Reset
        // through the session's model and verify the highlight set
        // survives (rules persist; only the runtime cache clears).
        const LogSession session;

        session.Model()->Reset();
        QVERIFY(session.Highlights() != nullptr);
    }

    static void TestFiltersDirtySignalFiresOnlyOnTransition()
    {
        // Task 2.4 pins the dirty-state marker onto `LogSession` and
        // exposes it through `MarkFiltersDirty()` /
        // `ClearFiltersDirty()` / `filtersDirtyChanged`. The signal
        // is what the window title binds to for the `[*]` marker, so
        // a redundant mark must be a no-op — otherwise every keystroke
        // in a filter editor would trigger a title repaint.
        LogSession session;
        // NOLINTNEXTLINE(misc-const-correctness): appended-to via `filtersDirtyChanged` slot.
        const QSignalSpy spy(&session, &LogSession::filtersDirtyChanged);
        QVERIFY(spy.isValid());

        QVERIFY(!session.IsFiltersDirty());
        session.MarkFiltersDirty();
        QVERIFY(session.IsFiltersDirty());
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toBool(), true);

        session.MarkFiltersDirty();
        QCOMPARE(spy.count(), 1); // idempotent

        session.ClearFiltersDirty();
        QVERIFY(!session.IsFiltersDirty());
        QCOMPARE(spy.count(), 2);
        QCOMPARE(spy.at(1).at(0).toBool(), false);

        session.ClearFiltersDirty();
        QCOMPARE(spy.count(), 2); // idempotent
    }

    static void TestMarkFiltersDirtyIsSuppressedWhileLoadingConfiguration()
    {
        // The re-entrancy gate lets bulk configuration loads coalesce
        // per-filter `MarkFiltersDirty()` calls into a single signal
        // on scope exit. While the gate is engaged, marking must be
        // a no-op — this is what keeps the `[*]` marker from flashing
        // during a `LoadConfiguration` call.
        LogSession session;
        // NOLINTNEXTLINE(misc-const-correctness): appended-to via `filtersDirtyChanged` slot.
        const QSignalSpy spy(&session, &LogSession::filtersDirtyChanged);

        session.SetLoadingConfiguration(true);
        session.MarkFiltersDirty();
        QVERIFY(!session.IsFiltersDirty());
        QCOMPARE(spy.count(), 0);

        session.SetLoadingConfiguration(false);
        session.MarkFiltersDirty();
        QVERIFY(session.IsFiltersDirty());
        QCOMPARE(spy.count(), 1);
    }

    static void TestPendingApplySortFromConfigLatchIsRoundTrippable()
    {
        // Task 2.4 moves the deferred-sort latch that
        // `TestRestoreLastSessionDefersSortUntilStreamingFinishes`
        // pins onto `LogSession`. The API is a plain boolean the
        // load / restore path sets and `OnStreamingFinished` reads
        // and clears; no signal is needed because nothing outside
        // the session cares about the transition.
        LogSession session;

        QVERIFY(!session.HasPendingApplySortFromConfig());
        session.SetPendingApplySortFromConfig(true);
        QVERIFY(session.HasPendingApplySortFromConfig());
        session.SetPendingApplySortFromConfig(false);
        QVERIFY(!session.HasPendingApplySortFromConfig());
    }

    static void TestSimpleFilterLeavesStartEmptyAndAreMutable()
    {
        // Task 2.4 pins the simple-mode filter leaves onto
        // `LogSession`. `MainWindow::AddLogFilter` /
        // `RemoveLogFilter` still drive the mutations for now, but
        // they reach the storage through
        // `mSession->MutableSimpleLeaves()` /
        // `mSession->MutableSimpleLeafOrder()` — so the accessors
        // must round-trip inserts and preserve insertion order (the
        // display-order invariant `RebuildFilterExpressionFromSimpleLeaves`
        // relies on) exactly as the previous inline `MainWindow`
        // members did.
        LogSession session;

        QVERIFY(session.SimpleLeaves().empty());
        QVERIFY(session.SimpleLeafOrder().empty());

        loglib::LeafRule ruleA;
        ruleA.type = loglib::LeafRule::Type::String;
        ruleA.columnKeys = {"level"};
        ruleA.filterString = "error";

        loglib::LeafRule ruleB;
        ruleB.type = loglib::LeafRule::Type::Boolean;
        ruleB.columnKeys = {"is_error"};
        ruleB.filterValues = {"true"};

        session.MutableSimpleLeaves()["id-a"] = ruleA;
        session.MutableSimpleLeafOrder().emplace_back("id-a");
        session.MutableSimpleLeaves()["id-b"] = ruleB;
        session.MutableSimpleLeafOrder().emplace_back("id-b");

        QCOMPARE(session.SimpleLeaves().size(), std::size_t{2});
        QCOMPARE(session.SimpleLeafOrder().size(), std::size_t{2});
        QCOMPARE(session.SimpleLeafOrder().front(), std::string{"id-a"});
        QCOMPARE(session.SimpleLeafOrder().back(), std::string{"id-b"});
        QVERIFY(session.SimpleLeaves().at("id-a").filterString.has_value());
        QCOMPARE(*session.SimpleLeaves().at("id-a").filterString, std::string{"error"});
    }

    static void TestApplyingEnumRebuildLatchIsRoundTrippable()
    {
        // Task 2.4 relocates the `enumColumnsChanged -> UpdateFilters`
        // re-entrancy guard off `MainWindow` and onto `LogSession`.
        // The API is a plain boolean the shell sets around the
        // rewrite; a nested `enumColumnsChanged` reads it and bails
        // to avoid observing half-rewritten state.
        LogSession session;

        QVERIFY(!session.IsApplyingEnumRebuild());
        session.SetApplyingEnumRebuild(true);
        QVERIFY(session.IsApplyingEnumRebuild());
        session.SetApplyingEnumRebuild(false);
        QVERIFY(!session.IsApplyingEnumRebuild());
    }

    static void TestRebuildFilterExpressionFromSimpleLeavesPreservesOrderAndAdvancedTree()
    {
        // Task 2.4 puts the shared filter-expression reconciler on
        // `LogSession`. The invariants pinned here mirror the ones
        // the old `MainWindow::MirrorSessionStateToConfiguration`
        // carried:
        //
        //   1. Simple-mode leaves become the leading `And` children
        //      in `SimpleLeafOrder()` order.
        //   2. Non-`Leaf` children of the existing root `And` are
        //      preserved — this is how an Advanced-mode `Or`/`Not`
        //      subtree survives a simple-mode edit.
        //   3. A bare-`Leaf` root that duplicates a simple leaf is
        //      dropped so we do not double-apply the same rule.
        LogSession session;

        loglib::LeafRule simpleA;
        simpleA.type = loglib::LeafRule::Type::String;
        simpleA.columnKeys = {"level"};
        simpleA.filterString = "error";

        loglib::LeafRule simpleB;
        simpleB.type = loglib::LeafRule::Type::String;
        simpleB.columnKeys = {"message"};
        simpleB.filterString = "warn";

        session.MutableSimpleLeaves()["id-a"] = simpleA;
        session.MutableSimpleLeafOrder().emplace_back("id-a");
        session.MutableSimpleLeaves()["id-b"] = simpleB;
        session.MutableSimpleLeafOrder().emplace_back("id-b");

        loglib::LeafRule advancedInner;
        advancedInner.type = loglib::LeafRule::Type::String;
        advancedInner.columnKeys = {"module"};
        advancedInner.filterString = "auth";
        loglib::FilterExpression advancedLeaf;
        advancedLeaf.node = loglib::FilterExpression::Leaf{advancedInner};
        loglib::FilterExpression::Or orSubtree;
        orSubtree.children.push_back(std::move(advancedLeaf));
        loglib::FilterExpression advancedBranch;
        advancedBranch.node = std::move(orSubtree);

        loglib::LeafRule staleLeaf;
        staleLeaf.type = loglib::LeafRule::Type::String;
        staleLeaf.columnKeys = {"stale"};
        staleLeaf.filterString = "drop";
        loglib::FilterExpression staleChild;
        staleChild.node = loglib::FilterExpression::Leaf{staleLeaf};

        loglib::FilterExpression::And rootAnd;
        rootAnd.children.push_back(std::move(staleChild)); // top-level Leaf that should be dropped
        rootAnd.children.push_back(std::move(advancedBranch));
        loglib::FilterExpression rootExpression;
        rootExpression.node = std::move(rootAnd);
        session.Model()->ConfigurationManager().SetExpression(std::move(rootExpression));

        session.RebuildFilterExpressionFromSimpleLeaves();

        const auto &recomposed = session.Model()->Configuration().expression;
        const auto *recomposedAnd = std::get_if<loglib::FilterExpression::And>(&recomposed.node);
        QVERIFY(recomposedAnd != nullptr);
        QCOMPARE(recomposedAnd->children.size(), std::size_t{3});

        const auto *firstLeaf = std::get_if<loglib::FilterExpression::Leaf>(&recomposedAnd->children.at(0).node);
        QVERIFY(firstLeaf != nullptr);
        QCOMPARE(firstLeaf->rule, simpleA);

        const auto *secondLeaf = std::get_if<loglib::FilterExpression::Leaf>(&recomposedAnd->children.at(1).node);
        QVERIFY(secondLeaf != nullptr);
        QCOMPARE(secondLeaf->rule, simpleB);

        QVERIFY(std::holds_alternative<loglib::FilterExpression::Or>(recomposedAnd->children.at(2).node));
    }

    static void TestRebuildFilterExpressionPreservesRootOrSubtreeWholesale()
    {
        // A root `Or` (no wrapping `And`) is preserved as a single
        // child so the Advanced tree survives a simple-mode edit,
        // instead of being flattened and losing structure.
        LogSession session;

        loglib::LeafRule simple;
        simple.type = loglib::LeafRule::Type::String;
        simple.columnKeys = {"level"};
        simple.filterString = "warn";
        session.MutableSimpleLeaves()["id-a"] = simple;
        session.MutableSimpleLeafOrder().emplace_back("id-a");

        loglib::LeafRule orLeafRule;
        orLeafRule.type = loglib::LeafRule::Type::String;
        orLeafRule.columnKeys = {"module"};
        orLeafRule.filterString = "auth";
        loglib::FilterExpression orLeaf;
        orLeaf.node = loglib::FilterExpression::Leaf{orLeafRule};

        loglib::FilterExpression::Or rootOr;
        rootOr.children.push_back(std::move(orLeaf));
        loglib::FilterExpression rootExpression;
        rootExpression.node = std::move(rootOr);
        session.Model()->ConfigurationManager().SetExpression(std::move(rootExpression));

        session.RebuildFilterExpressionFromSimpleLeaves();

        const auto &recomposed = session.Model()->Configuration().expression;
        const auto *recomposedAnd = std::get_if<loglib::FilterExpression::And>(&recomposed.node);
        QVERIFY(recomposedAnd != nullptr);
        QCOMPARE(recomposedAnd->children.size(), std::size_t{2});
        QVERIFY(std::holds_alternative<loglib::FilterExpression::Leaf>(recomposedAnd->children.at(0).node));
        QVERIFY(std::holds_alternative<loglib::FilterExpression::Or>(recomposedAnd->children.at(1).node));
    }

    static void TestMirrorSortToConfigurationRoundTripsProxyState()
    {
        // Task 2.4 folds the sort mirror into `LogSession`. The
        // helper reads `FilterProxy()->SortColumn()` /
        // `SortOrder()` and writes them into
        // `Model()->ConfigurationManager()`.
        LogSession session;

        session.FilterProxy()->sort(2, Qt::DescendingOrder);
        session.MirrorSortToConfiguration();

        const auto &sort = session.Model()->Configuration().sort;
        QCOMPARE(sort.columnIndex, 2);
        QCOMPARE(sort.descending, true);
    }

    static void TestMirrorSortHonoursDeferredSortLatch()
    {
        // While `HasPendingApplySortFromConfig()` is true and the
        // proxy is still unsorted (`-1`), the mirror must preserve
        // the configuration's existing sort — writing the transient
        // `-1` would clobber the latched value before
        // `OnStreamingFinished` had a chance to reapply it. Regression:
        // `TestRestoreLastSessionDefersSortUntilStreamingFinishes`.
        LogSession session;

        loglib::LogConfiguration::Sort persisted;
        persisted.columnIndex = 4;
        persisted.descending = true;
        session.Model()->ConfigurationManager().SetSort(persisted);
        session.SetPendingApplySortFromConfig(true);

        QCOMPARE(session.FilterProxy()->SortColumn(), -1);
        session.MirrorSortToConfiguration();

        const auto &sort = session.Model()->Configuration().sort;
        QCOMPARE(sort.columnIndex, 4);
        QCOMPARE(sort.descending, true);
    }

    static void TestMirrorSortWritesLiveValueOnceLatchIsCleared()
    {
        // Once the deferred-sort latch is cleared, an unsorted proxy
        // must persist `-1` — otherwise a user who explicitly
        // removed the sort would silently keep the loaded one.
        LogSession session;

        loglib::LogConfiguration::Sort persisted;
        persisted.columnIndex = 4;
        persisted.descending = true;
        session.Model()->ConfigurationManager().SetSort(persisted);

        session.SetPendingApplySortFromConfig(false);
        session.MirrorSortToConfiguration();

        const auto &sort = session.Model()->Configuration().sort;
        QCOMPARE(sort.columnIndex, -1);
    }

    static void TestMirrorAnchorsToConfigurationCapturesLiveEntries()
    {
        // Task 2.4 folds the anchor mirror into `LogSession`. The
        // helper reads `Anchors()->Entries()` (which drops
        // runtime-only anchors with empty locators) and writes them
        // to `Model()->ConfigurationManager().SetAnchors(...)`.
        LogSession session;

        const AnchorManager::Key persistedKey{.locator = "log-a", .lineId = 42};
        QVERIFY(session.Anchors()->SetAnchor(persistedKey, 3));
        const AnchorManager::Key runtimeOnlyKey{.locator = "", .lineId = 1};
        QVERIFY(session.Anchors()->SetAnchor(runtimeOnlyKey, 1));

        session.MirrorAnchorsToConfiguration();

        const auto &mirrored = session.Model()->Configuration().anchors;
        QCOMPARE(mirrored.size(), std::size_t{1});
        QCOMPARE(mirrored.front().locator, std::string{"log-a"});
        QCOMPARE(mirrored.front().lineId, static_cast<std::uint64_t>(42));
    }

    static void TestSetModeLatchesPreviousModeOnIdleTransition()
    {
        // Task 2.5 pins the session-mode transitions onto
        // `LogSession`. The invariant carried over from
        // `MainWindow::OnStreamingFinished`: an explicit
        // `SetMode(Idle)` snapshots the previous non-`Idle` mode
        // into `LastTerminalMode()` so a `closeEvent` firing after
        // a live-tail finished still routes through the live-tail
        // gate in `ShouldAutoSaveSession`.
        LogSession session;

        QCOMPARE(session.SessionMode(), LogSession::Mode::Idle);
        QCOMPARE(session.LastTerminalMode(), LogSession::Mode::Idle);
        QVERIFY(!session.IsSessionActive());
        QVERIFY(!session.IsLiveTailSession());

        session.SetMode(LogSession::Mode::LiveTail);
        QCOMPARE(session.SessionMode(), LogSession::Mode::LiveTail);
        QCOMPARE(session.LastTerminalMode(), LogSession::Mode::Idle);
        QVERIFY(session.IsSessionActive());
        QVERIFY(session.IsLiveTailSession());

        session.SetMode(LogSession::Mode::Idle);
        QCOMPARE(session.SessionMode(), LogSession::Mode::Idle);
        QCOMPARE(session.LastTerminalMode(), LogSession::Mode::LiveTail);
        QVERIFY(!session.IsSessionActive());
        QVERIFY(!session.IsLiveTailSession());
    }

    static void TestSetModeNonIdleTransitionDoesNotOverwriteLastTerminalMode()
    {
        // A `Static -> LiveTail` transition (or vice versa) must
        // leave `LastTerminalMode()` alone — it captures only the
        // most recent terminated run.
        LogSession session;
        session.SetMode(LogSession::Mode::Static);
        session.SetMode(LogSession::Mode::Idle);
        QCOMPARE(session.LastTerminalMode(), LogSession::Mode::Static);

        session.SetMode(LogSession::Mode::LiveTail);
        // Reset had not happened between; the previous terminated
        // mode still shines through until the next Idle transition.
        QCOMPARE(session.LastTerminalMode(), LogSession::Mode::Static);
    }

    static void TestResetModeClearsBothFields()
    {
        // `ResetMode()` covers the load-configuration / session-
        // switch paths that clear both fields at once so a stale
        // `LiveTail` cannot bleed into the restored session.
        LogSession session;
        session.SetMode(LogSession::Mode::LiveTail);
        session.SetMode(LogSession::Mode::Idle);
        QCOMPARE(session.LastTerminalMode(), LogSession::Mode::LiveTail);

        session.ResetMode();
        QCOMPARE(session.SessionMode(), LogSession::Mode::Idle);
        QCOMPARE(session.LastTerminalMode(), LogSession::Mode::Idle);
    }

    static void TestStreamingCountersDefaultAndRoundTrip()
    {
        // Task 2.5 folds the streaming progress counters into
        // `LogSession`. Callers on `MainWindow` used to write these
        // through `mStreamingLineCount = X;` etc; they now flow
        // through setters so the session can grow signals later
        // without changing every write site.
        LogSession session;

        QCOMPARE(session.StreamingLineCount(), qsizetype{0});
        QCOMPARE(session.StreamingErrorCount(), qsizetype{0});
        QCOMPARE(session.StreamingErrorsCut(), std::size_t{0});
        QVERIFY(!session.FirstStreamingBatchSeen());
        QVERIFY(!session.IsSourceWaiting());
        QVERIFY(session.StreamingFileName().isEmpty());

        session.SetStreamingLineCount(42);
        session.SetStreamingErrorCount(3);
        session.SetStreamingErrorsCut(7);
        session.SetFirstStreamingBatchSeen(true);
        session.SetSourceWaiting(true);
        session.SetStreamingFileName(QStringLiteral("logs/app.log"));

        QCOMPARE(session.StreamingLineCount(), qsizetype{42});
        QCOMPARE(session.StreamingErrorCount(), qsizetype{3});
        QCOMPARE(session.StreamingErrorsCut(), std::size_t{7});
        QVERIFY(session.FirstStreamingBatchSeen());
        QVERIFY(session.IsSourceWaiting());
        QCOMPARE(session.StreamingFileName(), QStringLiteral("logs/app.log"));
    }

    static void TestResetStreamingProgressClearsPerFileFieldsOnly()
    {
        // Per-file start on the streaming path resets line/error
        // count and the first-batch gate; every other field must
        // survive so a multi-file open keeps its file name and the
        // errors-cut watermark stays in lockstep with the model's
        // accumulated errors vector.
        LogSession session;
        session.SetStreamingLineCount(9);
        session.SetStreamingErrorCount(4);
        session.SetStreamingErrorsCut(12);
        session.SetFirstStreamingBatchSeen(true);
        session.SetSourceWaiting(true);
        session.SetStreamingFileName(QStringLiteral("logs/app.log"));

        session.ResetStreamingProgress();

        QCOMPARE(session.StreamingLineCount(), qsizetype{0});
        QCOMPARE(session.StreamingErrorCount(), qsizetype{0});
        QVERIFY(!session.FirstStreamingBatchSeen());
        QCOMPARE(session.StreamingErrorsCut(), std::size_t{12});
        QVERIFY(session.IsSourceWaiting());
        QCOMPARE(session.StreamingFileName(), QStringLiteral("logs/app.log"));
    }

    static void TestResetStreamingCountersAndFileNameClearsAll()
    {
        // Session-switch / retention-fail paths do a full clear.
        LogSession session;
        session.SetStreamingLineCount(9);
        session.SetStreamingErrorCount(4);
        session.SetStreamingErrorsCut(12);
        session.SetFirstStreamingBatchSeen(true);
        session.SetSourceWaiting(true);
        session.SetStreamingFileName(QStringLiteral("logs/app.log"));

        session.ResetStreamingCountersAndFileName();

        QCOMPARE(session.StreamingLineCount(), qsizetype{0});
        QCOMPARE(session.StreamingErrorCount(), qsizetype{0});
        QCOMPARE(session.StreamingErrorsCut(), std::size_t{0});
        QVERIFY(!session.FirstStreamingBatchSeen());
        QVERIFY(!session.IsSourceWaiting());
        QVERIFY(session.StreamingFileName().isEmpty());
    }

    static void TestCurrentSourceStartsEmptyAndRoundTrips()
    {
        // Task 2.5 folds `mCurrentSource` into `LogSession`. Reads
        // go through `CurrentSource()` (const); mutation sites use
        // `MutableCurrentSource()` to keep in-place `->` edits and
        // `AppendLocator(...)` calls cheap.
        LogSession session;
        QVERIFY(!session.CurrentSource().has_value());

        loglib::LogConfiguration::Source src;
        src.kind = loglib::LogConfiguration::Source::Kind::File;
        src.locators = {"logs/app.log"};
        src.locatorDedupKeys = {"logs/app.log"};
        session.SetCurrentSource(src);
        QVERIFY(session.CurrentSource().has_value());
        QCOMPARE(session.CurrentSource()->locators.size(), std::size_t{1});
        QCOMPARE(session.CurrentSource()->locators.front(), std::string{"logs/app.log"});

        session.MutableCurrentSource()->followRotationSiblings = true;
        QVERIFY(session.CurrentSource()->followRotationSiblings);

        session.ResetCurrentSource();
        QVERIFY(!session.CurrentSource().has_value());
    }

    static void TestSessionSwitchInProgressLatchIsRoundTrippable()
    {
        // Task 2.5 moves the session-switch latch off `MainWindow`;
        // `MainWindow::SessionSwitchScope` still guards the on/off
        // pairing but now flips this flag on the session. The flag
        // must round-trip so `OnStreamingFinished` can distinguish
        // a real `Cancelled` from the synchronous emit that
        // `mModel->Reset()` fires under the scope.
        LogSession session;

        QVERIFY(!session.IsSessionSwitchInProgress());
        session.SetSessionSwitchInProgress(true);
        QVERIFY(session.IsSessionSwitchInProgress());
        session.SetSessionSwitchInProgress(false);
        QVERIFY(!session.IsSessionSwitchInProgress());
    }

    static void TestResetSimpleFilterStateClearsBothContainers()
    {
        // `MainWindow::ResetSimpleFilterState()` now forwards to
        // `LogSession::ResetSimpleFilterState()`; the map and its
        // display-order companion must clear together so the
        // reconciler cannot see a stale UUID in one without the
        // other (which used to manifest as "phantom filter" menu
        // items after a session switch).
        LogSession session;
        loglib::LeafRule rule;
        rule.type = loglib::LeafRule::Type::String;
        rule.columnKeys = {"level"};
        rule.filterString = "warn";
        session.MutableSimpleLeaves()["id-a"] = rule;
        session.MutableSimpleLeafOrder().emplace_back("id-a");
        QVERIFY(!session.SimpleLeaves().empty());
        QVERIFY(!session.SimpleLeafOrder().empty());

        session.ResetSimpleFilterState();

        QVERIFY(session.SimpleLeaves().empty());
        QVERIFY(session.SimpleLeafOrder().empty());
    }

    static void TestPendingLiveTailPromotionRoundTripsAndTakes()
    {
        // Task 2.7 hosts the static-prefix-to-live-tail promotion
        // fields on `LogSession`. `SetPendingLiveTailPromotion`
        // matches the assignment used by `OpenLogStreamFromPath`;
        // `TakePendingLiveTailPromotion` is the one-shot consumer
        // called by `ContinueLiveTailAfterPrefix`.
        LogSession session;
        QVERIFY(!session.HasPendingLiveTailPromotion());
        QVERIFY(session.PendingLiveTailPrimary().isEmpty());
        QCOMPARE(session.PendingLiveTailRetention(), std::size_t{0});

        session.SetPendingLiveTailPromotion(QStringLiteral("logs/app.log"), std::size_t{2048});
        QVERIFY(session.HasPendingLiveTailPromotion());
        QCOMPARE(session.PendingLiveTailPrimary(), QStringLiteral("logs/app.log"));
        QCOMPARE(session.PendingLiveTailRetention(), std::size_t{2048});

        const auto taken = session.TakePendingLiveTailPromotion();
        QCOMPARE(taken.first, QStringLiteral("logs/app.log"));
        QCOMPARE(taken.second, std::size_t{2048});
        QVERIFY(!session.HasPendingLiveTailPromotion());
        QVERIFY(session.PendingLiveTailPrimary().isEmpty());
        QCOMPARE(session.PendingLiveTailRetention(), std::size_t{0});

        // A retention-only promotion (all-prefix-files-failed rescue
        // path) is still "pending" so the promoter runs.
        session.SetPendingLiveTailPromotion(QString{}, std::size_t{1});
        QVERIFY(session.HasPendingLiveTailPromotion());
        session.ClearPendingLiveTailPromotion();
        QVERIFY(!session.HasPendingLiveTailPromotion());
    }

    static void TestPreCheckCloseReportsFollowUpsAsBitmask()
    {
        // Review finding #2: the shell needs a side-effect-free probe
        // that distinguishes worker-drain follow-ups from unsaved-edit
        // prompts. `PreCheckClose` returns a bitmask so the shell can
        // aggregate multi-tab decisions ("N tabs have workers, M tabs
        // have unsaved filters, pick one prompt") without inspecting
        // three separate getters.
        //
        //   * Clean session               -> `None` (no follow-up).
        //   * Filters dirty               -> `FiltersDirty`.
        //   * Decompression in flight     -> `DecompressionInFlight`.
        //   * Export in flight            -> `ExportInFlight`.
        //   * Multiple gates simultaneous -> combined bitmask.
        //
        // The probe MUST NOT mutate session state -- multiple
        // consecutive calls on a clean session all return `None`.
        LogSession session;
        QCOMPARE(session.PreCheckClose(), std::uint32_t{0});
        QCOMPARE(session.PreCheckClose(), std::uint32_t{0});

        session.MarkFiltersDirty();
        QCOMPARE(session.PreCheckClose(), static_cast<std::uint32_t>(SessionClosePreconditions::FiltersDirty));
        QCOMPARE(session.PreCheckClose(), static_cast<std::uint32_t>(SessionClosePreconditions::FiltersDirty));

        // Layered gates: dirty + decompression should yield both bits.
        session.SetDecompressionInFlight(true);
        QCOMPARE(
            session.PreCheckClose(),
            static_cast<std::uint32_t>(SessionClosePreconditions::FiltersDirty) |
                static_cast<std::uint32_t>(SessionClosePreconditions::DecompressionInFlight)
        );
        session.SetDecompressionInFlight(false);

        // Isolated worker gates on separate sessions so each bit is
        // pinned independently.
        LogSession decompSession;
        decompSession.SetDecompressionInFlight(true);
        QCOMPARE(
            decompSession.PreCheckClose(), static_cast<std::uint32_t>(SessionClosePreconditions::DecompressionInFlight)
        );
        decompSession.SetDecompressionInFlight(false);
        QCOMPARE(decompSession.PreCheckClose(), std::uint32_t{0});

        LogSession exportSession;
        exportSession.SetExportInFlight(true);
        QCOMPARE(exportSession.PreCheckClose(), static_cast<std::uint32_t>(SessionClosePreconditions::ExportInFlight));
        exportSession.SetExportInFlight(false);
        QCOMPARE(exportSession.PreCheckClose(), std::uint32_t{0});
    }

    static void TestRequestCloseIsPhase2Stub()
    {
        // Review finding #2: `RequestClose` is documented as the
        // side-effecting entry point (cancel-and-drain workers,
        // prompt for unsaved edits) but its body is stubbed in
        // Phase 2 because the orchestration still lives on the
        // shell. Pin the stub so a future contributor cannot add
        // partial side effects without updating this test *and*
        // migrating the shell orchestration.
        LogSession session;
        QCOMPARE(session.RequestClose(), SessionCloseResult::Closed);

        session.MarkFiltersDirty();
        session.SetDecompressionInFlight(true);
        session.SetExportInFlight(true);
        QCOMPARE(session.RequestClose(), SessionCloseResult::Closed);
    }

    static void TestLiveTailElapsedTimerArmsOnStart()
    {
        // Task 2.10 groundwork: the live-tail wall-clock lives on
        // `LogSession`. Before `StartLiveTailElapsedTimer()` runs,
        // the timer reports invalid (`isValid()` false); after
        // starting, subsequent calls to `elapsed()` return a
        // non-negative monotonic delta. Deliberately do NOT check
        // for a positive value -- the monotonic clock ticks in
        // microseconds and QTest can complete the ``elapsed()``
        // call inside a single tick, especially on Windows where
        // ``QElapsedTimer`` uses ``QueryPerformanceCounter``.
        LogSession session;
        QVERIFY(!session.LiveTailElapsedTimer().isValid());

        session.StartLiveTailElapsedTimer();
        QVERIFY(session.LiveTailElapsedTimer().isValid());
        QVERIFY(session.LiveTailElapsedTimer().elapsed() >= 0);

        // Re-arming resets the "since start" clock without dropping
        // the valid latch (matches the shell's re-open lifecycle:
        // each live-tail open calls `StartLiveTailElapsedTimer()`
        // again; `Stop*` leaves the timer armed for the final
        // status line).
        session.StartLiveTailElapsedTimer();
        QVERIFY(session.LiveTailElapsedTimer().isValid());
    }

    static void TestPresentationSnapshotProjectsMigratedState()
    {
        // The shell now populates `SessionPresentationSnapshot`
        // straight from the migrated session state (task 2.5-2.11).
        // Pin the projection so future migrations don't silently
        // regress the tab-strip / status-bar contract.
        LogSession session;
        {
            const auto empty = session.PresentationSnapshot();
            QCOMPARE(empty.mode, SessionSourceMode::Idle);
            QCOMPARE(empty.operations, std::uint32_t{0});
            QVERIFY(!empty.dirty.filtersDirty);
            QVERIFY(!empty.dirty.restorableInPlace);
            QVERIFY(!empty.dirty.ephemeralUnreproducible);
            QVERIFY(empty.tooltip.isEmpty());
            QVERIFY(empty.sourceLabel.isEmpty());
            QVERIFY(empty.shortLabel.isEmpty());
            QCOMPARE(empty.errorCount, qsizetype{0});
            QCOMPARE(empty.droppedErrors, qsizetype{0});
            QVERIFY(empty.mutationsAllowed);
            QVERIFY(!empty.confirmBeforeClose);
        }

        // A dirty static file source should surface as StaticFile mode,
        // restorable-in-place dirty, filtersDirty=true, and
        // confirm-before-close=true (unsaved edits).
        //
        // Review finding #5: seed `mStreamingFileName` with a full
        // path so we exercise the basename-extraction branch that
        // splits `shortLabel` (tab title, no path) from `tooltip`
        // and `sourceLabel` (full source).
        loglib::LogConfiguration::Source fileSource;
        fileSource.kind = loglib::LogConfiguration::Source::Kind::File;
        fileSource.locators = {std::string{"C:/logs/app.log"}};
        session.MutableCurrentSource() = fileSource;
        session.SetMode(LogSession::Mode::Static);
        session.SetStreamingFileName(QStringLiteral("C:/logs/app.log"));
        session.SetStreamingLineCount(qsizetype{1234});
        session.SetStreamingErrorCount(qsizetype{5});
        session.SetStreamingErrorsCut(std::size_t{2});
        session.SetFirstStreamingBatchSeen(true);
        session.MarkFiltersDirty();
        {
            const auto dirty = session.PresentationSnapshot();
            QCOMPARE(dirty.mode, SessionSourceMode::StaticFile);
            QCOMPARE(dirty.operations, std::uint32_t{0});
            QVERIFY(dirty.dirty.filtersDirty);
            QVERIFY(dirty.dirty.restorableInPlace);
            QVERIFY(!dirty.dirty.ephemeralUnreproducible);
            QCOMPARE(dirty.tooltip, QStringLiteral("C:/logs/app.log"));
            QCOMPARE(dirty.sourceLabel, QStringLiteral("C:/logs/app.log"));
            QCOMPARE(dirty.shortLabel, QStringLiteral("app.log"));
            QCOMPARE(dirty.errorCount, qsizetype{5});
            QCOMPARE(dirty.droppedErrors, qsizetype{2});
            QVERIFY(dirty.mutationsAllowed);
            QVERIFY(dirty.confirmBeforeClose);
        }

        // In-flight decompression: mutations blocked, close needs
        // confirmation, Decompressing op flag set. Review finding
        // #4 also promotes the source-mode projection from
        // `StaticFile` to `Compressed` while the worker is armed.
        session.SetDecompressionInFlight(true);
        {
            const auto midDecomp = session.PresentationSnapshot();
            QCOMPARE(midDecomp.mode, SessionSourceMode::Compressed);
            QVERIFY((midDecomp.operations & static_cast<std::uint32_t>(SessionOperationState::Decompressing)) != 0U);
            QVERIFY(!midDecomp.mutationsAllowed);
            QVERIFY(midDecomp.confirmBeforeClose);
        }
        session.SetDecompressionInFlight(false);

        // Live-tail on a file source: LiveTail mode, Ingesting flag
        // set (no source-waiting latch), ephemeralUnreproducible
        // because reopening the locator would silently downgrade to
        // a static open.
        //
        // Review finding #3: a fresh live-tail open with no data
        // yet must NOT project `Parsing` alongside `Ingesting`;
        // `Parsing` is static-only. Assert both bits explicitly.
        session.SetMode(LogSession::Mode::LiveTail);
        session.SetSourceWaiting(false);
        session.SetFirstStreamingBatchSeen(false);
        {
            const auto live = session.PresentationSnapshot();
            QCOMPARE(live.mode, SessionSourceMode::LiveTail);
            QVERIFY((live.operations & static_cast<std::uint32_t>(SessionOperationState::Ingesting)) != 0U);
            QVERIFY((live.operations & static_cast<std::uint32_t>(SessionOperationState::Parsing)) == 0U);
            QVERIFY(live.dirty.ephemeralUnreproducible);
            QVERIFY(!live.dirty.restorableInPlace);
        }
        session.SetFirstStreamingBatchSeen(true);

        // Stdin source overrides the Static/LiveTail projection.
        loglib::LogConfiguration::Source stdinSource;
        stdinSource.kind = loglib::LogConfiguration::Source::Kind::Stdin;
        stdinSource.locators = {std::string{"<stdin>"}};
        session.MutableCurrentSource() = stdinSource;
        session.SetMode(LogSession::Mode::Static);
        {
            const auto stdinSnap = session.PresentationSnapshot();
            QCOMPARE(stdinSnap.mode, SessionSourceMode::Stdin);
            QVERIFY(stdinSnap.dirty.ephemeralUnreproducible);
        }

        // Review finding #8: Kind::NetworkStream branch is otherwise
        // only exercised via `apptest`; pin it here so the enum
        // projection cannot silently regress.
        loglib::LogConfiguration::Source networkSource;
        networkSource.kind = loglib::LogConfiguration::Source::Kind::NetworkStream;
        networkSource.locators = {std::string{"tcp://logs.example.com:5514"}};
        session.MutableCurrentSource() = networkSource;
        session.SetMode(LogSession::Mode::LiveTail);
        {
            const auto netSnap = session.PresentationSnapshot();
            QCOMPARE(netSnap.mode, SessionSourceMode::Network);
            QVERIFY(netSnap.dirty.ephemeralUnreproducible);
            QVERIFY(!netSnap.dirty.restorableInPlace);
        }

        // Review finding #8: `Parsing` bit (positive) and
        // `SourceWaiting` bit. Reset to a static file source with
        // no first-batch latch to isolate the `Parsing` gate; then
        // flip the source-waiting latch and pin that bit
        // independently.
        session.MutableCurrentSource() = fileSource;
        session.SetMode(LogSession::Mode::Static);
        session.SetFirstStreamingBatchSeen(false);
        session.SetSourceWaiting(false);
        {
            const auto parsingSnap = session.PresentationSnapshot();
            QVERIFY((parsingSnap.operations & static_cast<std::uint32_t>(SessionOperationState::Parsing)) != 0U);
            QVERIFY((parsingSnap.operations & static_cast<std::uint32_t>(SessionOperationState::Ingesting)) == 0U);
        }
        session.SetSourceWaiting(true);
        {
            const auto waitingSnap = session.PresentationSnapshot();
            QVERIFY((waitingSnap.operations & static_cast<std::uint32_t>(SessionOperationState::SourceWaiting)) != 0U);
        }
        session.SetSourceWaiting(false);
    }

    static void TestPresentationSnapshotProjectsBundleMultiFileCompressed()
    {
        // Review finding #4: `SessionSourceMode` now carries
        // `Bundle`, `Compressed`, and `MultiFile` variants so tab
        // chrome can pick the right badge without inspecting the
        // descriptor. Pin the priority order: Stdin/Network >
        // Bundle > LiveTail > MultiFile > Compressed > StaticFile.
        LogSession session;
        loglib::LogConfiguration::Source fileSource;
        fileSource.kind = loglib::LogConfiguration::Source::Kind::File;
        fileSource.locators = {std::string{"C:/logs/app.log"}};
        session.MutableCurrentSource() = fileSource;
        session.SetMode(LogSession::Mode::Static);

        // Multi-file static open (rotation history): mode == MultiFile.
        loglib::LogConfiguration::Source multiSource = fileSource;
        multiSource.locators = {
            std::string{"C:/logs/app.log"},
            std::string{"C:/logs/app.log.1"},
            std::string{"C:/logs/app.log.2"},
        };
        session.MutableCurrentSource() = multiSource;
        {
            const auto snap = session.PresentationSnapshot();
            QCOMPARE(snap.mode, SessionSourceMode::MultiFile);
        }

        // MultiFile beats plain Compressed (mode is single-valued;
        // the `Decompressing` operation bit still surfaces the
        // in-flight state separately for the progress indicator).
        session.SetDecompressionInFlight(true);
        {
            const auto snap = session.PresentationSnapshot();
            QCOMPARE(snap.mode, SessionSourceMode::MultiFile);
            QVERIFY((snap.operations & static_cast<std::uint32_t>(SessionOperationState::Decompressing)) != 0U);
        }
        session.SetDecompressionInFlight(false);

        // Third-pass review finding M1: `Compressed` today is
        // transient -- the badge holds only while the decompression
        // worker is armed. Pin the "worker cleared -> mode reverts
        // to the underlying static classification" behaviour so a
        // future contributor who promotes `Compressed` into a
        // "was compressed" latch trips this assertion and updates
        // the docstring (`SessionSourceMode` header explicitly
        // flags this as a phase 3 growth point).
        session.MutableCurrentSource() = fileSource; // single-locator File
        session.SetDecompressionInFlight(true);
        {
            const auto midDecomp = session.PresentationSnapshot();
            QCOMPARE(midDecomp.mode, SessionSourceMode::Compressed);
        }
        session.SetDecompressionInFlight(false);
        {
            const auto afterDecomp = session.PresentationSnapshot();
            QCOMPARE(afterDecomp.mode, SessionSourceMode::StaticFile);
            QVERIFY((afterDecomp.operations & static_cast<std::uint32_t>(SessionOperationState::Decompressing)) == 0U);
        }

        // Bundle beats MultiFile (bundle affordance dominates).
        session.SetApplyEmbeddedBundleConfigForPath(QStringLiteral("C:/logs/bundle.slvbundle"));
        {
            const auto snap = session.PresentationSnapshot();
            QCOMPARE(snap.mode, SessionSourceMode::Bundle);
        }

        // Bundle beats LiveTail too.
        session.SetMode(LogSession::Mode::LiveTail);
        {
            const auto snap = session.PresentationSnapshot();
            QCOMPARE(snap.mode, SessionSourceMode::Bundle);
        }
        session.ClearApplyEmbeddedBundleConfig();

        // With bundle cleared, LiveTail wins over MultiFile.
        {
            const auto snap = session.PresentationSnapshot();
            QCOMPARE(snap.mode, SessionSourceMode::LiveTail);
        }

        // Single-locator static + no worker + no bundle -> StaticFile.
        session.SetMode(LogSession::Mode::Static);
        session.MutableCurrentSource() = fileSource;
        {
            const auto snap = session.PresentationSnapshot();
            QCOMPARE(snap.mode, SessionSourceMode::StaticFile);
        }

        // Stdin overrides bundle armed intent (stream source can't be
        // a bundle in practice; the projection still respects the
        // source-kind override).
        session.SetApplyEmbeddedBundleConfigForPath(QStringLiteral("C:/logs/bundle.slvbundle"));
        loglib::LogConfiguration::Source stdinSource;
        stdinSource.kind = loglib::LogConfiguration::Source::Kind::Stdin;
        stdinSource.locators = {std::string{"<stdin>"}};
        session.MutableCurrentSource() = stdinSource;
        {
            const auto snap = session.PresentationSnapshot();
            QCOMPARE(snap.mode, SessionSourceMode::Stdin);
        }
        session.ClearApplyEmbeddedBundleConfig();
    }

    static void TestPresentationSnapshotShortLabelFallsBackWhenPathIsBare()
    {
        // Review finding #5 edge case: `QFileInfo::fileName` returns
        // "" for a bare directory path ending in a separator. In
        // that case `shortLabel` must fall back to the seed string
        // rather than surfacing an empty tab title. Real code cannot
        // easily hit this path (streaming file names are always
        // basenames or full file paths), but the guard is cheap and
        // future callers that seed `mStreamingFileName` from a
        // descriptor could trip it.
        LogSession session;
        loglib::LogConfiguration::Source fileSource;
        fileSource.kind = loglib::LogConfiguration::Source::Kind::File;
        fileSource.locators = {std::string{"C:/logs/"}};
        session.MutableCurrentSource() = fileSource;
        session.SetMode(LogSession::Mode::Static);
        session.SetStreamingFileName(QStringLiteral("C:/logs/"));
        const auto snap = session.PresentationSnapshot();
        QCOMPARE(snap.shortLabel, QStringLiteral("C:/logs/"));
        QCOMPARE(snap.tooltip, QStringLiteral("C:/logs/"));
    }

    static void TestShouldAutoSaveAfterStreamingWithoutHistoryManager()
    {
        // Task 2.12: `MainWindow::ShouldAutoSaveSession` forwards to
        // `LogSession::ShouldAutoSaveAfterStreaming`. The
        // "no history manager bound" branch is the safe null-out;
        // verify it short-circuits regardless of the source/mode
        // pair. The other branches (file/stream/live-tail source
        // gates) require a real `SessionHistoryManager` (`QDir` +
        // `IRecentsIndexStorage`) and stay covered by the
        // integration `apptest` suite; duplicating that machinery
        // here would just re-test the shell's construction path.
        LogSession session;
        QVERIFY(session.HistoryManager() == nullptr);
        QVERIFY(!session.ShouldAutoSaveAfterStreaming(LogSession::Mode::Static));
        QVERIFY(!session.ShouldAutoSaveAfterStreaming(LogSession::Mode::LiveTail));
        QVERIFY(!session.ShouldAutoSaveAfterStreaming(LogSession::Mode::Idle));

        loglib::LogConfiguration::Source fileSource;
        fileSource.kind = loglib::LogConfiguration::Source::Kind::File;
        fileSource.locators = {std::string{"C:/logs/app.log"}};
        session.MutableCurrentSource() = fileSource;
        QVERIFY(!session.ShouldAutoSaveAfterStreaming(LogSession::Mode::Static));
        QVERIFY(!session.ShouldAutoSaveAfterStreaming(LogSession::Mode::LiveTail));
    }

    static void TestEffectiveAutoDetectRotationHistoryFolds()
    {
        // Task 2.12: `Effective` is `Should` AND (no source OR
        // followRotationSiblings). The CLI override flips
        // `Should` to false unconditionally, so `Effective` follows.
        LogSession session;
        // Clean QSettings state matters here because `Should` reads
        // `ui/autoDetectRotatedHistory`. Reset it to the documented
        // default (`true`) before probing.
        {
            QSettings settings;
            settings.setValue(QStringLiteral("ui/autoDetectRotatedHistory"), true);
            settings.sync();
        }

        session.SetDisableRotationHistoryOverride(false);
        QVERIFY(session.ShouldAutoDetectRotationHistory());
        // No source -> Effective mirrors Should.
        QVERIFY(session.EffectiveAutoDetectRotationHistory());

        loglib::LogConfiguration::Source fileSource;
        fileSource.kind = loglib::LogConfiguration::Source::Kind::File;
        fileSource.locators = {std::string{"C:/logs/app.log"}};
        fileSource.followRotationSiblings = true;
        session.MutableCurrentSource() = fileSource;
        QVERIFY(session.EffectiveAutoDetectRotationHistory());

        // Source-level opt-out (user unchecked "follow rotation siblings"
        // on this source) -> Effective is false even though Should
        // stays true.
        session.MutableCurrentSource()->followRotationSiblings = false;
        QVERIFY(session.ShouldAutoDetectRotationHistory());
        QVERIFY(!session.EffectiveAutoDetectRotationHistory());

        // CLI opt-out short-circuits Should (and therefore
        // Effective) regardless of the source flag.
        session.SetDisableRotationHistoryOverride(true);
        QVERIFY(!session.ShouldAutoDetectRotationHistory());
        QVERIFY(!session.EffectiveAutoDetectRotationHistory());
    }

    static void TestRestorableSessionUuidHonoursSourceKind()
    {
        // `MainWindow::RestorableActiveSessionUuid` forwards here.
        // Verify the five gates the shell relied on:
        //   (1) no pinned uuid                     -> empty
        //   (2) pinned uuid, no source             -> pinned uuid
        //                                            (columns-only)
        //   (3) pinned uuid, file source           -> pinned uuid
        //   (4) pinned uuid, stream source         -> empty
        //   (5) pinned uuid, file + empty locators -> empty
        //       (review finding #6: partially-torn-down state must
        //        not be treated as a columns-only restore)
        LogSession session;
        QVERIFY(session.RestorableSessionUuid().isEmpty());

        const auto pinned = QStringLiteral("11112222-3333-4444-5555-666677778888");
        session.SetAutoSaveUuid(pinned);
        // (2) pinned uuid + no source -> pinned uuid (columns-only).
        QVERIFY(!session.CurrentSource().has_value());
        QCOMPARE(session.RestorableSessionUuid(), pinned);

        // (3) pinned uuid + File source -> pinned uuid.
        loglib::LogConfiguration::Source fileSource;
        fileSource.kind = loglib::LogConfiguration::Source::Kind::File;
        fileSource.locators = {std::string{"C:/logs/app.log"}};
        session.MutableCurrentSource() = fileSource;
        QCOMPARE(session.RestorableSessionUuid(), pinned);

        // (4) pinned uuid + Stream source -> empty (stream sources
        // cannot be re-bound from a saved locator).
        loglib::LogConfiguration::Source streamSource;
        streamSource.kind = loglib::LogConfiguration::Source::Kind::Stdin;
        streamSource.locators = {std::string{"stdin"}};
        session.MutableCurrentSource() = streamSource;
        QVERIFY(session.RestorableSessionUuid().isEmpty());

        // (5) pinned uuid + File source with empty locators -> empty.
        // The prior `!HasLocators` short-circuit collapsed this into
        // the columns-only branch. A partially-cancelled open can
        // leave the descriptor in this state; the shell must fall
        // back to the empty-window path rather than asking the
        // loader to re-open nothing.
        loglib::LogConfiguration::Source emptyFileSource;
        emptyFileSource.kind = loglib::LogConfiguration::Source::Kind::File;
        // locators intentionally empty.
        session.MutableCurrentSource() = emptyFileSource;
        QVERIFY(session.RestorableSessionUuid().isEmpty());

        // (1) drop the pinned uuid -> empty even with a file source.
        session.MutableCurrentSource() = fileSource;
        session.ClearAutoSaveUuid();
        QVERIFY(session.RestorableSessionUuid().isEmpty());
    }

    static void TestDetachAutoSaveUuidNoopsWhenEmpty()
    {
        // `MainWindow::DetachAutoSaveUuid` forwards here. When no
        // uuid is pinned the call must be a straight no-op so
        // `closeEvent` on an unnamed window does not incur a
        // cross-process lock. The other branch (published =>
        // `RemoveOpenWindowUuid`) is covered by the integration
        // `apptest` suite because it needs a live
        // `SessionHistoryManager` bound.
        LogSession session;
        QVERIFY(session.AutoSaveUuid().isEmpty());
        QVERIFY(!session.IsAutoSaveUuidPublished());
        session.DetachAutoSaveUuid();
        QVERIFY(session.AutoSaveUuid().isEmpty());
        QVERIFY(!session.IsAutoSaveUuidPublished());

        session.SetAutoSaveUuid(QStringLiteral("11112222-3333-4444-5555-666677778888"));
        QVERIFY(!session.AutoSaveUuid().isEmpty());
        QVERIFY(!session.IsAutoSaveUuidPublished());
        // Unpublished detach: skips `RemoveOpenWindowUuid` but still
        // clears the pinned uuid + latch (the invariant the
        // pre-migration `MainWindow` also enforced).
        session.DetachAutoSaveUuid();
        QVERIFY(session.AutoSaveUuid().isEmpty());
        QVERIFY(!session.IsAutoSaveUuidPublished());
    }

    static void TestFindMatchCacheRoundTripsAndResets()
    {
        // Task 2.11 folds the find-bar "i of N" match cache onto
        // `LogSession`. Verify that a fresh session starts with an
        // empty optional, that `MutableFindMatchCacheState` lets us
        // populate a cache entry, and that `ResetFindMatchCache`
        // matches `MainWindow::InvalidateFindMatchCache`.
        LogSession session;
        QVERIFY(!session.FindMatchCacheState().has_value());

        session.MutableFindMatchCacheState() = LogSession::FindMatchCache{
            .needle = QStringLiteral("error"),
            .wildcards = false,
            .regularExpressions = true,
            .overflowed = false,
            .sortedRows = {1, 4, 9, 16},
            .totalMatches = 4,
            .bucketCounts = {1U, 0U, 2U, 1U},
        };
        QVERIFY(session.FindMatchCacheState().has_value());
        const auto &cache = *session.FindMatchCacheState();
        QCOMPARE(cache.needle, QStringLiteral("error"));
        QVERIFY(!cache.wildcards);
        QVERIFY(cache.regularExpressions);
        QVERIFY(!cache.overflowed);
        QCOMPARE(cache.sortedRows.size(), std::size_t{4});
        QCOMPARE(cache.sortedRows.back(), 16);
        QCOMPARE(cache.totalMatches, uint32_t{4});
        QCOMPARE(cache.bucketCounts.size(), std::size_t{4});
        QCOMPARE(cache.bucketCounts[2], uint32_t{2});

        session.ResetFindMatchCache();
        QVERIFY(!session.FindMatchCacheState().has_value());
    }

    static void TestWorkerProgressAtomicsRoundTrip()
    {
        // Task 2.8/2.9 fold the decompression + export progress
        // atomics onto `LogSession`. Verify that the mutable
        // accessors let the worker capture a stable pointer, that
        // stores are observed by the const reader, and that a fresh
        // session starts with all four counters at zero.
        LogSession session;
        QCOMPARE(session.DecompressionBytesIn().loadRelaxed(), qint64(0));
        QCOMPARE(session.DecompressionTotalBytesIn().loadRelaxed(), qint64(0));
        QCOMPARE(session.ExportRowsWritten().loadRelaxed(), qint64(0));
        QCOMPARE(session.ExportRowsTotal().loadRelaxed(), qint64(0));

        auto *decompBytesPtr = &session.MutableDecompressionBytesIn();
        auto *decompTotalPtr = &session.MutableDecompressionTotalBytesIn();
        auto *exportWrittenPtr = &session.MutableExportRowsWritten();
        auto *exportTotalPtr = &session.MutableExportRowsTotal();

        decompBytesPtr->storeRelaxed(1024);
        decompTotalPtr->storeRelaxed(8192);
        exportWrittenPtr->storeRelaxed(42);
        exportTotalPtr->storeRelaxed(100);

        QCOMPARE(session.DecompressionBytesIn().loadRelaxed(), qint64(1024));
        QCOMPARE(session.DecompressionTotalBytesIn().loadRelaxed(), qint64(8192));
        QCOMPARE(session.ExportRowsWritten().loadRelaxed(), qint64(42));
        QCOMPARE(session.ExportRowsTotal().loadRelaxed(), qint64(100));

        session.MutableDecompressionBytesIn().storeRelaxed(0);
        session.MutableDecompressionTotalBytesIn().storeRelaxed(0);
        session.MutableExportRowsWritten().storeRelaxed(0);
        session.MutableExportRowsTotal().storeRelaxed(0);
        QCOMPARE(session.DecompressionBytesIn().loadRelaxed(), qint64(0));
        QCOMPARE(session.DecompressionTotalBytesIn().loadRelaxed(), qint64(0));
        QCOMPARE(session.ExportRowsWritten().loadRelaxed(), qint64(0));
        QCOMPARE(session.ExportRowsTotal().loadRelaxed(), qint64(0));
    }

    static void TestWorkerStopSourcesResetAndPropagate()
    {
        // Task 2.8/2.9 fold the decompression + export cooperative
        // stop sources onto `LogSession`. Each `Begin*Async*` site
        // reassigns a fresh `StopSource{}` at the top; each cancel
        // path calls `request_stop()`. Verify that the mutable
        // accessor lets us do both and that a token grabbed before
        // reassignment continues to observe the *old* source's
        // (never-requested) state -- matching the invariant that
        // stale worker callbacks may not leak a cancel into a new
        // open.
        LogSession session;
        QVERIFY(!session.DecompressionStopSource().stop_requested());
        QVERIFY(!session.ExportStopSource().stop_requested());

        const auto decompStaleToken = session.DecompressionStopSource().get_token();
        const auto exportStaleToken = session.ExportStopSource().get_token();

        session.MutableDecompressionStopSource().request_stop();
        session.MutableExportStopSource().request_stop();
        QVERIFY(session.DecompressionStopSource().stop_requested());
        QVERIFY(session.ExportStopSource().stop_requested());
        QVERIFY(decompStaleToken.stop_requested());
        QVERIFY(exportStaleToken.stop_requested());

        session.MutableDecompressionStopSource() = loglib::StopSource{};
        session.MutableExportStopSource() = loglib::StopSource{};
        QVERIFY(!session.DecompressionStopSource().stop_requested());
        QVERIFY(!session.ExportStopSource().stop_requested());
        QVERIFY(decompStaleToken.stop_requested());
        QVERIFY(exportStaleToken.stop_requested());

        const auto decompFreshToken = session.DecompressionStopSource().get_token();
        const auto exportFreshToken = session.ExportStopSource().get_token();
        QVERIFY(!decompFreshToken.stop_requested());
        QVERIFY(!exportFreshToken.stop_requested());
    }

    static void TestExportScalarStateRoundTrips()
    {
        // Task 2.9 folds the export scratch scalars (in-flight
        // latch, bundle-label selector, destination path, format
        // label, wall-clock start) onto `LogSession`.
        // `ClearExportScratchState` matches the paired reset every
        // teardown / cancel / completion site performs.
        LogSession session;
        QVERIFY(!session.IsExportInFlight());
        QVERIFY(!session.IsExportBundle());
        QVERIFY(session.ExportDestinationPath().isEmpty());
        QVERIFY(session.ExportFormatLabel().isEmpty());

        session.SetExportInFlight(true);
        session.SetExportIsBundle(true);
        session.SetExportDestinationPath(QStringLiteral("out/session.slvbundle"));
        session.SetExportFormatLabel(QStringLiteral("Session bundle"));
        const auto started = std::chrono::steady_clock::now();
        session.SetExportStartedAt(started);

        QVERIFY(session.IsExportInFlight());
        QVERIFY(session.IsExportBundle());
        QCOMPARE(session.ExportDestinationPath(), QStringLiteral("out/session.slvbundle"));
        QCOMPARE(session.ExportFormatLabel(), QStringLiteral("Session bundle"));
        QCOMPARE(session.ExportStartedAt(), started);

        session.ClearExportScratchState();
        QVERIFY(session.ExportDestinationPath().isEmpty());
        QVERIFY(session.ExportFormatLabel().isEmpty());
        // Same rationale as the decompression scratch clear: the
        // in-flight and bundle-selector latches are cleared
        // separately (they gate the finished slot / label choice
        // through the completion arc).
        QVERIFY(session.IsExportInFlight());
        QVERIFY(session.IsExportBundle());

        session.SetExportInFlight(false);
        session.SetExportIsBundle(false);
        QVERIFY(!session.IsExportInFlight());
        QVERIFY(!session.IsExportBundle());
    }

    static void TestDecompressionScalarStateRoundTrips()
    {
        // Task 2.8 folds the decompression scratch scalars (in-flight
        // latch, original path, codec name, wall-clock start) onto
        // `LogSession`. `ClearDecompressionScratchPaths` matches the
        // paired reset every teardown / cancel / success-consumed
        // site performs.
        LogSession session;
        QVERIFY(!session.IsDecompressionInFlight());
        QVERIFY(session.DecompressionOriginalPath().isEmpty());
        QVERIFY(session.DecompressionCodecName().isEmpty());

        session.SetDecompressionInFlight(true);
        session.SetDecompressionOriginalPath(QStringLiteral("logs/app.log.gz"));
        session.SetDecompressionCodecName(QStringLiteral("gzip"));
        const auto started = std::chrono::steady_clock::now();
        session.SetDecompressionStartedAt(started);

        QVERIFY(session.IsDecompressionInFlight());
        QCOMPARE(session.DecompressionOriginalPath(), QStringLiteral("logs/app.log.gz"));
        QCOMPARE(session.DecompressionCodecName(), QStringLiteral("gzip"));
        QCOMPARE(session.DecompressionStartedAt(), started);

        session.ClearDecompressionScratchPaths();
        QVERIFY(session.DecompressionOriginalPath().isEmpty());
        QVERIFY(session.DecompressionCodecName().isEmpty());
        // The in-flight latch is deliberately NOT touched by the
        // scratch-paths clear -- the legacy code cleared it
        // separately at each teardown site so that the finished
        // slot can distinguish a clean "no worker" state from a
        // torn-down-but-still-in-progress worker.
        QVERIFY(session.IsDecompressionInFlight());

        session.SetDecompressionInFlight(false);
        QVERIFY(!session.IsDecompressionInFlight());
    }

    static void TestApplyEmbeddedBundleConfigGateRoundTrips()
    {
        // Task 2.8 folds the pending-bundle "may apply embedded
        // configuration" gate onto `LogSession`.
        // `SetApplyEmbeddedBundleConfigForPath` matches the seam in
        // `DispatchMixedOpenInput` where a bundle is armed;
        // `ClearApplyEmbeddedBundleConfig` matches the reset every
        // decompression cancel / failure / success-consumed site
        // performs.
        LogSession session;
        QVERIFY(!session.ShouldApplyEmbeddedBundleConfig());
        QVERIFY(session.ApplyEmbeddedBundleConfigForPath().isEmpty());

        session.SetApplyEmbeddedBundleConfigForPath(QStringLiteral("logs/session.slvbundle"));
        QVERIFY(session.ShouldApplyEmbeddedBundleConfig());
        QCOMPARE(session.ApplyEmbeddedBundleConfigForPath(), QStringLiteral("logs/session.slvbundle"));

        session.ClearApplyEmbeddedBundleConfig();
        QVERIFY(!session.ShouldApplyEmbeddedBundleConfig());
        QVERIFY(session.ApplyEmbeddedBundleConfigForPath().isEmpty());
    }

    static void TestAutoSaveUuidRoundTripsAndDetaches()
    {
        // Task 2.12 puts the recents-entry uuid + publish latch on
        // `LogSession`. `ClearAutoSaveUuid()` matches the pair-reset
        // used by `MainWindow::DetachAutoSaveUuid` after dropping
        // the cross-process publish.
        LogSession session;
        QVERIFY(session.AutoSaveUuid().isEmpty());
        QVERIFY(!session.IsAutoSaveUuidPublished());

        session.SetAutoSaveUuid(QStringLiteral("11112222-3333-4444-5555-666677778888"));
        QCOMPARE(session.AutoSaveUuid(), QStringLiteral("11112222-3333-4444-5555-666677778888"));
        QVERIFY(!session.IsAutoSaveUuidPublished());

        session.SetAutoSaveUuidPublished(true);
        QVERIFY(session.IsAutoSaveUuidPublished());

        session.ClearAutoSaveUuid();
        QVERIFY(session.AutoSaveUuid().isEmpty());
        QVERIFY(!session.IsAutoSaveUuidPublished());
    }

    static void TestRotationExpansionUndoRoundTripsAndClears()
    {
        // Task 2.7 also folds the rotation-expansion undo capture
        // (`mLastRotationExpansion*`) and the per-window CLI opt-out
        // (`mDisableRotationHistoryOverride`) into `LogSession`.
        LogSession session;
        QVERIFY(session.LastRotationExpansionOriginalInputs().isEmpty());
        QVERIFY(!session.LastRotationExpansionWasLiveTail());
        QVERIFY(!session.DisableRotationHistoryOverride());

        const QStringList inputs{QStringLiteral("logs/a.log"), QStringLiteral("logs/b.log")};
        session.SetLastRotationExpansion(inputs, /*wasLiveTail=*/true);
        QCOMPARE(session.LastRotationExpansionOriginalInputs(), inputs);
        QVERIFY(session.LastRotationExpansionWasLiveTail());

        session.SetDisableRotationHistoryOverride(true);
        QVERIFY(session.DisableRotationHistoryOverride());
        session.SetDisableRotationHistoryOverride(false);
        QVERIFY(!session.DisableRotationHistoryOverride());

        session.ClearRotationExpansionUndoState();
        QVERIFY(session.LastRotationExpansionOriginalInputs().isEmpty());
        QVERIFY(!session.LastRotationExpansionWasLiveTail());
    }

    static void TestPendingOpenQueuesRoundTripAndClear()
    {
        // Task 2.5 moves the multi-file-open queue and its two error
        // buckets into `LogSession`. `SetPendingOpenFiles` matches
        // the assignment used by `StartStreamingOpenQueue` and the
        // rotation-prefix seam; `ClearPendingOpenQueues` matches the
        // triplet reset every destructive session-switch performs.
        LogSession session;
        QVERIFY(session.PendingOpenFiles().isEmpty());
        QVERIFY(session.PendingOpenErrors().empty());
        QVERIFY(session.PendingDecompressionErrors().empty());

        const QStringList files{QStringLiteral("logs/a.log"), QStringLiteral("logs/b.log")};
        session.SetPendingOpenFiles(files);
        QCOMPARE(session.PendingOpenFiles(), files);

        // The drain-side helpers mutate the queue in place through
        // the mutable accessor; simulate one iteration of the drain.
        const QString first = session.MutablePendingOpenFiles().takeFirst();
        QCOMPARE(first, QStringLiteral("logs/a.log"));
        QCOMPARE(session.PendingOpenFiles().size(), 1);

        session.MutablePendingOpenErrors().emplace_back("Failed to open 'a.log'");
        session.MutablePendingDecompressionErrors().emplace_back("Failed to decompress 'b.log'");
        QCOMPARE(session.PendingOpenErrors().size(), std::size_t{1});
        QCOMPARE(session.PendingDecompressionErrors().size(), std::size_t{1});

        session.ClearPendingOpenErrors();
        QVERIFY(session.PendingOpenErrors().empty());
        QVERIFY(!session.PendingDecompressionErrors().empty());
        session.ClearPendingDecompressionErrors();
        QVERIFY(session.PendingDecompressionErrors().empty());

        session.MutablePendingOpenErrors().emplace_back("Failed to open 'c.log'");
        session.MutablePendingDecompressionErrors().emplace_back("Failed to decompress 'd.log'");
        session.ClearPendingOpenQueues();
        QVERIFY(session.PendingOpenFiles().isEmpty());
        QVERIFY(session.PendingOpenErrors().empty());
        QVERIFY(session.PendingDecompressionErrors().empty());
    }

    static void TestEnsureWatchersLazilyAllocateAndCacheInstance()
    {
        // Review finding #13: the prior `Set*Watcher` setters were
        // public and untyped, so a caller that re-assigned without
        // draining the previous instance would silently leak.
        // Replace with `Ensure*Watcher`: the session owns the
        // lazy allocation, and repeated calls return the cached
        // instance so shell callers can safely re-wire their
        // `finished` slot with `Qt::UniqueConnection`.
        LogSession session;
        QVERIFY(session.DecompressionWatcherPtr() == nullptr);
        QVERIFY(session.ExportWatcherPtr() == nullptr);

        auto *decomp1 = session.EnsureDecompressionWatcher();
        QVERIFY(decomp1 != nullptr);
        QCOMPARE(session.DecompressionWatcherPtr(), decomp1);
        // Watcher is parented on the session so tab teardown reaps
        // it automatically; assert the parent-child link so a
        // future refactor cannot silently orphan the QObject.
        QCOMPARE(decomp1->parent(), &session);

        // Second call must return the cached instance (no
        // reallocation, no leak).
        auto *decomp2 = session.EnsureDecompressionWatcher();
        QCOMPARE(decomp2, decomp1);

        auto *exp1 = session.EnsureExportWatcher();
        QVERIFY(exp1 != nullptr);
        QCOMPARE(session.ExportWatcherPtr(), exp1);
        QCOMPARE(exp1->parent(), &session);

        auto *exp2 = session.EnsureExportWatcher();
        QCOMPARE(exp2, exp1);
    }

    static void TestSessionDestructorDrainsWatchersWithoutArmedFuture()
    {
        // Second-pass review finding: `~LogSession` waits on any
        // watcher its `Ensure*Watcher()` sites lazily allocated,
        // even if no future was armed. This is the "no-op" branch
        // of the drain -- production teardown always runs behind
        // `MainWindow::CancelInFlight*` which drained via
        // `waitForFinished()` already, and a bare unit test that
        // just allocates the watcher without arming a future must
        // still tear down cleanly. Also covers the "no watcher
        // ever allocated" path (both watchers null on a fresh
        // session): `~LogSession` must remain a no-op in that
        // case.
        {
            // Fresh session, never allocated a watcher.
            LogSession session;
            QVERIFY(session.DecompressionWatcherPtr() == nullptr);
            QVERIFY(session.ExportWatcherPtr() == nullptr);
            // Falls out of scope; no crash.
        }
        {
            // Session that allocated both watchers but never armed
            // a future. Destructor's `waitForFinished()` returns
            // immediately for a watcher on the default (finished)
            // future.
            LogSession session;
            (void)session.EnsureDecompressionWatcher();
            (void)session.EnsureExportWatcher();
            QVERIFY(session.DecompressionWatcherPtr() != nullptr);
            QVERIFY(session.ExportWatcherPtr() != nullptr);
            // Falls out of scope; no crash, no leaked watcher.
        }
    }

    static void TestFindColumnIndexByKeysLocatesConfiguredColumns()
    {
        // Review finding #7: `FindColumnIndexByKeys` was previously
        // only exercised via `apptest`. It is a pure lookup over
        // `Model()->Configuration().columns`; pin every branch here
        // so a future column-key refactor cannot silently break the
        // Go To Column path without a direct-`LogSession` failure.
        LogSession session;

        // No columns installed yet -> every lookup returns -1.
        QCOMPARE(session.FindColumnIndexByKeys({std::string{"ts"}}), -1);

        loglib::LogConfiguration cfg;
        cfg.columns.push_back(
            loglib::LogConfiguration::Column{
                .header = "timestamp",
                .keys = {std::string{"ts"}},
                .type = loglib::LogConfiguration::Type::Time,
            }
        );
        cfg.columns.push_back(
            loglib::LogConfiguration::Column{
                .header = "level",
                .keys = {std::string{"log"}, std::string{"level"}},
                .type = loglib::LogConfiguration::Type::Enumeration,
            }
        );
        cfg.columns.push_back(
            loglib::LogConfiguration::Column{
                .header = "msg",
                .keys = {std::string{"msg"}},
                .type = loglib::LogConfiguration::Type::String,
            }
        );
        session.Model()->ConfigurationManager().SetConfiguration(cfg);

        // Single-key column lookup.
        QCOMPARE(session.FindColumnIndexByKeys({std::string{"ts"}}), 0);
        // Multi-key column lookup: exact match on the full keys vector.
        QCOMPARE(session.FindColumnIndexByKeys({std::string{"log"}, std::string{"level"}}), 1);
        QCOMPARE(session.FindColumnIndexByKeys({std::string{"msg"}}), 2);

        // Empty keys -> -1 (documented behaviour; there is no
        // "unnamed" column to match).
        QCOMPARE(session.FindColumnIndexByKeys({}), -1);

        // Partial match on a multi-key column -> no match (the keys
        // vector must equal the column's keys exactly; a prefix
        // like {"log"} alone does not resolve to column 1).
        QCOMPARE(session.FindColumnIndexByKeys({std::string{"log"}}), -1);

        // Unknown key -> -1.
        QCOMPARE(session.FindColumnIndexByKeys({std::string{"unknown"}}), -1);
    }

    static void TestFindFirstRowAtOrAfterTimestampGuardsInvalidInputs()
    {
        // Review finding #7: `FindFirstRowAtOrAfterTimestamp` has
        // three fast paths (monotonic-no-sort, non-monotonic,
        // user-sort-active) that are heavily exercised via
        // `apptest` because building a populated `LogModel` in a
        // unit test requires a full `LineSource` mock. Pin the
        // guard branches here so a future contributor cannot
        // introduce a regression that returns a stale row for a
        // clearly-invalid input.
        LogSession session;

        // No columns / no rows -> -1 regardless of the target.
        QCOMPARE(session.FindFirstRowAtOrAfterTimestamp(0, 0), -1);
        QCOMPARE(session.FindFirstRowAtOrAfterTimestamp(0, 1'700'000'000'000'000), -1);

        // Negative column index -> -1 without probing the model.
        QCOMPARE(session.FindFirstRowAtOrAfterTimestamp(-1, 0), -1);

        // Configuring columns without appending any rows keeps
        // `sourceRowCount == 0`, so the target column being in-range
        // still yields -1.
        loglib::LogConfiguration cfg;
        cfg.columns.push_back(
            loglib::LogConfiguration::Column{
                .header = "timestamp",
                .keys = {std::string{"ts"}},
                .type = loglib::LogConfiguration::Type::Time,
            }
        );
        session.Model()->ConfigurationManager().SetConfiguration(cfg);
        QCOMPARE(session.FindFirstRowAtOrAfterTimestamp(0, 0), -1);
    }

    static void TestPresentationChangedFiresOnModeTransitionsOnly()
    {
        // Review finding #9: `SetMode` / `ResetMode` both emit
        // `presentationChanged()` -- but only on actual transitions.
        // Pin both the fires-on-transition and the
        // no-emit-on-idempotent-write invariants so a future
        // contributor cannot silently swap either method for an
        // unconditional emit (which would spam every source-status
        // consumer on every reset).
        LogSession session;
        QSignalSpy spy(&session, &LogSession::presentationChanged);
        QVERIFY(spy.isValid());

        // Idle -> Idle: idempotent, no emit.
        session.SetMode(LogSession::Mode::Idle);
        QCOMPARE(spy.count(), 0);

        // Idle -> Static: transition, one emit.
        session.SetMode(LogSession::Mode::Static);
        QCOMPARE(spy.count(), 1);

        // Static -> Static: idempotent, still one emit total.
        session.SetMode(LogSession::Mode::Static);
        QCOMPARE(spy.count(), 1);

        // Static -> LiveTail: transition, two emits total.
        session.SetMode(LogSession::Mode::LiveTail);
        QCOMPARE(spy.count(), 2);

        // LiveTail -> Idle (via SetMode): transition, three emits.
        session.SetMode(LogSession::Mode::Idle);
        QCOMPARE(spy.count(), 3);

        // ResetMode on an already-Idle session with an already-Idle
        // last-terminal mirror is a no-op. `SetMode(Idle)` above
        // latched `Static -> LiveTail -> Idle`'s prior LiveTail into
        // `mLastTerminalMode`, so this first Reset transitions the
        // mirror back to Idle and emits.
        session.ResetMode();
        QCOMPARE(spy.count(), 4);

        // Second `ResetMode` on a fully-idle session: no emit.
        session.ResetMode();
        QCOMPARE(spy.count(), 4);
    }

    static void TestPresentationChangedFiresOnEverySnapshotAffectingMutator()
    {
        // Second-pass review finding: the signal docstring promises
        // to fire whenever the snapshot could have changed, but the
        // original Phase 2 landing only wired it up on `SetMode` /
        // `ResetMode`. Pin the full set of mutators here so every
        // non-mode field that projects into `SessionPresentationSnapshot`
        // (source descriptor, dirty state, decompression latch,
        // export latch, source-waiting, first-batch, streaming
        // errors + cut counters, streaming file name, embedded
        // bundle intent) also emits on a real change and stays
        // silent on an idempotent write. Row-count changes are
        // intentionally out of scope -- they flow via the owned
        // models' `rowsInserted` / `modelReset` signals rather
        // than through the session (see the docstring on
        // `presentationChanged`).
        LogSession session;
        QSignalSpy spy(&session, &LogSession::presentationChanged);
        QVERIFY(spy.isValid());
        int expected = 0;

        // MarkFiltersDirty / ClearFiltersDirty: transition emits.
        session.MarkFiltersDirty();
        QCOMPARE(spy.count(), ++expected);
        session.MarkFiltersDirty();
        QCOMPARE(spy.count(), expected);
        session.ClearFiltersDirty();
        QCOMPARE(spy.count(), ++expected);
        session.ClearFiltersDirty();
        QCOMPARE(spy.count(), expected);

        // SetDecompressionInFlight: transition emits (Compressed mode
        // projection + Decompressing op bit + mutations + close).
        session.SetDecompressionInFlight(true);
        QCOMPARE(spy.count(), ++expected);
        session.SetDecompressionInFlight(true);
        QCOMPARE(spy.count(), expected);
        session.SetDecompressionInFlight(false);
        QCOMPARE(spy.count(), ++expected);

        // SetExportInFlight: transition emits.
        session.SetExportInFlight(true);
        QCOMPARE(spy.count(), ++expected);
        session.SetExportInFlight(true);
        QCOMPARE(spy.count(), expected);
        session.SetExportInFlight(false);
        QCOMPARE(spy.count(), ++expected);

        // SetSourceWaiting / SetFirstStreamingBatchSeen: transition
        // emits (SourceWaiting op bit, Parsing op bit).
        session.SetSourceWaiting(true);
        QCOMPARE(spy.count(), ++expected);
        session.SetSourceWaiting(true);
        QCOMPARE(spy.count(), expected);
        session.SetSourceWaiting(false);
        QCOMPARE(spy.count(), ++expected);
        session.SetFirstStreamingBatchSeen(true);
        QCOMPARE(spy.count(), ++expected);
        session.SetFirstStreamingBatchSeen(true);
        QCOMPARE(spy.count(), expected);
        session.SetFirstStreamingBatchSeen(false);
        QCOMPARE(spy.count(), ++expected);

        // SetStreamingErrorCount / SetStreamingErrorsCut: transition
        // emits (errorCount / droppedErrors on the snapshot).
        session.SetStreamingErrorCount(qsizetype{7});
        QCOMPARE(spy.count(), ++expected);
        session.SetStreamingErrorCount(qsizetype{7});
        QCOMPARE(spy.count(), expected);
        session.SetStreamingErrorCount(qsizetype{0});
        QCOMPARE(spy.count(), ++expected);
        session.SetStreamingErrorsCut(std::size_t{3});
        QCOMPARE(spy.count(), ++expected);
        session.SetStreamingErrorsCut(std::size_t{3});
        QCOMPARE(spy.count(), expected);
        session.SetStreamingErrorsCut(std::size_t{0});
        QCOMPARE(spy.count(), ++expected);

        // SetStreamingLineCount is deliberately signal-free (see
        // the docstring): the model owns the row-count fan.
        session.SetStreamingLineCount(qsizetype{9999});
        QCOMPARE(spy.count(), expected);

        // SetStreamingFileName / ClearStreamingFileName: labels
        // (shortLabel / tooltip / sourceLabel) all flip on the
        // string transition.
        session.SetStreamingFileName(QStringLiteral("C:/logs/app.log"));
        QCOMPARE(spy.count(), ++expected);
        session.SetStreamingFileName(QStringLiteral("C:/logs/app.log"));
        QCOMPARE(spy.count(), expected);
        session.ClearStreamingFileName();
        QCOMPARE(spy.count(), ++expected);
        session.ClearStreamingFileName();
        QCOMPARE(spy.count(), expected);

        // SetApplyEmbeddedBundleConfigForPath / Clear: emits on the
        // set/not-set boundary only (the snapshot projects `Bundle`
        // mode on presence, not on the exact path).
        session.SetApplyEmbeddedBundleConfigForPath(QStringLiteral("C:/pkg/session.slvbundle"));
        QCOMPARE(spy.count(), ++expected);
        // Same non-empty path -> still non-empty; no boundary flip.
        session.SetApplyEmbeddedBundleConfigForPath(QStringLiteral("C:/pkg/session.slvbundle"));
        QCOMPARE(spy.count(), expected);
        // Different non-empty path -> still non-empty; no boundary flip.
        session.SetApplyEmbeddedBundleConfigForPath(QStringLiteral("D:/pkg/other.slvbundle"));
        QCOMPARE(spy.count(), expected);
        session.ClearApplyEmbeddedBundleConfig();
        QCOMPARE(spy.count(), ++expected);
        session.ClearApplyEmbeddedBundleConfig();
        QCOMPARE(spy.count(), expected);

        // SetCurrentSource / ResetCurrentSource. `Source` currently
        // has no `operator==`, so `SetCurrentSource` only suppresses
        // the emit on the "nullopt -> nullopt" corner; a same-source
        // rewrite while a value is already set still emits. That is
        // an intentional pragmatic trade-off documented on the
        // implementation. `ResetCurrentSource` uses the cheaper
        // `has_value()` short-circuit.
        loglib::LogConfiguration::Source fileSource;
        fileSource.kind = loglib::LogConfiguration::Source::Kind::File;
        fileSource.locators = {std::string{"C:/logs/app.log"}};
        session.SetCurrentSource(fileSource);
        QCOMPARE(spy.count(), ++expected);
        // value -> value (same descriptor): still emits.
        session.SetCurrentSource(fileSource);
        QCOMPARE(spy.count(), ++expected);
        session.ResetCurrentSource();
        QCOMPARE(spy.count(), ++expected);
        // nullopt -> nullopt via ResetCurrentSource: silent.
        session.ResetCurrentSource();
        QCOMPARE(spy.count(), expected);
        // nullopt -> nullopt via SetCurrentSource: silent.
        session.SetCurrentSource(std::nullopt);
        QCOMPARE(spy.count(), expected);

        // ResetStreamingCountersAndFileName coalesces every field
        // update into a single emit when any of the tracked fields
        // was non-default. On a session that just went through the
        // sequence above, `mStreamingLineCount` is still 9999, so
        // the call transitions at least that scalar and emits once.
        session.ResetStreamingCountersAndFileName();
        QCOMPARE(spy.count(), ++expected);
        // Second call on an already-cleared session: everything is
        // default, no emit.
        session.ResetStreamingCountersAndFileName();
        QCOMPARE(spy.count(), expected);
    }

    static void TestMutableCurrentSourceRawAccessorDoesNotFanPresentationChanged()
    {
        // Third-pass review finding H1/M4: the raw
        // `MutableCurrentSource()` accessor cannot detect what the
        // caller edits, so it intentionally does NOT fan
        // `presentationChanged`. This test pins that silence so a
        // future well-meaning contributor who "helpfully" wires an
        // emit into the accessor breaks this test and re-reads the
        // header docstring instead of silently double-emitting
        // (raw accessor + follow-up `SetCurrentSource` setter).
        //
        // The complementary positive tests that follow
        // (`TestMutateCurrentSourceHelperFansPresentationChanged`,
        // `TestNotifyPresentationChangedFiresUnconditionally`)
        // document the supported ways to opt IN to the fan.
        LogSession session;
        QSignalSpy spy(&session, &LogSession::presentationChanged);
        QVERIFY(spy.isValid());

        loglib::LogConfiguration::Source fileSource;
        fileSource.kind = loglib::LogConfiguration::Source::Kind::File;
        fileSource.locators = {std::string{"C:/logs/app.log"}};

        // Whole-value overwrite through the raw accessor: no fan.
        session.MutableCurrentSource() = fileSource;
        QCOMPARE(spy.count(), 0);

        // Field mutation through the raw accessor's `->`: no fan.
        session.MutableCurrentSource()->followRotationSiblings = true;
        QCOMPARE(spy.count(), 0);

        // Locator append through the raw accessor: no fan.
        session.MutableCurrentSource()->locators.emplace_back(std::string{"C:/logs/app.log.1"});
        QCOMPARE(spy.count(), 0);

        // `.reset()` through the raw accessor: no fan (contrast
        // with `ResetCurrentSource()` which does emit).
        session.MutableCurrentSource().reset();
        QCOMPARE(spy.count(), 0);
    }

    static void TestMutateCurrentSourceHelperFansPresentationChanged()
    {
        // Third-pass review finding H1: `MutateCurrentSource(fn)`
        // is the opt-in wrapper for callers that need in-place
        // mutation AND the presentation fan. It fires the signal
        // unconditionally on scope exit -- the caller opted in
        // by choosing the helper, so no diff-guard runs.
        LogSession session;
        QSignalSpy spy(&session, &LogSession::presentationChanged);
        QVERIFY(spy.isValid());

        loglib::LogConfiguration::Source fileSource;
        fileSource.kind = loglib::LogConfiguration::Source::Kind::File;
        fileSource.locators = {std::string{"C:/logs/app.log"}};

        // Whole-value overwrite via helper: fans once.
        session.MutateCurrentSource([&fileSource](std::optional<loglib::LogConfiguration::Source> &src) {
            src = fileSource;
        });
        QCOMPARE(spy.count(), 1);

        // Field mutation via helper: fans once.
        session.MutateCurrentSource([](std::optional<loglib::LogConfiguration::Source> &src) {
            if (src.has_value())
            {
                src->followRotationSiblings = true;
            }
        });
        QCOMPARE(spy.count(), 2);

        // Even a no-op lambda fans (the helper's contract is
        // unconditional; diff-guard is the caller's job if they
        // want one).
        session.MutateCurrentSource([](std::optional<loglib::LogConfiguration::Source> &) {});
        QCOMPARE(spy.count(), 3);
    }

    static void TestNotifyPresentationChangedFiresUnconditionally()
    {
        // Third-pass review finding H1: `NotifyPresentationChanged()`
        // is the escape hatch for callers that edited raw and now
        // need the subscribers to refresh (e.g. a batch of
        // `MutableCurrentSource()->locators.push_back(...)` calls
        // in a rotation-append loop). Fires every call; no
        // diff-guard.
        LogSession session;
        QSignalSpy spy(&session, &LogSession::presentationChanged);
        QVERIFY(spy.isValid());

        session.NotifyPresentationChanged();
        QCOMPARE(spy.count(), 1);
        session.NotifyPresentationChanged();
        QCOMPARE(spy.count(), 2);
        // Even on an idle session with no state: still fires.
        session.NotifyPresentationChanged();
        QCOMPARE(spy.count(), 3);
    }

    static void TestResetStreamingProgressCoalescesSignalOnRealChange()
    {
        // Third-pass review finding H2: `ResetStreamingProgress()`
        // is called at the start of every stream. Historically it
        // was `noexcept` and signal-free on the assumption that
        // the interim "Parsing" bit would be transient (next batch
        // re-emits). That assumption breaks for slow-source cases
        // (compressed source, network stream in wait) where the
        // tab strip would keep its "loaded" badge between reset
        // and first batch.
        //
        // Contract:
        //   * At least one of `mStreamingLineCount`,
        //     `mStreamingErrorCount`, `mFirstStreamingBatchSeen`
        //     transitions -> single coalesced emit.
        //   * All three already-zero'd -> no emit (rapid
        //     open/close/open must not fan spurious signals).
        LogSession session;
        QSignalSpy spy(&session, &LogSession::presentationChanged);
        QVERIFY(spy.isValid());

        // Fresh session: all three fields at default. Reset is a
        // no-op with no fan.
        session.ResetStreamingProgress();
        QCOMPARE(spy.count(), 0);

        // Arm the "first batch seen" latch (mirrors the state at
        // the tail of a completed stream) then reset: single fan.
        session.SetFirstStreamingBatchSeen(true);
        QCOMPARE(spy.count(), 1);
        session.ResetStreamingProgress();
        QCOMPARE(spy.count(), 2);
        QVERIFY(!session.FirstStreamingBatchSeen());
        QCOMPARE(session.StreamingLineCount(), qsizetype{0});

        // Also verify with error count: transition emits once
        // (coalesced with the batch-seen flip in the same call).
        session.SetFirstStreamingBatchSeen(true);
        session.SetStreamingErrorCount(qsizetype{4});
        const int before = spy.count();
        session.ResetStreamingProgress();
        QCOMPARE(spy.count(), before + 1);
        QCOMPARE(session.StreamingErrorCount(), qsizetype{0});

        // Line count-only transition also fans (setter for line
        // count itself is signal-free by design; the reset must
        // still see the non-zero value and emit).
        session.SetStreamingLineCount(qsizetype{500});
        const int beforeLine = spy.count();
        session.ResetStreamingProgress();
        QCOMPARE(spy.count(), beforeLine + 1);
        QCOMPARE(session.StreamingLineCount(), qsizetype{0});

        // Double-reset on an already-cleared session: silent.
        const int beforeDouble = spy.count();
        session.ResetStreamingProgress();
        QCOMPARE(spy.count(), beforeDouble);
    }

    static void TestFiltersDirtyChangedAndPresentationChangedFanTogether()
    {
        // `filtersDirtyChanged` used to be the only signal that
        // fired on a dirty transition, so `setWindowModified` could
        // subscribe to it directly. Phase 2's snapshot lists
        // `dirty.filtersDirty` and `confirmBeforeClose` as
        // snapshot-observable, so `presentationChanged` must fan
        // out on the same transitions. Verify both fans stay in
        // lockstep (and only on actual transitions).
        LogSession session;
        QSignalSpy dirtySpy(&session, &LogSession::filtersDirtyChanged);
        QSignalSpy presentationSpy(&session, &LogSession::presentationChanged);
        QVERIFY(dirtySpy.isValid());
        QVERIFY(presentationSpy.isValid());

        session.MarkFiltersDirty();
        QCOMPARE(dirtySpy.count(), 1);
        QCOMPARE(presentationSpy.count(), 1);
        // Idempotent second call: both silent.
        session.MarkFiltersDirty();
        QCOMPARE(dirtySpy.count(), 1);
        QCOMPARE(presentationSpy.count(), 1);
        session.ClearFiltersDirty();
        QCOMPARE(dirtySpy.count(), 2);
        QCOMPARE(presentationSpy.count(), 2);
        session.ClearFiltersDirty();
        QCOMPARE(dirtySpy.count(), 2);
        QCOMPARE(presentationSpy.count(), 2);
    }

    static void TestPresentationSnapshotEmptyFileLocatorsIsNotRestorable()
    {
        // Second-pass review finding: `SessionDirtyState::restorableInPlace`
        // used to trust `Kind::File && !LiveTail` as sufficient for
        // "restorable", which diverged from `RestorableSessionUuid`'s
        // stricter locator gate (review finding #6). The three
        // "restorable" predicates (`restorableInPlace`,
        // `RestorableSessionUuid`, `ShouldAutoSaveAfterStreaming`)
        // must now all agree that a File descriptor with an empty
        // locator vector is *not* restorable, otherwise the tab
        // strip would advertise a state the autosave gate will
        // silently refuse to persist.
        LogSession session;
        loglib::LogConfiguration::Source emptyFile;
        emptyFile.kind = loglib::LogConfiguration::Source::Kind::File;
        // locators intentionally empty
        session.MutableCurrentSource() = emptyFile;
        session.SetMode(LogSession::Mode::Static);

        const auto snap = session.PresentationSnapshot();
        // The three sibling predicates must all agree:
        QVERIFY(!snap.dirty.restorableInPlace);
        QVERIFY(!snap.dirty.ephemeralUnreproducible);
        QVERIFY(session.RestorableSessionUuid().isEmpty());
        // (RestorableSessionUuid is empty either because uuid is
        // empty, or because the locator vector is empty -- both
        // hold here.)
    }

    static void TestPresentationSnapshotRowCountsMirrorModel()
    {
        // Review finding #8: `PresentationSnapshot` exposes
        // `rowCount` / `visibleRows` from the model quintet but no
        // existing test asserts they mirror `mModel->rowCount()` /
        // `mSortFilterProxyModel->rowCount()`. Populating rows
        // requires a full LineSource fixture (out of scope for a
        // unit test), so pin the zero-rows path here: the snapshot
        // must project 0 for both counters when the model is empty
        // regardless of the source descriptor / mode state.
        LogSession session;
        loglib::LogConfiguration::Source fileSource;
        fileSource.kind = loglib::LogConfiguration::Source::Kind::File;
        fileSource.locators = {std::string{"C:/logs/app.log"}};
        session.MutableCurrentSource() = fileSource;
        session.SetMode(LogSession::Mode::Static);
        const auto snap = session.PresentationSnapshot();
        QCOMPARE(snap.rowCount, qsizetype{0});
        QCOMPARE(snap.visibleRows, qsizetype{0});

        // The model itself is non-null on a `LogSession` (the ctor
        // constructs the quintet); pin that alias so a future
        // "lazy model" refactor cannot silently short-circuit the
        // snapshot path to project junk row counts.
        QVERIFY(session.Model() != nullptr);
        QCOMPARE(static_cast<qsizetype>(session.Model()->rowCount()), snap.rowCount);
        QVERIFY(session.FilterProxy() != nullptr);
        QCOMPARE(static_cast<qsizetype>(session.FilterProxy()->rowCount()), snap.visibleRows);
    }

    static void TestShouldAutoSaveAfterStreamingPositiveBranchesRequireHistoryManager()
    {
        // Review finding #10: the shell's autosave gate lives on the
        // history-manager pointer; only the "no manager -> false"
        // short-circuit is unit-testable here without pulling in a
        // real `SessionHistoryManager` (which requires a `QDir` and
        // an `IRecentsIndexStorage`).  Extend the null-manager pin
        // with the *positive*-branch inputs so a future gate rewrite
        // that swaps the null check for e.g. a signal-based bind
        // still trips this test when the guard silently starts
        // returning true for an unbound session.
        LogSession session;
        QVERIFY(session.HistoryManager() == nullptr);

        // File source with a locator + Static mode: with a manager,
        // this is the canonical yes-autosave case (`apptest` covers
        // it). Without a manager, still false.
        loglib::LogConfiguration::Source fileSource;
        fileSource.kind = loglib::LogConfiguration::Source::Kind::File;
        fileSource.locators = {std::string{"C:/logs/app.log"}};
        session.MutableCurrentSource() = fileSource;
        QVERIFY(!session.ShouldAutoSaveAfterStreaming(LogSession::Mode::Static));

        // LiveTail on a file source: even with a manager, always
        // false (would silently downgrade the reopen path to a
        // one-shot static open). Without a manager, still false.
        QVERIFY(!session.ShouldAutoSaveAfterStreaming(LogSession::Mode::LiveTail));

        // Stdin source: even with a manager, always false (network /
        // stream sources cannot be re-bound from a saved locator).
        loglib::LogConfiguration::Source stdinSource;
        stdinSource.kind = loglib::LogConfiguration::Source::Kind::Stdin;
        stdinSource.locators = {std::string{"<stdin>"}};
        session.MutableCurrentSource() = stdinSource;
        QVERIFY(!session.ShouldAutoSaveAfterStreaming(LogSession::Mode::Static));

        // File source with empty locators: no locator to persist, so
        // even with a manager, false (mirrors the
        // `RestorableSessionUuid` fix in review finding #6).
        loglib::LogConfiguration::Source emptyFileSource;
        emptyFileSource.kind = loglib::LogConfiguration::Source::Kind::File;
        session.MutableCurrentSource() = emptyFileSource;
        QVERIFY(!session.ShouldAutoSaveAfterStreaming(LogSession::Mode::Static));
    }
};

QTEST_MAIN(LogSessionTest)
#include "log_session_test.moc"
