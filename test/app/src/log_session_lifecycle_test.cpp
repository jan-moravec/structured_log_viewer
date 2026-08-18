// Tests for session lifecycle commands and default presentation state.

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
        QCOMPARE(snapshot.statusSummary, QStringLiteral("Idle"));
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
