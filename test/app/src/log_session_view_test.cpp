// Tests for `LogSessionView` behavior and ownership of its table
// and overview-rail subtree.

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

    // The guarded session pointer must clear if teardown destroys
    // the session before its view.
    static void TestSessionPointerClearsWhenSessionDestroyedFirst()
    {
        auto session = std::make_unique<LogSession>();
        auto view = std::make_unique<LogSessionView>(session.get());
        session.reset();
        QVERIFY(view->Session() == nullptr);
    }

    static void TestViewOwnsTableRailAndRailModelAsChildren()
    {
        // The view owns the complete table and overview-rail subtree.
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
        // The rail is constructed hidden and attached to the table
        // only when enabled.
        // Use `isHidden()` (not `!isVisible()`): the parent view
        // is never `show()`n in this test, so `isVisible()`
        // returns false regardless of the rail's own state.
        // `!isVisible()` therefore cannot verify the explicit
        // `hide()` call. `isHidden()`
        // is only true when the widget was explicitly hidden.
        auto session = std::make_unique<LogSession>();
        auto view = std::make_unique<LogSessionView>(session.get());
        QVERIFY(view->OverviewRail()->isHidden());
    }

    static void TestTableViewFillsSessionViewLayout()
    {
        // The table occupies the session view's primary layout slot.
        auto session = std::make_unique<LogSession>();
        auto view = std::make_unique<LogSessionView>(session.get());
        auto *layout = qobject_cast<QVBoxLayout *>(view->layout());
        QVERIFY(layout != nullptr);
        QCOMPARE(layout->count(), 1);
        QCOMPARE(layout->itemAt(0)->widget(), view->TableView());
    }

    // Empty-model navigation behavior.

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
        // Out-of-range rows return before follow-tail is disengaged.
        auto session = std::make_unique<LogSession>();
        auto view = std::make_unique<LogSessionView>(session.get());
        QSignalSpy spy(view.get(), &LogSessionView::followTailDisengageRequested);
        view->ScrollToProxyRow(0, /*replaceSelection=*/true);
        QCOMPARE(spy.count(), 0);
    }

    // Header and delegate application behavior.

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
        // No delegate has been installed, so this covers the safe
        // null early-return path rather than detachment.
        auto session = std::make_unique<LogSession>();
        auto view = std::make_unique<LogSessionView>(session.get());
        view->ApplyLevelCellDelegate(nullptr);
        QVERIFY(view->TableView()->itemDelegateForColumn(0) == nullptr);
    }

    // Navigation bodies and parsing are driven directly without modals.

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
        // An empty session returns before latching the timestamp;
        // clearing an unset sticky value remains safe.
        auto session = std::make_unique<LogSession>();
        auto view = std::make_unique<LogSessionView>(session.get());
        view->ExecuteGotoTimestamp(QStringLiteral("bad-input"), std::chrono::system_clock::now());
        QCOMPARE(view->LastGotoTimestampInput(), QString());
        view->ClearGotoStickyInputs();
        QCOMPARE(view->LastGotoTimestampInput(), QString());
    }

    static void TestClearGotoStickyInputsResetsLatchedValue()
    {
        // Seed through the test seam to avoid requiring a populated
        // model with a timestamp column.
        auto session = std::make_unique<LogSession>();
        auto view = std::make_unique<LogSessionView>(session.get());
        view->SetLastGotoTimestampInputForTest(QStringLiteral("2024-04-28T10:00:02+00:00"));
        QCOMPARE(view->LastGotoTimestampInput(), QStringLiteral("2024-04-28T10:00:02+00:00"));
        view->ClearGotoStickyInputs();
        QCOMPARE(view->LastGotoTimestampInput(), QString());
    }

    // Empty-session jump behavior.

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

    // Per-tab progress strip behavior.

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

    // A standalone view is fully model-bound after construction.

    static void TestTableViewIsBoundToSessionFilterProxyOnConstruction()
    {
        // `Initialise` binds the table to the session filter proxy
        // before construction returns.
        auto session = std::make_unique<LogSession>();
        auto view = std::make_unique<LogSessionView>(session.get());
        QVERIFY(view->TableView()->model() != nullptr);
        QCOMPARE(view->TableView()->model(), session->FilterProxy());
    }

    static void TestTableViewHasNonNullSelectionModelOnConstruction()
    {
        // `setModel(...)` creates the selection model required by
        // navigation methods.
        auto session = std::make_unique<LogSession>();
        auto view = std::make_unique<LogSessionView>(session.get());
        QVERIFY(view->TableView()->selectionModel() != nullptr);
    }

    static void TestTableViewSelectionDefaultsAreRowsWithExtendedMode()
    {
        // Row selection, extended mode, and solid row coloring are
        // log-view UX invariants.
        auto session = std::make_unique<LogSession>();
        auto view = std::make_unique<LogSessionView>(session.get());
        QCOMPARE(view->TableView()->selectionBehavior(), QAbstractItemView::SelectRows);
        QCOMPARE(view->TableView()->selectionMode(), QAbstractItemView::ExtendedSelection);
        QCOMPARE(view->TableView()->alternatingRowColors(), false);
    }

    static void TestTableViewSortingEnabledAndHeaderMovableOnConstruction()
    {
        // Sorting and movable header sections are view defaults.
        auto session = std::make_unique<LogSession>();
        auto view = std::make_unique<LogSessionView>(session.get());
        QVERIFY(view->TableView()->isSortingEnabled());
        auto *header = view->TableView()->horizontalHeader();
        QVERIFY(header != nullptr);
        QVERIFY(header->sectionsMovable());
    }

    // Hiding the overview rail must preserve session-view ownership.

    static void TestOverviewRailParentIsSessionViewByDefault()
    {
        auto session = std::make_unique<LogSession>();
        auto view = std::make_unique<LogSessionView>(session.get());
        QCOMPARE(view->OverviewRail()->parent(), view.get());
    }

    static void TestOverviewRailReparentedToViewOnHideStaysWithView()
    {
        // `SetOverviewRailVisible(false)` detaches the rail from
        // the table and reparents it onto the owning session view
        // (not the shell). Destroying the view must then reap the
        // rail even though the rail is no longer visually inside
        // the table.
        auto session = std::make_unique<LogSession>();
        auto view = std::make_unique<LogSessionView>(session.get());
        view->SetOverviewRailVisible(true);
        view->SetOverviewRailVisible(false);

        QPointer<OverviewRailWidget> railGuard = view->OverviewRail();
        QCOMPARE(railGuard->parent(), view.get());
        QVERIFY(railGuard->isHidden());

        view.reset();
        QVERIFY2(railGuard.isNull(), "Rail must die with the session view, not orphan onto shell");
    }

    static void TestOverviewRailParentSurvivesFullToggleCycle()
    {
        // Across an attach-detach-attach cycle, the rail is owned
        // by the table while attached and by the view while detached.
        auto session = std::make_unique<LogSession>();
        auto view = std::make_unique<LogSessionView>(session.get());
        auto *rail = view->OverviewRail();
        auto *table = view->TableView();

        // Initial state: bare-parented on the view, hidden.
        QCOMPARE(rail->parent(), view.get());
        QVERIFY(rail->isHidden());

        view->SetOverviewRailVisible(true);
        QCOMPARE(rail->parent(), table);
        QVERIFY(table->OverviewRail() != nullptr);

        view->SetOverviewRailVisible(false);
        QCOMPARE(rail->parent(), view.get());
        QCOMPARE(table->OverviewRail(), static_cast<QWidget *>(nullptr));

        view->SetOverviewRailVisible(true);
        QCOMPARE(rail->parent(), table);
    }

    // Operation generations identify stale poll callbacks and bump
    // only on an in-flight rising edge.

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
        // Decompression and export generations are independent.
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
