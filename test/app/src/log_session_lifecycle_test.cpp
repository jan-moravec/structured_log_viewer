// Focused characterization tests for the destructive-session
// lifecycle ordering documented in
// `tasks/architecture-inventory.md` §3 (task 1.3). This binary is
// created in Phase 1 so subsequent phases can extend it as the
// concrete session state moves out of `MainWindow`.
//
// The initial coverage exercises the shell-agnostic pieces we can
// pin today: `ScopedConnections` composition and the presentation-
// snapshot shape. Phase 2 grows this file to cover
// `NewSession` cancels-export-before-reset,
// `OpenLogStreamFromPath` autosaves-before-reset,
// `closeEvent` cancels-workers-before-autosave, and
// `~MainWindow` resets-model-under-session-switch-scope orderings.

#include "log_session.hpp"
#include "log_session_commands.hpp"
#include "log_session_presentation.hpp"

#include <QObject>
#include <QTest>

// NOLINTNEXTLINE(misc-use-internal-linkage): `Q_OBJECT` QtTest fixture.
class LogSessionLifecycleTest : public QObject
{
    Q_OBJECT

private slots:
    static void TestDefaultPresentationSnapshotIsIdle()
    {
        const LogSession session;
        const SessionPresentationSnapshot snapshot = session.PresentationSnapshot();
        QCOMPARE(snapshot.mode, SessionSourceMode::Idle);
        QCOMPARE(snapshot.operations, 0U);
        QVERIFY(!snapshot.dirty.filtersDirty);
        QVERIFY(!snapshot.dirty.restorableInPlace);
        QVERIFY(!snapshot.dirty.ephemeralUnreproducible);
        QVERIFY(snapshot.mutationsAllowed);
        QVERIFY(!snapshot.confirmBeforeClose);
        QCOMPARE(snapshot.rowCount, static_cast<qsizetype>(0));
        QCOMPARE(snapshot.visibleRows, static_cast<qsizetype>(0));
        QCOMPARE(snapshot.errorCount, static_cast<qsizetype>(0));
        QCOMPARE(snapshot.droppedErrors, static_cast<qsizetype>(0));
        QVERIFY(snapshot.shortLabel.isEmpty());
        QVERIFY(snapshot.tooltip.isEmpty());
    }

    static void TestSkeletonRequestCloseReportsClean()
    {
        LogSession session;
        QCOMPARE(session.RequestClose(), SessionCloseResult::Closed);
    }

    static void TestSkeletonCommandsAreCallableWithoutServices()
    {
        LogSession session;
        session.RequestNewSession();
        session.RequestOpenFiles(QStringList{}, LogSessionCommands::OpenMode::Append);
        session.RequestOpenLogStream(QString{});
        session.RequestAutoSaveSnapshot(false);
        QCOMPARE(session.RequestClose(), SessionCloseResult::Closed);
    }
};

QTEST_MAIN(LogSessionLifecycleTest)
#include "log_session_lifecycle_test.moc"
