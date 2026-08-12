// Dock binding / rebinding tests (task 1.9). Phase 1 seeds a
// minimal binary so `test/app/CMakeLists.txt` and `ctest` register
// the target; Phase 5 grows this file to cover repeated bind,
// unbind, hidden refresh, stale timers, duplicate connections,
// model reset, previous-session destruction, and originating-
// session edit rules across the Find, Parse Errors, Anchors,
// Histogram, and Record Details docks.

#include "anchor_manager.hpp"
#include "highlight_rule_set.hpp"
#include "log_filter_model.hpp"
#include "log_model.hpp"
#include "log_session.hpp"
#include "log_session_view.hpp"
#include "row_order_proxy_model.hpp"
#include "session_bind_context.hpp"

#include <QItemSelectionModel>
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
        QVERIFY(context.rowOrderProxy.isNull());
        QVERIFY(context.filterProxy.isNull());
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

    // -----------------------------------------------------------------
    // Bug review (post-review): `IsBound()` was documented as "every
    // session-owned pointer resolves to a live object" but the check
    // originally skipped `rowOrderProxy` and `filterProxy`, so a
    // Phase-5 dock reading `context.filterProxy` after a passing
    // `IsBound()` gate could deref a null `QPointer`. Fix: pin each
    // of the seven session-owned pointers by populating a fully-live
    // context and independently nulling one field at a time. Any
    // future field addition that omits an `isNull()` clause in
    // `IsBound()` will fail this pin instead of surfacing as a null
    // deref inside a dock body.
    // -----------------------------------------------------------------

    static void TestIsBoundGatesOnEverySessionOwnedField()
    {
        auto session = std::make_unique<LogSession>();
        auto view = std::make_unique<LogSessionView>(session.get());

        // Small helper that repopulates every session-owned pointer
        // from the live session/view. Called between "null one
        // field" checks so each iteration starts from a fully-
        // bound context. Repopulating from the live pointees --
        // rather than saving a `QPointer` copy and restoring it --
        // avoids a clang-analyzer false-positive on
        // `~QWeakPointer` that fires when a local `QPointer` copy
        // goes out of scope inside an inner block.
        auto populate = [&](SessionBindContext &ctx) {
            ctx.session = session.get();
            ctx.view = view.get();
            ctx.model = session->Model();
            ctx.rowOrderProxy = session->RowOrderProxy();
            ctx.filterProxy = session->FilterProxy();
            ctx.anchors = session->Anchors();
            ctx.highlights = session->Highlights();
        };

        SessionBindContext context;
        populate(context);
        QVERIFY(context.IsBound());

        // Null each session-owned field in turn and re-populate
        // from the live pointees. Every clause of `IsBound()`
        // needs to trip for the corresponding field null.
        context.session = nullptr;
        QVERIFY2(!context.IsBound(), "IsBound() should gate on `session`");
        populate(context);

        context.view = nullptr;
        QVERIFY2(!context.IsBound(), "IsBound() should gate on `view`");
        populate(context);

        context.model = nullptr;
        QVERIFY2(!context.IsBound(), "IsBound() should gate on `model`");
        populate(context);

        context.rowOrderProxy = nullptr;
        QVERIFY2(!context.IsBound(), "IsBound() should gate on `rowOrderProxy`");
        populate(context);

        context.filterProxy = nullptr;
        QVERIFY2(!context.IsBound(), "IsBound() should gate on `filterProxy`");
        populate(context);

        context.anchors = nullptr;
        QVERIFY2(!context.IsBound(), "IsBound() should gate on `anchors`");
        populate(context);

        context.highlights = nullptr;
        QVERIFY2(!context.IsBound(), "IsBound() should gate on `highlights`");
        populate(context);

        // `selection` and `theme` are intentionally NOT part of the
        // gate (see the docstring on `IsBound()`); pin that too so
        // a future change that tightens the gate is caught here.
        QVERIFY(context.IsBound());
        context.selection = nullptr;
        context.theme = nullptr;
        QVERIFY2(
            context.IsBound(),
            "IsBound() must remain true when only view-owned `selection` and window-owned `theme` are null"
        );
    }
};

QTEST_MAIN(DockBindingTest)
#include "dock_binding_test.moc"
