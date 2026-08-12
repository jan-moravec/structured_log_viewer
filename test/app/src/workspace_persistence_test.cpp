// Grouped workspace persistence tests (task 1.9). Phase 1 seeds a
// minimal binary so `test/app/CMakeLists.txt` and `ctest` register
// the target; Phase 8 grows this file to cover ordered windows,
// stable window identity, geometry, dock state, tab descriptors,
// active-tab identity, atomic write, and restore-on-launch flow.

#include "log_session_presentation.hpp"

#include <QObject>
#include <QTest>

// NOLINTNEXTLINE(misc-use-internal-linkage): `Q_OBJECT` QtTest fixture.
class WorkspacePersistenceTest : public QObject
{
    Q_OBJECT

private slots:
    static void TestSessionSourceModeEnumIsStable()
    {
        // Persistence rides on the numeric values; do not renumber
        // without a schema migration story (Phase 8 task 8.1).
        // Review finding #4 appended `Bundle` / `Compressed` /
        // `MultiFile` at the end so existing tab-strip persistence
        // stays byte-compatible; new values must keep growing
        // upwards from 5.
        QCOMPARE(static_cast<std::uint8_t>(SessionSourceMode::Idle), static_cast<std::uint8_t>(0));
        QCOMPARE(static_cast<std::uint8_t>(SessionSourceMode::StaticFile), static_cast<std::uint8_t>(1));
        QCOMPARE(static_cast<std::uint8_t>(SessionSourceMode::LiveTail), static_cast<std::uint8_t>(2));
        QCOMPARE(static_cast<std::uint8_t>(SessionSourceMode::Stdin), static_cast<std::uint8_t>(3));
        QCOMPARE(static_cast<std::uint8_t>(SessionSourceMode::Network), static_cast<std::uint8_t>(4));
        QCOMPARE(static_cast<std::uint8_t>(SessionSourceMode::Bundle), static_cast<std::uint8_t>(5));
        QCOMPARE(static_cast<std::uint8_t>(SessionSourceMode::Compressed), static_cast<std::uint8_t>(6));
        QCOMPARE(static_cast<std::uint8_t>(SessionSourceMode::MultiFile), static_cast<std::uint8_t>(7));
    }
};

QTEST_MAIN(WorkspacePersistenceTest)
#include "workspace_persistence_test.moc"
