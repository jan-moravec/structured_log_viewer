// Multi-source tab lifecycle tests (task 1.9). Phase 1 seeds a
// minimal binary so `test/app/CMakeLists.txt` and `ctest` register
// the target; Phase 6 grows this file to cover create / close /
// reorder / switch, last-tab-close-closes-window, focus, shortcuts,
// labels, indicators, accessibility, Open / Recent Sessions
// routing, drag / drop, and static-session isolation.

#include "log_session_presentation.hpp"

#include <QObject>
#include <QTest>

// NOLINTNEXTLINE(misc-use-internal-linkage): `Q_OBJECT` QtTest fixture.
class SessionTabsTest : public QObject
{
    Q_OBJECT

private slots:
    static void TestSessionOperationStateFlagsPack()
    {
        // Compact indicators combine several state bits per PRD FR-18.
        const std::uint32_t combined = static_cast<std::uint32_t>(SessionOperationState::Ingesting) |
                                       static_cast<std::uint32_t>(SessionOperationState::Paused);
        QVERIFY((combined & static_cast<std::uint32_t>(SessionOperationState::Ingesting)) != 0U);
        QVERIFY((combined & static_cast<std::uint32_t>(SessionOperationState::Paused)) != 0U);
        QVERIFY((combined & static_cast<std::uint32_t>(SessionOperationState::Exporting)) == 0U);
    }
};

QTEST_MAIN(SessionTabsTest)
#include "session_tabs_test.moc"
