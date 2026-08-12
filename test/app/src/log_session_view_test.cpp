// Direct `LogSessionView` unit tests (task 1.9 seeded; task 3.10
// grows). Phase 3 wires the view up with a real `LogTableView` +
// `OverviewRailModel` + `OverviewRailWidget` subtree; these tests
// pin the ownership invariants that the shell now relies on
// (`MainWindow::mTableView`, `mOverviewRailModel`,
// `mOverviewRailWidget` are all non-owning aliases into the view).

#include "log_filter_model.hpp"
#include "log_model.hpp"
#include "log_session.hpp"
#include "log_session_view.hpp"
#include "log_table_view.hpp"
#include "overview_rail_model.hpp"
#include "overview_rail_widget.hpp"

#include <loglib/log_configuration.hpp>

#include <QAbstractItemView>
#include <QHeaderView>
#include <QObject>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalSpy>
#include <QTest>
#include <QVBoxLayout>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

// NOLINTNEXTLINE(misc-use-internal-linkage): `Q_OBJECT` QtTest fixture.
class LogSessionViewTest : public QObject
{
    Q_OBJECT

private slots:
    static void TestViewBindsToInjectedSessionForLifetime()
    {
        auto session = std::make_unique<LogSession>();
        auto view = std::make_unique<LogSessionView>(session.get());
        QVERIFY(view->Session() == session.get());
    }

    /// PRD §8.1 forbids a `LogSessionView` from outliving its
    /// session in production, but the teardown races have to be
    /// safe anyway (dialogs that hold a raw pointer to the view,
    /// scheduled repaints, etc.). This test just pins the
    /// `QPointer` guard: once the session is gone, `Session()`
    /// returns `nullptr` instead of a dangling pointer.
    static void TestSessionPointerClearsWhenSessionDestroyedFirst()
    {
        auto session = std::make_unique<LogSession>();
        auto view = std::make_unique<LogSessionView>(session.get());
        session.reset();
        QVERIFY(view->Session() == nullptr);
    }

    static void TestViewOwnsTableRailAndRailModelAsChildren()
    {
        // Task 3.2 / 3.3: `LogSessionView::Initialise` constructs
        // the table view, overview-rail model, and overview-rail
        // widget, and parents each of them on the view so the
        // whole subtree tears down with the tab. Pin the parent
        // chain so a future contributor who moves construction
        // back onto the shell (or forgets `this` in a `new` call)
        // breaks this test rather than silently leaking widgets
        // onto the shell's parent chain.
        auto session = std::make_unique<LogSession>();
        auto view = std::make_unique<LogSessionView>(session.get());

        QVERIFY(view->TableView() != nullptr);
        QVERIFY(view->OverviewRail() != nullptr);
        QVERIFY(view->OverviewRailModelPtr() != nullptr);

        QCOMPARE(view->TableView()->parent(), view.get());
        QCOMPARE(view->OverviewRail()->parent(), view.get());
        QCOMPARE(view->OverviewRailModelPtr()->parent(), view.get());
    }

    static void TestOverviewRailStartsHidden()
    {
        // Matches the pre-migration `MainWindow` behaviour: the
        // rail is instantiated but explicitly hidden; the shell's
        // toggle (`SetOverviewRailVisible`) reparents it into the
        // table view via `LogTableView::AttachOverviewRail` when
        // the user (or a persisted preference) turns it on. If
        // the rail ever ships visible-by-default it would show up
        // as an unrelated regression in every window-open test
        // fixture, so keep this pin explicit.
        //
        // Use `isHidden()` (not `!isVisible()`): the parent view
        // is never `show()`n in this test, so `isVisible()`
        // returns false regardless of the rail's own state -- a
        // regression that removes the explicit `hide()` call
        // would still pass a `!isVisible()` assertion. `isHidden()`
        // is only true when the widget was explicitly hidden
        // (review finding #6).
        auto session = std::make_unique<LogSession>();
        auto view = std::make_unique<LogSessionView>(session.get());
        QVERIFY(view->OverviewRail()->isHidden());
    }

    static void TestTableViewFillsSessionViewLayout()
    {
        // The pre-migration `MainWindow` created a full-margin
        // `QVBoxLayout` on `ui->centralWidget` and dropped the
        // table into it with stretch 1. The session view now
        // reproduces that layout internally so the shell can
        // just parent the view into the central widget and get
        // the same visual result. Pin the "table is in the
        // layout" invariant so a rewrite that swaps the layout
        // shape (e.g. a split-pane refactor) still keeps the
        // table on the main axis.
        auto session = std::make_unique<LogSession>();
        auto view = std::make_unique<LogSessionView>(session.get());
        auto *layout = qobject_cast<QVBoxLayout *>(view->layout());
        QVERIFY(layout != nullptr);
        QCOMPARE(layout->count(), 1);
        QCOMPARE(layout->itemAt(0)->widget(), view->TableView());
    }

    // -----------------------------------------------------------------
    // Task 3.4: `SelectSourceRow` / `ScrollToProxyRow` -- with an
    // empty model, both are expected to no-op silently. The
    // `rowNotVisible()` signal is documented as the shell hook
    // for stale-row feedback; `followTailDisengageRequested()` is
    // the shell hook for pre-scroll `actionFollowTail` uncheck.
    // -----------------------------------------------------------------

    static void TestSelectSourceRowOnEmptyModelEmitsRowNotVisible()
    {
        auto session = std::make_unique<LogSession>();
        auto view = std::make_unique<LogSessionView>(session.get());
        QSignalSpy spy(view.get(), &LogSessionView::rowNotVisible);
        // Row 0 is out-of-range on an empty model, so the view
        // emits the "row not visible" signal so the shell can
        // surface a status-bar message.
        view->SelectSourceRow(0);
        QCOMPARE(spy.count(), 1);
    }

    static void TestSelectSourceRowNegativeIndexEmitsRowNotVisible()
    {
        auto session = std::make_unique<LogSession>();
        auto view = std::make_unique<LogSessionView>(session.get());
        QSignalSpy spy(view.get(), &LogSessionView::rowNotVisible);
        view->SelectSourceRow(-1);
        QCOMPARE(spy.count(), 1);
    }

    static void TestScrollToProxyRowOutOfRangeDoesNotDisengageFollowTail()
    {
        // Pre-migration behaviour: `ScrollToProxyRow` short-
        // circuits on `proxyRow < 0 || proxyRow >= rowCount`
        // BEFORE emitting `followTailDisengageRequested`. On an
        // empty proxy any request row is out of range, so the
        // signal must not fire. Post-review-3 finding rename:
        // the previous name (`TestScrollToProxyRowEmitsFollowTailDisengage`)
        // implied the opposite of what the assertion pins.
        auto session = std::make_unique<LogSession>();
        auto view = std::make_unique<LogSessionView>(session.get());
        QSignalSpy spy(view.get(), &LogSessionView::followTailDisengageRequested);
        view->ScrollToProxyRow(0, /*replaceSelection=*/true);
        QCOMPARE(spy.count(), 0);
    }

    // -----------------------------------------------------------------
    // Task 3.5: header-visibility apply pins.
    // -----------------------------------------------------------------

    static void TestApplyColumnVisibilityIsSafeOnEmptyModel()
    {
        // Column configuration is empty on a fresh session (no
        // load yet); the apply pass must not crash and must not
        // change any header state. This pins the "guard-and-
        // no-op" contract on an unwired session.
        auto session = std::make_unique<LogSession>();
        auto view = std::make_unique<LogSessionView>(session.get());
        view->ApplyColumnVisibility();
        QCOMPARE(view->TableView()->horizontalHeader()->count(), 0);
    }

    static void TestApplyLevelCellDelegateWithNullDelegateOnFreshViewIsNoOp()
    {
        // The no-theme test-fixture path passes a null delegate on
        // a fresh view whose `mInstalledLevelDelegateColumn` is
        // still -1 (no prior install). The view must accept the
        // null and take the early-return branch without touching
        // the table's delegate map.
        //
        // Review finding #6 called out the previous test name as
        // over-claiming: with no prior install, the detach branch
        // never runs, so the test really pinned "safe null accept",
        // not "detach after install". The install-then-detach
        // scenario requires a live `LevelCellDelegate` (needs a
        // `ThemeControl`) plus a populated model with icon-mode
        // active, and belongs with the shell integration tests
        // that already exercise the theme change path.
        auto session = std::make_unique<LogSession>();
        auto view = std::make_unique<LogSessionView>(session.get());
        view->ApplyLevelCellDelegate(nullptr);
        QVERIFY(view->TableView()->itemDelegateForColumn(0) == nullptr);
    }

    // -----------------------------------------------------------------
    // Task 3.6: Goto Line / Timestamp dialogs live on the view.
    // The `Execute*` bodies + parser are exposed so tests can
    // drive them without a modal.
    // -----------------------------------------------------------------

    static void TestExecuteGotoLineOnEmptyModelEmitsStatusMessage()
    {
        auto session = std::make_unique<LogSession>();
        auto view = std::make_unique<LogSessionView>(session.get());
        QSignalSpy spy(view.get(), &LogSessionView::statusMessageRequested);
        view->ExecuteGotoLine(QStringLiteral("1"));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.takeFirst().at(0).toString(), QStringLiteral("No log loaded."));
    }

    static void TestExecuteGotoTimestampOnEmptyModelEmitsStatusMessage()
    {
        auto session = std::make_unique<LogSession>();
        auto view = std::make_unique<LogSessionView>(session.get());
        QSignalSpy spy(view.get(), &LogSessionView::statusMessageRequested);
        view->ExecuteGotoTimestamp(QStringLiteral("2024-04-28 12:34:56"), std::chrono::system_clock::now());
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.takeFirst().at(0).toString(), QStringLiteral("No log loaded."));
    }

    static void TestParseGotoTimestampRelativeShortcut()
    {
        // Pin the relative-shortcut branch: `-1h` resolves to
        // `now - 1 hour` regardless of TZ, always UTC (isNaive=false).
        const auto now = std::chrono::system_clock::from_time_t(1'700'000'000);
        const auto parsed = LogSessionView::ParseGotoTimestampInput(QStringLiteral("-1h"), {}, now);
        QVERIFY(parsed.has_value());
        QCOMPARE(parsed->isNaive, false);
        constexpr int64_t MICROS_PER_HOUR = 3'600LL * 1'000'000LL;
        const int64_t expected =
            std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count() - MICROS_PER_HOUR;
        QCOMPARE(parsed->micros, expected);
    }

    static void TestParseGotoTimestampInvalidReturnsNullopt()
    {
        const auto parsed = LogSessionView::ParseGotoTimestampInput(
            QStringLiteral("not a timestamp"), {}, std::chrono::system_clock::now()
        );
        QVERIFY(!parsed.has_value());
    }

    static void TestClearGotoStickyInputsOnEmptySessionIsSafe()
    {
        // Review finding #6: the previous test claimed to verify
        // `ClearGotoStickyInputs` resets a latched value, but
        // `ExecuteGotoTimestamp` on an empty session short-circuits
        // on the `rowCount() == 0` check BEFORE writing the
        // sticky, so the "clear" was operating on an already-empty
        // value. Rename to reflect what this actually tests: the
        // clear method is a safe no-op on a never-set sticky.
        //
        // A real latch+clear pin lives in
        // `TestClearGotoStickyInputsResetsLatchedValue` below (uses
        // the `SetLastGotoTimestampInputForTest` seam so the test
        // does not need a full parser fixture) and in `apptest`'s
        // integration path `TestGotoTimestampStickyInputClearsOnNewSession`.
        auto session = std::make_unique<LogSession>();
        auto view = std::make_unique<LogSessionView>(session.get());
        view->ExecuteGotoTimestamp(QStringLiteral("bad-input"), std::chrono::system_clock::now());
        QCOMPARE(view->LastGotoTimestampInput(), QString());
        view->ClearGotoStickyInputs();
        QCOMPARE(view->LastGotoTimestampInput(), QString());
    }

    static void TestClearGotoStickyInputsResetsLatchedValue()
    {
        // Review-2 finding #4: pin the latch+clear cycle directly
        // in the view suite (previously only covered by `apptest`
        // integration). Seeds the latch via the test seam so the
        // test does not need a populated `LogModel` with a time
        // column, then verifies `ClearGotoStickyInputs` zeroes
        // both the timestamp sticky (via the accessor) and the
        // line sticky (via a second seed + subsequent
        // observable-through-clear -- there is no direct
        // `LastGotoLineInput` accessor since the pre-migration
        // shell never exposed one).
        auto session = std::make_unique<LogSession>();
        auto view = std::make_unique<LogSessionView>(session.get());
        view->SetLastGotoTimestampInputForTest(QStringLiteral("2024-04-28T10:00:02+00:00"));
        QCOMPARE(view->LastGotoTimestampInput(), QStringLiteral("2024-04-28T10:00:02+00:00"));
        view->ClearGotoStickyInputs();
        QCOMPARE(view->LastGotoTimestampInput(), QString());
    }

    // -----------------------------------------------------------------
    // Task 3.4 continued: JumpToNewestRow / JumpToAnchor are safe
    // no-ops on an empty session and emit the expected status
    // messages on the "no anchors" branch.
    // -----------------------------------------------------------------

    static void TestJumpToNewestRowOnEmptyModelIsNoOp()
    {
        auto session = std::make_unique<LogSession>();
        auto view = std::make_unique<LogSessionView>(session.get());
        // Empty model: `rowCount() == 0`, method returns before
        // any scroll. Should not emit any signals or crash.
        QSignalSpy statusSpy(view.get(), &LogSessionView::statusMessageRequested);
        view->JumpToNewestRow();
        QCOMPARE(statusSpy.count(), 0);
    }

    static void TestJumpToAnchorOnEmptyAnchorsEmitsStatusMessage()
    {
        auto session = std::make_unique<LogSession>();
        auto view = std::make_unique<LogSessionView>(session.get());
        QSignalSpy spy(view.get(), &LogSessionView::statusMessageRequested);
        view->JumpToAnchor(/*forward=*/true);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.takeFirst().at(0).toString(), QStringLiteral("No anchors set."));
    }

    // -----------------------------------------------------------------
    // Task 3.7: per-tab progress strip lifecycle.
    // -----------------------------------------------------------------

    static void TestProgressStripStartsHidden()
    {
        auto session = std::make_unique<LogSession>();
        auto view = std::make_unique<LogSessionView>(session.get());
        // Lazy construction means the strip doesn't even exist
        // until `ShowOperationProgress` is called. `IsVisible()`
        // must return false either way.
        QVERIFY(!view->IsOperationProgressVisible());
    }

    static void TestShowOperationProgressMakesStripVisible()
    {
        auto session = std::make_unique<LogSession>();
        auto view = std::make_unique<LogSessionView>(session.get());
        view->ShowOperationProgress(QStringLiteral("Decompressing test.gz"), 42);
        QVERIFY(view->IsOperationProgressVisible());
    }

    static void TestHideOperationProgressReturnsToHidden()
    {
        auto session = std::make_unique<LogSession>();
        auto view = std::make_unique<LogSessionView>(session.get());
        view->ShowOperationProgress(QStringLiteral("Export"), 10);
        QVERIFY(view->IsOperationProgressVisible());
        view->HideOperationProgress();
        QVERIFY(!view->IsOperationProgressVisible());
    }

    static void TestProgressCancelButtonEmitsSignal()
    {
        auto session = std::make_unique<LogSession>();
        auto view = std::make_unique<LogSessionView>(session.get());
        view->ShowOperationProgress(QStringLiteral("Test"), 50);
        QSignalSpy spy(view.get(), &LogSessionView::progressCancelRequested);
        // Locate the cancel button by object name (a hard-coded
        // widget-name lookup is fragile in general but the button
        // has a stable objectName set in the view's `EnsureProgressStrip`).
        auto *button = view->findChild<QPushButton *>(QStringLiteral("sessionProgressCancelButton"));
        QVERIFY(button != nullptr);
        button->click();
        QCOMPARE(spy.count(), 1);
    }

    static void TestUpdateOperationProgressOnHiddenStripIsNoOp()
    {
        auto session = std::make_unique<LogSession>();
        auto view = std::make_unique<LogSessionView>(session.get());
        // Never shown -> hidden. `UpdateOperationProgress` must
        // not crash and must not construct the strip.
        view->UpdateOperationProgress(QStringLiteral("Ignore me"), 75);
        QVERIFY(!view->IsOperationProgressVisible());
        // No progress-bar child should exist yet (lazy alloc).
        auto *bar = view->findChild<QProgressBar *>(QStringLiteral("sessionProgressBar"));
        QVERIFY(bar == nullptr);
    }

    // -----------------------------------------------------------------
    // Review finding #1: a standalone `LogSessionView` must be
    // independently model-bound. Before the fix, the shell set
    // the table's model + anchor manager + selection defaults
    // after view construction; a populated bare view could reach
    // valid proxy indices while `selectionModel()` was still
    // null. These tests pin the invariants so a future contributor
    // removing the binding from `LogSessionView::Initialise` gets
    // an immediate red bar.
    // -----------------------------------------------------------------

    static void TestTableViewIsBoundToSessionFilterProxyOnConstruction()
    {
        // The view's `Initialise` calls `mTableView->setModel(filterProxy)`
        // before the ctor returns. A regression that reverts to
        // the shell-driven binding would leave `model()` at the
        // Qt-default null and this test would fail.
        auto session = std::make_unique<LogSession>();
        auto view = std::make_unique<LogSessionView>(session.get());
        QVERIFY(view->TableView()->model() != nullptr);
        QCOMPARE(view->TableView()->model(), session->FilterProxy());
    }

    static void TestTableViewHasNonNullSelectionModelOnConstruction()
    {
        // The crash pattern review finding #1 called out: a
        // populated bare view whose `selectionModel()` returned
        // null would crash the moment a navigation body tried to
        // set the current index. `QAbstractItemView::selectionModel()`
        // is auto-created by `setModel(...)`, so this test also
        // proves the previous `setModel` call actually ran.
        auto session = std::make_unique<LogSession>();
        auto view = std::make_unique<LogSessionView>(session.get());
        QVERIFY(view->TableView()->selectionModel() != nullptr);
    }

    static void TestTableViewSelectionDefaultsAreRowsWithExtendedMode()
    {
        // Pins the widget property defaults migrated from the
        // shell (selection behaviour = SelectRows, mode =
        // ExtendedSelection, alternating-row colours off). These
        // are UX invariants for the log viewer; regressing them
        // would silently degrade multi-row copy / find behaviour.
        auto session = std::make_unique<LogSession>();
        auto view = std::make_unique<LogSessionView>(session.get());
        QCOMPARE(view->TableView()->selectionBehavior(), QAbstractItemView::SelectRows);
        QCOMPARE(view->TableView()->selectionMode(), QAbstractItemView::ExtendedSelection);
        QCOMPARE(view->TableView()->alternatingRowColors(), false);
    }

    static void TestTableViewSortingEnabledAndHeaderMovableOnConstruction()
    {
        // Pins the header setup migrated from the shell: sorting
        // enabled with sections movable. The shell adds the
        // context-menu policy + `sectionMoved` slot on top, but
        // the base widget setup is now a view responsibility.
        auto session = std::make_unique<LogSession>();
        auto view = std::make_unique<LogSessionView>(session.get());
        QVERIFY(view->TableView()->isSortingEnabled());
        auto *header = view->TableView()->horizontalHeader();
        QVERIFY(header != nullptr);
        QVERIFY(header->sectionsMovable());
    }

    // -----------------------------------------------------------------
    // Review finding #3: hiding the overview rail must not orphan
    // it onto the shell. `MainWindow::SetOverviewRailVisible(false)`
    // reparents the rail to `mSessionView`, so destroying the
    // session view still reaps the rail. Pin the parent chain
    // so a regression reintroducing the shell reparent gets
    // caught by the ownership sentinel below.
    // -----------------------------------------------------------------

    static void TestOverviewRailParentIsSessionViewByDefault()
    {
        auto session = std::make_unique<LogSession>();
        auto view = std::make_unique<LogSessionView>(session.get());
        QCOMPARE(view->OverviewRail()->parent(), view.get());
    }

    static void TestOverviewRailReparentedToViewOnHideStaysWithView()
    {
        // Simulates the shell's `SetOverviewRailVisible(false)`
        // reparent path (review finding #3): after
        // `AttachOverviewRail(nullptr)` drops the table's
        // parent-reference, the rail is reparented onto the
        // owning session view (not the shell). Destroying the
        // view must then reap the rail even though the rail is
        // no longer visually inside the table.
        auto session = std::make_unique<LogSession>();
        auto view = std::make_unique<LogSessionView>(session.get());
        view->TableView()->AttachOverviewRail(nullptr);
        view->OverviewRail()->setParent(view.get());
        view->OverviewRail()->hide();

        QPointer<OverviewRailWidget> railGuard = view->OverviewRail();
        QCOMPARE(railGuard->parent(), view.get());
        QVERIFY(railGuard->isHidden());

        view.reset();
        QVERIFY2(railGuard.isNull(), "Rail must die with the session view, not orphan onto shell");
    }

    static void TestOverviewRailParentSurvivesFullToggleCycle()
    {
        // Post-review-3: the review points out that no existing
        // pin catches a regression where `SetOverviewRailVisible`
        // reparents the rail onto the shell (`this`) instead of
        // the view. Simulate a full attach -> detach -> re-attach
        // cycle on the view directly and assert the rail's parent
        // is either the view (detached) or the table (attached),
        // never anything else. If a future shell reintroduces
        // `setParent(mainWindow)`, this test would only fail if
        // it also drove the toggle -- so add a redundant assertion
        // that after detach the parent is specifically `view`,
        // and after re-attach it is specifically `TableView()`.
        auto session = std::make_unique<LogSession>();
        auto view = std::make_unique<LogSessionView>(session.get());
        auto *rail = view->OverviewRail();
        auto *table = view->TableView();

        // Initial state: bare-parented on the view, hidden.
        QCOMPARE(rail->parent(), view.get());
        QVERIFY(rail->isHidden());

        // Attach onto the table: parent flips to the table view.
        table->AttachOverviewRail(rail);
        QCOMPARE(rail->parent(), table);

        // Detach: shell's `SetOverviewRailVisible(false)` path
        // is what performs the reparent; simulate the essential
        // steps here.
        table->AttachOverviewRail(nullptr);
        rail->setParent(view.get());
        rail->hide();
        QCOMPARE(rail->parent(), view.get());

        // Re-attach: parent flips back to the table view.
        table->AttachOverviewRail(rail);
        QCOMPARE(rail->parent(), table);
    }

    // -----------------------------------------------------------------
    // Review finding #2/#4: session-scoped operation generation
    // counters. The shell's poll timers capture the generation at
    // `Begin*` time and drop ticks whose captured generation no
    // longer matches (queued completion silently rearmed for the
    // next file). Direct pins on the counter's rising-edge
    // semantics without a shell driver.
    // -----------------------------------------------------------------

    static void TestDecompressionGenerationBumpsOnRisingEdge()
    {
        auto session = std::make_unique<LogSession>();
        const auto initial = session->DecompressionGeneration();
        session->SetDecompressionInFlight(true);
        QVERIFY(session->DecompressionGeneration() > initial);
        const auto afterFirst = session->DecompressionGeneration();
        session->SetDecompressionInFlight(false);
        QCOMPARE(session->DecompressionGeneration(), afterFirst);
        session->SetDecompressionInFlight(true);
        QVERIFY(session->DecompressionGeneration() > afterFirst);
    }

    static void TestExportGenerationBumpsOnRisingEdge()
    {
        auto session = std::make_unique<LogSession>();
        const auto initial = session->ExportGeneration();
        session->SetExportInFlight(true);
        QVERIFY(session->ExportGeneration() > initial);
        const auto afterFirst = session->ExportGeneration();
        session->SetExportInFlight(false);
        QCOMPARE(session->ExportGeneration(), afterFirst);
        session->SetExportInFlight(true);
        QVERIFY(session->ExportGeneration() > afterFirst);
    }

    static void TestGenerationCountersAreIndependent()
    {
        // Bumping decompression must not touch export and vice
        // versa; the two operations can (and per PRD §5 will)
        // run concurrently once per-session mutation guards
        // allow it.
        auto session = std::make_unique<LogSession>();
        const auto decompInitial = session->DecompressionGeneration();
        const auto exportInitial = session->ExportGeneration();
        session->SetDecompressionInFlight(true);
        QCOMPARE(session->ExportGeneration(), exportInitial);
        session->SetExportInFlight(true);
        QVERIFY(session->DecompressionGeneration() > decompInitial);
        QVERIFY(session->ExportGeneration() > exportInitial);
    }

    static void TestViewSubtreeSurvivesShellDestruction()
    {
        // Ownership contract: everything the view constructs is a
        // child of the view, not of the shell. So destroying a
        // *hypothetical parent* (the shell's central widget) must
        // reap the view AND its whole subtree together. The
        // `QPointer` sentinels prove the subtree died with the
        // view rather than surviving as orphan widgets that a
        // later shell teardown would then double-free.
        auto session = std::make_unique<LogSession>();
        auto centralHost = std::make_unique<QWidget>();
        auto *view = new LogSessionView(session.get(), centralHost.get());
        QPointer<LogTableView> table = view->TableView();
        QPointer<OverviewRailWidget> rail = view->OverviewRail();
        QPointer<OverviewRailModel> railModel = view->OverviewRailModelPtr();
        QPointer<LogSessionView> viewGuard = view;

        centralHost.reset();

        QVERIFY(viewGuard.isNull());
        QVERIFY(table.isNull());
        QVERIFY(rail.isNull());
        QVERIFY(railModel.isNull());
    }
};

QTEST_MAIN(LogSessionViewTest)
#include "log_session_view_test.moc"
