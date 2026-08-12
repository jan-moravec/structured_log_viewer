// Direct `LogSessionView` unit tests (task 1.9). Phase 1 seeds a
// minimal binary so `test/app/CMakeLists.txt` and `ctest` register
// the target; Phase 3 grows this file to cover model binding,
// selection, follow-newest, jump-to-tail, overview rail, focus
// restoration, and destruction ordering.

#include "log_session.hpp"
#include "log_session_view.hpp"

#include <QObject>
#include <QTest>

#include <memory>

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
};

QTEST_MAIN(LogSessionViewTest)
#include "log_session_view_test.moc"
