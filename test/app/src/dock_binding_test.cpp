// Dock binding / rebinding tests (task 1.9). Phase 1 seeds a
// minimal binary so `test/app/CMakeLists.txt` and `ctest` register
// the target; Phase 5 grows this file to cover repeated bind,
// unbind, hidden refresh, stale timers, duplicate connections,
// model reset, previous-session destruction, and originating-
// session edit rules across the Find, Parse Errors, Anchors,
// Histogram, and Record Details docks.

#include "log_session.hpp"
#include "log_session_view.hpp"
#include "session_bind_context.hpp"

#include <QObject>
#include <QTest>

#include <memory>

// NOLINTNEXTLINE(misc-use-internal-linkage): `Q_OBJECT` QtTest fixture.
class DockBindingTest : public QObject
{
    Q_OBJECT

private slots:
    static void TestDefaultContextIsUnbound()
    {
        const SessionBindContext context;
        QVERIFY(!context.IsBound());
        QVERIFY(context.session.isNull());
        QVERIFY(context.view.isNull());
        QVERIFY(context.model.isNull());
        QVERIFY(context.anchors.isNull());
        QVERIFY(context.highlights.isNull());
    }

    static void TestContextObservesGuardedSessionDestruction()
    {
        auto session = std::make_unique<LogSession>();
        auto view = std::make_unique<LogSessionView>(session.get());
        SessionBindContext context;
        context.session = session.get();
        context.view = view.get();
        QVERIFY(!context.session.isNull());
        QVERIFY(!context.view.isNull());
        view.reset();
        session.reset();
        QVERIFY(context.session.isNull());
        QVERIFY(context.view.isNull());
        QVERIFY(!context.IsBound());
    }
};

QTEST_MAIN(DockBindingTest)
#include "dock_binding_test.moc"
