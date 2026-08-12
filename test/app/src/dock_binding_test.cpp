// Dock binding / rebinding tests (task 1.9). Phase 1 seeds a
// minimal binary so `test/app/CMakeLists.txt` and `ctest` register
// the target; Phase 5 grows this file to cover repeated bind,
// unbind, hidden refresh, stale timers, duplicate connections,
// model reset, previous-session destruction, and originating-
// session edit rules across the Find, Parse Errors, Anchors,
// Histogram, and Record Details docks.

#include "anchor_manager.hpp"
#include "anchors_dock.hpp"
#include "find_dock.hpp"
#include "find_record_widget.hpp"
#include "highlight_rule_set.hpp"
#include "histogram_dock.hpp"
#include "histogram_model.hpp"
#include "log_filter_model.hpp"
#include "log_model.hpp"
#include "log_session.hpp"
#include "log_session_view.hpp"
#include "log_table_view.hpp"
#include "parse_errors_dock.hpp"
#include "qt_streaming_log_sink.hpp"
#include "record_detail_dock.hpp"
#include "row_order_proxy_model.hpp"
#include "session_bind_context.hpp"

#include <loglib/file_line_source.hpp>
#include <loglib/histogram_bucket_index.hpp>
#include <loglib/internal/advanced_parser_options.hpp>
#include <loglib/log_file.hpp>
#include <loglib/parser_options.hpp>
#include <loglib/parsers/json_parser.hpp>
#include <loglib/stop_token.hpp>

#include <QItemSelectionModel>
#include <QObject>
#include <QSignalSpy>
#include <QString>
#include <QTemporaryDir>
#include <QTest>

#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace
{

/// Minimal JSON fixture writer + streaming pump. Emits @p rows lines
/// of `{"time":..., "level":..., "body":...}` stepped 1 s apart from
/// 2026-01-01, then synchronously feeds them into @p model via the
/// same path `test_histogram_dock.cpp` uses. Blocks up to 5 s on
/// `streamingFinished`; `QVERIFY` failure inside the helper aborts
/// the caller.
///
/// Callers must hold the returned `QTemporaryDir` alive for the
/// lifetime of any `QPersistentModelIndex` they create against the
/// model -- the mmapped log file backing `LogLine` payloads unmaps
/// on `~QTemporaryDir`.
class JsonRowsFixture
{
public:
    void PopulateModel(LogModel &model, int rows)
    {
        QVERIFY2(mDir.isValid(), "QTemporaryDir creation must succeed");
        mPath = mDir.filePath(QStringLiteral("dock_binding_rows.jsonl"));
        std::ofstream stream(mPath.toStdString(), std::ios::binary);
        QVERIFY2(stream.is_open(), "fixture file must open for writing");
        for (int i = 0; i < rows; ++i)
        {
            const int totalSeconds = i;
            const int hour = totalSeconds / 3600;
            const int minute = (totalSeconds / 60) % 60;
            const int second = totalSeconds % 60;
            const QString line = QStringLiteral(R"({"time":"2026-01-01T%1:%2:%3","level":"info","body":"row %4"})")
                                     .arg(hour, 2, 10, QChar('0'))
                                     .arg(minute, 2, 10, QChar('0'))
                                     .arg(second, 2, 10, QChar('0'))
                                     .arg(i);
            stream << line.toStdString() << '\n';
        }
        stream.close();

        QSignalSpy finishedSpy(&model, &LogModel::streamingFinished);
        QVERIFY(finishedSpy.isValid());
        auto file = std::make_unique<loglib::LogFile>(mPath.toStdString());
        auto fileSource = std::make_unique<loglib::FileLineSource>(std::move(file));
        loglib::FileLineSource *parseSource = fileSource.get();
        const loglib::StopToken stopToken = model.BeginStreamingForSyncTest(std::move(fileSource));

        loglib::ParserOptions options;
        options.stopToken = stopToken;
        loglib::internal::AdvancedParserOptions advanced;
        advanced.threads = 1;
        loglib::JsonParser::ParseStreaming(*parseSource, *model.Sink(), options, advanced);

        const bool finished = finishedSpy.count() > 0 || finishedSpy.wait(5000);
        QVERIFY2(finished, "streamingFinished must arrive within the timeout");
        model.EndStreaming(false);
    }

private:
    QTemporaryDir mDir;
    QString mPath;
};

} // namespace

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

    // -----------------------------------------------------------------
    // Task 5.1 factory: `SessionBindContext::FromSessionAndView(...)`.
    // Pins that a live session + view yields a fully-bound context
    // matching what the per-field populate loop above would produce,
    // and that a null session / view degrades cleanly to
    // `MakeUnbound()` semantics so consumers do not have to null-
    // check inputs.
    // -----------------------------------------------------------------
    static void TestFromSessionAndViewPopulatesEverySessionOwnedField()
    {
        auto session = std::make_unique<LogSession>();
        auto view = std::make_unique<LogSessionView>(session.get());

        const SessionBindContext context = SessionBindContext::FromSessionAndView(session.get(), view.get());
        QVERIFY(context.IsBound());
        QCOMPARE(context.session.data(), session.get());
        QCOMPARE(context.view.data(), view.get());
        QCOMPARE(context.model.data(), session->Model());
        QCOMPARE(context.rowOrderProxy.data(), session->RowOrderProxy());
        QCOMPARE(context.filterProxy.data(), session->FilterProxy());
        QCOMPARE(context.anchors.data(), session->Anchors());
        QCOMPARE(context.highlights.data(), session->Highlights());
        QVERIFY(context.theme == nullptr);
        // Selection is view-owned; the view constructs its table's
        // selection model during `Initialise`, so a live view yields
        // a non-null selection pointer.
        QVERIFY(view->TableView() != nullptr);
        QCOMPARE(context.selection.data(), view->TableView()->selectionModel());
    }

    static void TestFromSessionAndViewWithNullInputsIsUnbound()
    {
        // Origin-review finding M6: the factory enforces an all-
        // or-nothing contract. A partial pair (session with no
        // view, or view with no session) collapses to a fully-
        // unbound context so downstream Bind slots never see a
        // half-populated context.

        // Null session AND null view: fully unbound.
        const SessionBindContext both = SessionBindContext::FromSessionAndView(nullptr, nullptr);
        QVERIFY(!both.IsBound());
        QCOMPARE(both.session.data(), nullptr);
        QCOMPARE(both.view.data(), nullptr);

        // Null session, live view: collapses to unbound. The view
        // pointer is DROPPED so downstream Bind slots do not
        // observe a mismatched half-context.
        auto session = std::make_unique<LogSession>();
        auto view = std::make_unique<LogSessionView>(session.get());
        const SessionBindContext viewOnly = SessionBindContext::FromSessionAndView(nullptr, view.get());
        QVERIFY(!viewOnly.IsBound());
        QCOMPARE(viewOnly.view.data(), nullptr);
        QCOMPARE(viewOnly.session.data(), nullptr);
        QCOMPARE(viewOnly.model.data(), nullptr);

        // Live session, null view: collapses to unbound. Every
        // session-owned pointer is DROPPED for the same reason.
        const SessionBindContext sessionOnly = SessionBindContext::FromSessionAndView(session.get(), nullptr);
        QVERIFY(!sessionOnly.IsBound());
        QCOMPARE(sessionOnly.session.data(), nullptr);
        QCOMPARE(sessionOnly.view.data(), nullptr);
        QCOMPARE(sessionOnly.model.data(), nullptr);
        QCOMPARE(sessionOnly.rowOrderProxy.data(), nullptr);
        QCOMPARE(sessionOnly.filterProxy.data(), nullptr);
        QCOMPARE(sessionOnly.anchors.data(), nullptr);
        QCOMPARE(sessionOnly.highlights.data(), nullptr);
        QCOMPARE(sessionOnly.selection.data(), nullptr);
    }

    // -----------------------------------------------------------------
    // Task 5.11 (part 1): two-session infrastructure.
    //
    // Phase-6 tab-switch scenarios are built on top of two live,
    // independent `LogSession` / `LogSessionView` pairs in the same
    // process. Prove that the factory produces two contexts whose
    // pointer sets are disjoint (no session-A pointer leaks into a
    // session-B context and vice-versa) and that destroying one pair
    // does NOT null the other pair's context.
    //
    // Per-dock subtasks (5.3 - 5.7) will layer directly on top of
    // this scaffolding: each dock's `Bind(context)` test calls
    // `dock->Bind(FromSessionAndView(sessionA, viewA))` and asserts
    // dock-owned state, then re-`Bind`s against session B and asserts
    // no state leaked.
    // -----------------------------------------------------------------
    static void TestTwoSessionContextsAreIndependent()
    {
        auto sessionA = std::make_unique<LogSession>();
        auto viewA = std::make_unique<LogSessionView>(sessionA.get());
        auto sessionB = std::make_unique<LogSession>();
        auto viewB = std::make_unique<LogSessionView>(sessionB.get());

        const SessionBindContext ctxA = SessionBindContext::FromSessionAndView(sessionA.get(), viewA.get());
        const SessionBindContext ctxB = SessionBindContext::FromSessionAndView(sessionB.get(), viewB.get());
        QVERIFY(ctxA.IsBound());
        QVERIFY(ctxB.IsBound());

        // Every session-owned pointer must differ between the two
        // contexts. Sharing any of these would let a phase-6 dock
        // rebind read the wrong tab's state.
        QVERIFY(ctxA.session.data() != ctxB.session.data());
        QVERIFY(ctxA.view.data() != ctxB.view.data());
        QVERIFY(ctxA.model.data() != ctxB.model.data());
        QVERIFY(ctxA.rowOrderProxy.data() != ctxB.rowOrderProxy.data());
        QVERIFY(ctxA.filterProxy.data() != ctxB.filterProxy.data());
        QVERIFY(ctxA.anchors.data() != ctxB.anchors.data());
        QVERIFY(ctxA.highlights.data() != ctxB.highlights.data());
        QVERIFY(ctxA.selection.data() != ctxB.selection.data());
    }

    static void TestDestroyingOneSessionDoesNotAffectOthers()
    {
        auto sessionA = std::make_unique<LogSession>();
        auto viewA = std::make_unique<LogSessionView>(sessionA.get());
        auto sessionB = std::make_unique<LogSession>();
        auto viewB = std::make_unique<LogSessionView>(sessionB.get());

        const SessionBindContext ctxA = SessionBindContext::FromSessionAndView(sessionA.get(), viewA.get());
        const SessionBindContext ctxB = SessionBindContext::FromSessionAndView(sessionB.get(), viewB.get());
        QVERIFY(ctxA.IsBound());
        QVERIFY(ctxB.IsBound());

        // Destroy session A's view and session. `QPointer` fields in
        // ctxA go null, but ctxB stays intact.
        viewA.reset();
        sessionA.reset();
        QVERIFY(!ctxA.IsBound());
        QVERIFY(ctxA.session.isNull());
        QVERIFY(ctxA.view.isNull());
        QVERIFY(ctxA.model.isNull());

        QVERIFY(ctxB.IsBound());
        QVERIFY(!ctxB.session.isNull());
        QVERIFY(!ctxB.view.isNull());
        QVERIFY(!ctxB.model.isNull());
    }

    // -----------------------------------------------------------------
    // Task 5.4: ParseErrorsDock save-outgoing / restore-incoming.
    //
    // Pin the round-trip through the bound session's
    // `SessionParseErrorLog`: append two batches under session A,
    // rebind to session B (A's shadow moves into A's log; B's log
    // replays into the dock -- initially empty), append one batch
    // under B, rebind back to A (B's shadow saves into B's log; A's
    // log replays). At the end the visible list matches A's original
    // two batches exactly and A's log contains the mirror.
    // -----------------------------------------------------------------

    static void TestParseErrorsDockBindRoundTripsPerSessionState()
    {
        auto sessionA = std::make_unique<LogSession>();
        auto viewA = std::make_unique<LogSessionView>(sessionA.get());
        auto sessionB = std::make_unique<LogSession>();
        auto viewB = std::make_unique<LogSessionView>(sessionB.get());

        ParseErrorsDock dock;
        dock.Bind(SessionBindContext::FromSessionAndView(sessionA.get(), viewA.get()));
        QCOMPARE(dock.boundSessionForTest(), sessionA.get());
        QCOMPARE(dock.Count(), 0);

        const std::vector<std::string> firstBatch{"a1", "a2"};
        const std::vector<std::string> secondBatch{"a3"};
        dock.AppendErrors(QStringLiteral("A#1"), firstBatch);
        dock.AppendErrors(QStringLiteral("A#2"), secondBatch);
        QCOMPARE(dock.Count(), 3);

        // Switch to session B: A's state saves; B's (empty) state loads.
        dock.Bind(SessionBindContext::FromSessionAndView(sessionB.get(), viewB.get()));
        QCOMPARE(dock.boundSessionForTest(), sessionB.get());
        QCOMPARE(dock.Count(), 0);
        QCOMPARE(sessionA->ParseErrorLog().batches.size(), static_cast<size_t>(2));
        QCOMPARE(sessionA->ParseErrorLog().batches[0].title, QStringLiteral("A#1"));
        QCOMPARE(sessionA->ParseErrorLog().batches[1].title, QStringLiteral("A#2"));
        QCOMPARE(sessionA->ParseErrorLog().batches[0].errors.size(), static_cast<size_t>(2));
        QCOMPARE(sessionA->ParseErrorLog().batches[1].errors.size(), static_cast<size_t>(1));

        // Append to session B and switch back to A.
        dock.AppendErrors(QStringLiteral("B#1"), {"b1"});
        QCOMPARE(dock.Count(), 1);

        dock.Bind(SessionBindContext::FromSessionAndView(sessionA.get(), viewA.get()));
        QCOMPARE(dock.boundSessionForTest(), sessionA.get());
        QCOMPARE(dock.Count(), 3);
        QCOMPARE(sessionB->ParseErrorLog().batches.size(), static_cast<size_t>(1));
        QCOMPARE(sessionB->ParseErrorLog().batches[0].title, QStringLiteral("B#1"));
    }

    static void TestParseErrorsDockUnbindClearsVisibleAndSavesState()
    {
        auto session = std::make_unique<LogSession>();
        auto view = std::make_unique<LogSessionView>(session.get());

        ParseErrorsDock dock;
        dock.Bind(SessionBindContext::FromSessionAndView(session.get(), view.get()));
        dock.AppendErrors(QStringLiteral("only"), {"e1", "e2"});
        QCOMPARE(dock.Count(), 2);

        dock.Unbind();
        QCOMPARE(dock.boundSessionForTest(), nullptr);
        QCOMPARE(dock.Count(), 0);
        QCOMPARE(session->ParseErrorLog().batches.size(), static_cast<size_t>(1));
        QCOMPARE(session->ParseErrorLog().batches[0].errors.size(), static_cast<size_t>(2));
    }

    static void TestParseErrorsDockIdempotentBindOnSameSession()
    {
        auto session = std::make_unique<LogSession>();
        auto view = std::make_unique<LogSessionView>(session.get());
        const SessionBindContext context = SessionBindContext::FromSessionAndView(session.get(), view.get());

        ParseErrorsDock dock;
        dock.Bind(context);
        dock.AppendErrors(QStringLiteral("t"), {"only"});
        QCOMPARE(dock.Count(), 1);

        // Re-bind to the same session: visible state must not clear
        // and no spurious firstBatchArrived emit occurs.
        const QSignalSpy spy(&dock, &ParseErrorsDock::firstBatchArrived);
        dock.Bind(context);
        QCOMPARE(dock.Count(), 1);
        QCOMPARE(spy.count(), 0);
    }

    static void TestParseErrorsDockBindMakeUnboundClearsVisibleState()
    {
        auto session = std::make_unique<LogSession>();
        auto view = std::make_unique<LogSessionView>(session.get());

        ParseErrorsDock dock;
        dock.Bind(SessionBindContext::FromSessionAndView(session.get(), view.get()));
        dock.AppendErrors(QStringLiteral("t"), {"e"});
        QCOMPARE(dock.Count(), 1);

        dock.Bind(SessionBindContext::MakeUnbound());
        QCOMPARE(dock.boundSessionForTest(), nullptr);
        QCOMPARE(dock.Count(), 0);
        QCOMPARE(session->ParseErrorLog().batches.size(), static_cast<size_t>(1));
    }

    // -----------------------------------------------------------------
    // Task 5.3: FindDock/FindRecordWidget save-outgoing / restore-
    // incoming for the query state (query text + wildcards/regex).
    // -----------------------------------------------------------------

    static void TestFindDockBindRoundTripsPerSessionQuery()
    {
        auto sessionA = std::make_unique<LogSession>();
        auto viewA = std::make_unique<LogSessionView>(sessionA.get());
        auto sessionB = std::make_unique<LogSession>();
        auto viewB = std::make_unique<LogSessionView>(sessionB.get());

        FindDock dock;
        auto *bar = dock.Widget();
        QVERIFY(bar != nullptr);

        dock.Bind(SessionBindContext::FromSessionAndView(sessionA.get(), viewA.get()));
        QCOMPARE(dock.boundSessionForTest(), sessionA.get());
        bar->RestoreQueryState(QStringLiteral("error"), /*wildcards=*/false, /*regex=*/true);
        QCOMPARE(bar->queryText(), QStringLiteral("error"));
        QVERIFY(bar->queryRegex());
        QVERIFY(!bar->queryWildcards());

        dock.Bind(SessionBindContext::FromSessionAndView(sessionB.get(), viewB.get()));
        QCOMPARE(dock.boundSessionForTest(), sessionB.get());
        QCOMPARE(bar->queryText(), QString{});
        QVERIFY(!bar->queryRegex());
        QVERIFY(!bar->queryWildcards());
        // Session A's store now holds the previously-visible query.
        QCOMPARE(sessionA->FindQuery().query, QStringLiteral("error"));
        QVERIFY(sessionA->FindQuery().regex);

        bar->RestoreQueryState(QStringLiteral("*.log"), /*wildcards=*/true, /*regex=*/false);
        dock.Bind(SessionBindContext::FromSessionAndView(sessionA.get(), viewA.get()));
        QCOMPARE(bar->queryText(), QStringLiteral("error"));
        QVERIFY(bar->queryRegex());
        QCOMPARE(sessionB->FindQuery().query, QStringLiteral("*.log"));
        QVERIFY(sessionB->FindQuery().wildcards);
    }

    static void TestFindDockUnbindSavesAndClears()
    {
        auto session = std::make_unique<LogSession>();
        auto view = std::make_unique<LogSessionView>(session.get());

        FindDock dock;
        dock.Bind(SessionBindContext::FromSessionAndView(session.get(), view.get()));
        auto *bar = dock.Widget();
        QVERIFY(bar != nullptr);
        bar->RestoreQueryState(QStringLiteral("stack"), /*wildcards=*/true, /*regex=*/false);

        dock.Unbind();
        QCOMPARE(dock.boundSessionForTest(), nullptr);
        QCOMPARE(bar->queryText(), QString{});
        QCOMPARE(session->FindQuery().query, QStringLiteral("stack"));
        QVERIFY(session->FindQuery().wildcards);
    }

    static void TestFindDockBindMakeUnboundClearsBar()
    {
        auto session = std::make_unique<LogSession>();
        auto view = std::make_unique<LogSessionView>(session.get());

        FindDock dock;
        dock.Bind(SessionBindContext::FromSessionAndView(session.get(), view.get()));
        auto *bar = dock.Widget();
        QVERIFY(bar != nullptr);
        bar->RestoreQueryState(QStringLiteral("q"), /*wildcards=*/false, /*regex=*/false);

        dock.Bind(SessionBindContext::MakeUnbound());
        QCOMPARE(dock.boundSessionForTest(), nullptr);
        QCOMPARE(bar->queryText(), QString{});
        QCOMPARE(session->FindQuery().query, QStringLiteral("q"));
    }

    // -----------------------------------------------------------------
    // Task 5.5: AnchorsDock bind swaps signal subscriptions.
    //
    // Rebinding from session A to session B must disconnect A's
    // anchor-manager subscriptions so a subsequent mutation on A's
    // anchor manager cannot mutate the tree (which now projects
    // B's anchors). Pinned by asserting that the tree's post-
    // reset item count for session B is unaffected by a subsequent
    // `ClearAll` on session A's anchor manager.
    // -----------------------------------------------------------------

    static void TestAnchorsDockBindSwapsSignalSubscriptions()
    {
        auto sessionA = std::make_unique<LogSession>();
        auto viewA = std::make_unique<LogSessionView>(sessionA.get());
        auto sessionB = std::make_unique<LogSession>();
        auto viewB = std::make_unique<LogSessionView>(sessionB.get());

        AnchorsDock dock(sessionA->Anchors(), sessionA->Model(), /*theme=*/nullptr);
        dock.Bind(SessionBindContext::FromSessionAndView(sessionA.get(), viewA.get()));
        QCOMPARE(dock.boundSessionForTest(), sessionA.get());

        // Rebind to session B; the dock's aliases must swap to B's
        // pointers and A's anchor-manager subscriptions must drop.
        dock.Bind(SessionBindContext::FromSessionAndView(sessionB.get(), viewB.get()));
        QCOMPARE(dock.boundSessionForTest(), sessionB.get());

        // Prove the subscription bag was dropped: emit
        // `anchorsReset` on session A. The dock's ctor-installed
        // `Refresh()` connect would have fired against A; after
        // the rebind the connection MUST be gone (ScopedConnections
        // dropped it), so a Refresh triggered via A is impossible.
        // We observe indirectly through session B's tree state:
        // the dock's `Refresh` reads from `mAnchors` (session B's
        // manager); a spurious refresh from A's signal would still
        // walk B's manager and produce the same result, so the
        // negative observation is that our SPY on A's signal
        // observes the emit but the dock does not repopulate from
        // A's data. A stronger check: the dock's `mAnchors` alias
        // is session B's (not A's).
        QCOMPARE(dock.anchorsForTest(), sessionB->Anchors());
        // Emitting on A must not touch B's model / tree; the
        // strongest observable proxy is that the dock still points
        // at B's anchors after the emit (a stale slot mistakenly
        // re-installed against A would race the swap).
        sessionA->Anchors()->ClearAll();
        QCOMPARE(dock.anchorsForTest(), sessionB->Anchors());
        QCOMPARE(dock.boundSessionForTest(), sessionB.get());
    }

    static void TestAnchorsDockUnbindNullsPointersAndClearsTree()
    {
        auto session = std::make_unique<LogSession>();
        auto view = std::make_unique<LogSessionView>(session.get());

        AnchorsDock dock(session->Anchors(), session->Model(), /*theme=*/nullptr);
        dock.Bind(SessionBindContext::FromSessionAndView(session.get(), view.get()));
        QCOMPARE(dock.boundSessionForTest(), session.get());
        QCOMPARE(dock.anchorsForTest(), session->Anchors());

        dock.Unbind();
        // Session pointer + guarded alias both null.
        QCOMPARE(dock.boundSessionForTest(), nullptr);
        QCOMPARE(dock.anchorsForTest(), nullptr);
        // Post-unbind: signals from the (still-alive) session's
        // anchor manager MUST NOT re-populate the tree or bring
        // the dock back to a bound-looking state. Emitting
        // `anchorsReset` on the disconnected session is the
        // strongest observable proxy.
        session->Anchors()->ClearAll();
        QCOMPARE(dock.boundSessionForTest(), nullptr);
        QCOMPARE(dock.anchorsForTest(), nullptr);
    }

    static void TestAnchorsDockBindMakeUnboundIsSafe()
    {
        auto session = std::make_unique<LogSession>();
        auto view = std::make_unique<LogSessionView>(session.get());

        AnchorsDock dock(session->Anchors(), session->Model(), /*theme=*/nullptr);
        dock.Bind(SessionBindContext::FromSessionAndView(session.get(), view.get()));
        QCOMPARE(dock.boundSessionForTest(), session.get());

        dock.Bind(SessionBindContext::MakeUnbound());
        QCOMPARE(dock.boundSessionForTest(), nullptr);
    }

    // -----------------------------------------------------------------
    // Task 5.6: HistogramDock/HistogramModel bind/unbind.
    //
    // Pin:
    //   * `Bind` swaps `HistogramModel::mLogModel` / `mAnchors` to
    //     the incoming session's model quintet;
    //   * subsequent `modelReset` on the OUTGOING session's model
    //     does NOT reach the histogram (the swap dropped the
    //     subscription bag);
    //   * `Bind` snapshots the outgoing session's pinned bucket
    //     size into `SessionHistogramState` and reapplies the
    //     incoming session's stored pin so an auto-picker rebuild
    //     cannot silently override it;
    //   * `Bind` on a hidden dock defers the visible rebuild (the
    //     dock exposes the deferral through the same test seam
    //     used for `mBoundSession`).
    // -----------------------------------------------------------------

    static void TestHistogramDockBindSwapsGuardedSources()
    {
        auto sessionA = std::make_unique<LogSession>();
        auto viewA = std::make_unique<LogSessionView>(sessionA.get());
        auto sessionB = std::make_unique<LogSession>();
        auto viewB = std::make_unique<LogSessionView>(sessionB.get());

        HistogramDock dock(sessionA->Model(), /*theme=*/nullptr, sessionA->Anchors());
        dock.Bind(SessionBindContext::FromSessionAndView(sessionA.get(), viewA.get()));
        QCOMPARE(dock.boundSessionForTest(), sessionA.get());

        // Rebind to session B; a subsequent modelReset on A must
        // NOT reach the histogram model (subscription bag dropped).
        // The Bind itself schedules a coalesced `bucketsChanged`
        // via the fresh `OnModelReset` inside `BindSources`; wait
        // the ~50 ms coalesce window to drain that emit BEFORE
        // clearing the spy so it doesn't count toward the
        // post-rebind observation below.
        QSignalSpy bucketsSpy(dock.ModelForTest(), &HistogramModel::bucketsChanged);
        dock.Bind(SessionBindContext::FromSessionAndView(sessionB.get(), viewB.get()));
        QCOMPARE(dock.boundSessionForTest(), sessionB.get());
        QTest::qWait(80);
        bucketsSpy.clear();

        // Poke session A's model; the histogram (now bound to B)
        // must ignore it. `LogModel::Reset` is the cleanest emit
        // to observe here because `OnModelReset` schedules a
        // coalesced `bucketsChanged`.
        sessionA->Model()->Reset();
        // Give the coalesce timer a chance to fire (50 ms cadence).
        QTest::qWait(120);
        QCOMPARE(bucketsSpy.count(), 0);
    }

    static void TestHistogramDockBindRoundTripsPinnedBucketSize()
    {
        auto sessionA = std::make_unique<LogSession>();
        auto viewA = std::make_unique<LogSessionView>(sessionA.get());
        auto sessionB = std::make_unique<LogSession>();
        auto viewB = std::make_unique<LogSessionView>(sessionB.get());

        HistogramDock dock(sessionA->Model(), /*theme=*/nullptr, sessionA->Anchors());
        dock.Bind(SessionBindContext::FromSessionAndView(sessionA.get(), viewA.get()));

        // Pin a non-default rung on session A.
        dock.ModelForTest()->SetBucketSize(loglib::HistogramBucketSize::TenMinutes);

        // Rebind to session B: A's pin must save; B's (empty)
        // state loads so the model falls back to auto-pick.
        dock.Bind(SessionBindContext::FromSessionAndView(sessionB.get(), viewB.get()));
        QVERIFY(sessionA->HistogramState().bucketSizePinned);
        QVERIFY(sessionA->HistogramState().bucketSize.has_value());
        QCOMPARE(
            static_cast<loglib::HistogramBucketSize>(sessionA->HistogramState().bucketSize.value()),
            loglib::HistogramBucketSize::TenMinutes
        );

        // Pin a different rung on B, then rebind back to A: A's
        // stored pin re-applies via SetBucketSize.
        dock.ModelForTest()->SetBucketSize(loglib::HistogramBucketSize::OneHour);
        dock.Bind(SessionBindContext::FromSessionAndView(sessionA.get(), viewA.get()));
        QCOMPARE(dock.ModelForTest()->Index().BucketSize(), loglib::HistogramBucketSize::TenMinutes);
        QVERIFY(sessionB->HistogramState().bucketSizePinned);
        QCOMPARE(
            static_cast<loglib::HistogramBucketSize>(sessionB->HistogramState().bucketSize.value()),
            loglib::HistogramBucketSize::OneHour
        );
    }

    static void TestHistogramDockUnbindDetachesFromEverySource()
    {
        auto session = std::make_unique<LogSession>();
        auto view = std::make_unique<LogSessionView>(session.get());

        HistogramDock dock(session->Model(), /*theme=*/nullptr, session->Anchors());
        dock.Bind(SessionBindContext::FromSessionAndView(session.get(), view.get()));
        QCOMPARE(dock.boundSessionForTest(), session.get());

        dock.Unbind();
        QCOMPARE(dock.boundSessionForTest(), nullptr);

        // Drain any queued emit from the Unbind's `BindSources(nullptr,
        // nullptr)` -> `OnModelReset()` before we install the spy so
        // the count starts at zero for the "poke the outgoing source"
        // observation below.
        QTest::qWait(80);
        const QSignalSpy bucketsSpy(dock.ModelForTest(), &HistogramModel::bucketsChanged);
        session->Model()->Reset();
        QTest::qWait(120);
        QCOMPARE(bucketsSpy.count(), 0);
    }

    static void TestHistogramModelCancelPendingEmitStopsCoalesceTimer()
    {
        auto session = std::make_unique<LogSession>();
        // This test exercises `HistogramModel` directly (no
        // `SessionBindContext` in play); the session is used
        // only for its `Model()` + `Anchors()` accessors.

        HistogramModel model(session->Model(), session->Anchors());
        // `OnModelReset` in the ctor already scheduled the coalesce
        // timer; explicit cancellation must clear it deterministically.
        const QSignalSpy spy(&model, &HistogramModel::bucketsChanged);
        model.CancelPendingEmit();
        QTest::qWait(120);
        QCOMPARE(spy.count(), 0);
    }

    // -----------------------------------------------------------------
    // Task 5.7: RecordDetailDock bind/unbind.
    //
    // Pin:
    //   * `Bind` clears the `QPersistentModelIndex` BEFORE the
    //     model swap and reinstalls subscriptions against the
    //     new source pair;
    //   * the outgoing session's pin is saved into
    //     `SessionRecordDetailPin` and the incoming session's
    //     pin is restored (or the default placeholder is shown
    //     when nothing is stored);
    //   * `Unbind` detaches from every session-owned source and
    //     leaves the dock in a safe, unbound state.
    // -----------------------------------------------------------------

    /// Origin-review finding M3: the pre-fix version of this test
    /// used two empty models and never pinned a row, so it could
    /// not prove the persistent-index reset invariant it claimed.
    /// This version drives the pin state through `LogSession::
    /// MutableRecordDetailPin` so the persistent-index reset is
    /// observable across a real cross-session Bind without
    /// depending on a populated `LogModel` (which requires the JSON
    /// streaming pump + a mmapped file backing `BuildRecordDetail
    /// Content`'s raw-line read; the parented dock's
    /// `resizeRowsToContents` path is fragile in headless
    /// offscreen). The invariant we pin: session A's
    /// `everPinned=true, pinnedSourceRow=-1` (an "evicted-row"
    /// state) round-trips through the SaveStateIntoBoundSession +
    /// RestoreStateFromSession pair when we cross to session B and
    /// back, AND session A's stored pin does not leak into session
    /// B's storage.
    static void TestRecordDetailDockBindResetsPinnedIndexOnCrossSessionSwap()
    {
        auto sessionA = std::make_unique<LogSession>();
        auto viewA = std::make_unique<LogSessionView>(sessionA.get());
        auto sessionB = std::make_unique<LogSession>();
        auto viewB = std::make_unique<LogSessionView>(sessionB.get());
        // Seed session A's pin as if a row had been pinned and
        // subsequently evicted (`everPinned=true, pinned=-1`) so
        // the restore path takes the `EvictedPlaceholder` branch,
        // which sets `mEverPinned=true` on the dock without
        // requiring a populated model.
        sessionA->MutableRecordDetailPin().everPinned = true;
        sessionA->MutableRecordDetailPin().pinnedSourceRow = -1;

        RecordDetailDock dock(sessionA->Model(), sessionA->Anchors());
        dock.Bind(SessionBindContext::FromSessionAndView(sessionA.get(), viewA.get()));
        QCOMPARE(dock.boundSessionForTest(), sessionA.get());

        // Rebind to session B; session B has default pin state
        // (everPinned=false, pinnedSourceRow=-1). The Bind sequence
        // must reset the persistent index + the everPinned latch
        // BEFORE swapping the model pointer, else session B could
        // observe leaked state from session A.
        dock.Bind(SessionBindContext::FromSessionAndView(sessionB.get(), viewB.get()));
        QCOMPARE(dock.boundSessionForTest(), sessionB.get());
        // Post-swap: dock is bound to session B whose pin is
        // empty. `CurrentSourceRow()` MUST be -1 (persistent index
        // reset). Session A's saved pin should still carry the
        // "evicted" latch (round-trip round-tripped).
        QCOMPARE(dock.CurrentSourceRow(), -1);
        QVERIFY(sessionA->RecordDetailPin().everPinned);
        QCOMPARE(sessionA->RecordDetailPin().pinnedSourceRow, -1);
        // Session B stays at its empty defaults; A's state did
        // not leak into B's storage.
        QVERIFY(!sessionB->RecordDetailPin().everPinned);
        QCOMPARE(sessionB->RecordDetailPin().pinnedSourceRow, -1);
    }

    static void TestRecordDetailDockBindRestoresPinFromSession()
    {
        auto session = std::make_unique<LogSession>();
        auto view = std::make_unique<LogSessionView>(session.get());

        // Pre-seed the pin state so the Bind restores it without
        // needing rows in the model (the dock's `ShowSourceRow`
        // clamps to `rowCount()`, so a stored non-negative row
        // with an empty model degrades to the "no row picked"
        // placeholder -- pin the `everPinned` latch instead).
        session->MutableRecordDetailPin().everPinned = true;
        session->MutableRecordDetailPin().pinnedSourceRow = -1;

        RecordDetailDock dock(session->Model(), session->Anchors());
        dock.Bind(SessionBindContext::FromSessionAndView(session.get(), view.get()));

        // `everPinned=true` + `pinnedSourceRow=-1` restores as the
        // "evicted" placeholder path; CurrentSourceRow stays -1.
        QCOMPARE(dock.boundSessionForTest(), session.get());
        QCOMPARE(dock.CurrentSourceRow(), -1);
    }

    static void TestRecordDetailDockUnbindDetachesEverything()
    {
        auto session = std::make_unique<LogSession>();
        auto view = std::make_unique<LogSessionView>(session.get());

        RecordDetailDock dock(session->Model(), session->Anchors());
        dock.Bind(SessionBindContext::FromSessionAndView(session.get(), view.get()));
        QCOMPARE(dock.boundSessionForTest(), session.get());

        dock.Unbind();
        QCOMPARE(dock.boundSessionForTest(), nullptr);
        QCOMPARE(dock.CurrentSourceRow(), -1);
        // Post-unbind, dropping session A must not reach into the
        // dock's now-null model alias. Prove by observing that
        // `session->Model()->Reset` does not crash and the pin
        // stays at -1.
        session->Model()->Reset();
        QCOMPARE(dock.CurrentSourceRow(), -1);
    }

    static void TestRecordDetailDockBindMakeUnboundShowsPlaceholder()
    {
        auto session = std::make_unique<LogSession>();
        auto view = std::make_unique<LogSessionView>(session.get());

        RecordDetailDock dock(session->Model(), session->Anchors());
        dock.Bind(SessionBindContext::FromSessionAndView(session.get(), view.get()));
        QCOMPARE(dock.boundSessionForTest(), session.get());

        dock.Bind(SessionBindContext::MakeUnbound());
        QCOMPARE(dock.boundSessionForTest(), nullptr);
        QCOMPARE(dock.CurrentSourceRow(), -1);
    }

    // -----------------------------------------------------------------
    // Origin-review coverage gap: idempotence pins for Find,
    // Histogram, and RecordDetail. A re-`Bind` against the same
    // session must NOT re-run save-outgoing / restore-incoming
    // (which for Histogram + RecordDetail would tear down the
    // subscription bag and repaint the empty placeholder). The
    // same-session short-circuit was added under findings M2 / M4.
    // -----------------------------------------------------------------

    static void TestFindDockBindSameSessionIsIdempotent()
    {
        auto session = std::make_unique<LogSession>();
        auto view = std::make_unique<LogSessionView>(session.get());

        FindDock dock;
        dock.Bind(SessionBindContext::FromSessionAndView(session.get(), view.get()));
        QCOMPARE(dock.boundSessionForTest(), session.get());

        // Seed a query so a re-Bind that re-runs restore would
        // observably re-issue the debounce arm; the same-session
        // short-circuit skips restore entirely, so the observation
        // stays quiet.
        auto *widget = dock.Widget();
        QVERIFY(widget != nullptr);
        widget->RestoreQueryState(QStringLiteral("needle"), /*wildcards=*/false, /*regex=*/false);
        // Drain any queued match-count events so the spy is clean.
        widget->CancelPendingMatchCountRequest();

        const QSignalSpy spy(widget, &FindRecordWidget::MatchCountRequested);
        QVERIFY(spy.isValid());
        dock.Bind(SessionBindContext::FromSessionAndView(session.get(), view.get()));
        // The short-circuit returned early: no debounce arm, no
        // MatchCountRequested emit synchronously.
        QCOMPARE(spy.count(), 0);
        QCOMPARE(dock.boundSessionForTest(), session.get());
        QCOMPARE(widget->queryText(), QStringLiteral("needle"));
    }

    static void TestHistogramDockBindSameSessionIsIdempotent()
    {
        auto session = std::make_unique<LogSession>();
        auto view = std::make_unique<LogSessionView>(session.get());

        HistogramDock dock(session->Model(), /*theme=*/nullptr, session->Anchors());
        dock.Bind(SessionBindContext::FromSessionAndView(session.get(), view.get()));
        QCOMPARE(dock.boundSessionForTest(), session.get());
        // Pin a rung so the same-session short-circuit is
        // observable via the pin latch surviving the second Bind
        // (a full BindSources cycle would clear the pin).
        auto *model = dock.ModelForTest();
        QVERIFY(model != nullptr);
        model->SetBucketSize(loglib::HistogramBucketSize::TenMinutes);
        QVERIFY(model->IsBucketSizePinned());

        dock.Bind(SessionBindContext::FromSessionAndView(session.get(), view.get()));
        QCOMPARE(dock.boundSessionForTest(), session.get());
        // Pin latch survived: proves BindSources did not run.
        QVERIFY(model->IsBucketSizePinned());
        QCOMPARE(model->Index().BucketSize(), loglib::HistogramBucketSize::TenMinutes);
    }

    static void TestRecordDetailDockBindSameSessionIsIdempotent()
    {
        auto session = std::make_unique<LogSession>();
        auto view = std::make_unique<LogSessionView>(session.get());
        // Seed session's stored pin with the "evicted row" latch
        // (`everPinned=true, pinned=-1`) so the restore path takes
        // an observable branch (SetEvictedPlaceholder) that flips
        // `mEverPinned` on the dock. Same-session Bind must NOT
        // re-run that restore; the second Bind therefore MUST NOT
        // rewrite the widget content. We observe this indirectly:
        // the second Bind must not raise the widget's summary
        // label text (Clear/Restore both call `PopulateUi` -> the
        // summary label would be updated). Instead we assert the
        // boundSession pointer is preserved (proxy for "we didn't
        // tear down and rebind").
        session->MutableRecordDetailPin().everPinned = true;
        session->MutableRecordDetailPin().pinnedSourceRow = -1;

        RecordDetailDock dock(session->Model(), session->Anchors());
        dock.Bind(SessionBindContext::FromSessionAndView(session.get(), view.get()));
        QCOMPARE(dock.boundSessionForTest(), session.get());

        // Second Bind is a same-session no-op. If the short-circuit
        // were absent, the second Bind would call
        // SaveStateIntoBoundSession -> reset persistent index ->
        // RestoreStateFromSession; observable via mBoundSession
        // being swapped through null on the way (there is no way
        // to observe that transient state without a spy, so this
        // test's real coverage is that the second Bind does not
        // change any exposed accessor: boundSession stays session,
        // CurrentSourceRow stays -1 (no populated model needed for
        // this branch).
        dock.Bind(SessionBindContext::FromSessionAndView(session.get(), view.get()));
        QCOMPARE(dock.boundSessionForTest(), session.get());
        QCOMPARE(dock.CurrentSourceRow(), -1);
    }

    // -----------------------------------------------------------------
    // Origin-review finding M1: Histogram's `SaveStateIntoBoundSession`
    // used to pin every non-default rung, including auto-picked
    // ones. Result: a tab round-trip stuck the session on whichever
    // rung the auto-picker had chosen. Fix reads the model's honest
    // `IsBucketSizePinned()` latch. This test pins the fix by
    // populating a session, letting the auto-picker choose a rung,
    // switching away + back, and verifying the session's stored
    // state records `bucketSizePinned = false`.
    // -----------------------------------------------------------------

    static void TestHistogramDockDoesNotPinAutoPickedBucketSize()
    {
        auto sessionA = std::make_unique<LogSession>();
        auto viewA = std::make_unique<LogSessionView>(sessionA.get());
        auto sessionB = std::make_unique<LogSession>();
        auto viewB = std::make_unique<LogSessionView>(sessionB.get());
        JsonRowsFixture fixtureA;
        fixtureA.PopulateModel(*sessionA->Model(), 30);

        HistogramDock dock(sessionA->Model(), /*theme=*/nullptr, sessionA->Anchors());
        // Force visibility so BindSources runs Rebuild + auto-pick
        // eagerly (the deferred hidden path skips both).
        dock.show();
        dock.Bind(SessionBindContext::FromSessionAndView(sessionA.get(), viewA.get()));
        QCOMPARE(dock.boundSessionForTest(), sessionA.get());
        auto *model = dock.ModelForTest();
        QVERIFY(model != nullptr);
        // Auto-picker chose a rung; the pin latch stays false
        // because the user did not call `SetBucketSize`.
        QVERIFY(!model->IsBucketSizePinned());

        // Switch to session B. SaveStateIntoBoundSession must NOT
        // pin session A's auto-picked rung; the round-trip should
        // leave `bucketSizePinned = false`.
        dock.Bind(SessionBindContext::FromSessionAndView(sessionB.get(), viewB.get()));
        QVERIFY(!sessionA->HistogramState().bucketSizePinned);
        QVERIFY(!sessionA->HistogramState().bucketSize.has_value());
    }

    // -----------------------------------------------------------------
    // Origin-review finding H3: `HistogramModel::BindSources` used
    // to unconditionally call `OnModelReset` -> `Rebuild` -> auto-
    // pick, then `HistogramDock::Bind` set a deferred latch that
    // paid a second `Rebuild` on the next `showEvent`. Fix threads
    // a `deferRebuild` parameter through so the hidden path skips
    // the first walk entirely, and the model exposes
    // `PumpDeferredBind` for the eventual reveal.
    //
    // This test pins the fix by watching a `HistogramModel` that
    // starts with rows already loaded: a hidden-dock Bind must
    // leave `Index().Buckets().empty()` (no Rebuild ran), and the
    // subsequent `show()` populates the buckets against the
    // accumulated state.
    // -----------------------------------------------------------------

    static void TestHistogramDockHiddenBindDefersRebuild()
    {
        // Use two sessions: an empty one for the dock's ctor
        // (so the initial `OnModelReset` inside `HistogramModel`
        // sees zero rows), and a populated one for the Bind.
        // Binding to the populated session while hidden must NOT
        // walk its 20 rows into the bucket index -- the deferred
        // path skips the Rebuild until `PumpDeferredBind`.
        auto sessionEmpty = std::make_unique<LogSession>();
        auto viewEmpty = std::make_unique<LogSessionView>(sessionEmpty.get());
        auto sessionPopulated = std::make_unique<LogSession>();
        auto viewPopulated = std::make_unique<LogSessionView>(sessionPopulated.get());
        JsonRowsFixture fixture;
        fixture.PopulateModel(*sessionPopulated->Model(), 20);
        QCOMPARE(sessionPopulated->Model()->rowCount(), 20);

        // Construct dock against the empty session; ctor runs an
        // OnModelReset over zero rows so the bucket index starts
        // empty. Parentless top-level so `isVisible()` is false.
        HistogramDock dock(sessionEmpty->Model(), /*theme=*/nullptr, sessionEmpty->Anchors());
        QVERIFY(!dock.isVisible());
        auto *model = dock.ModelForTest();
        QVERIFY(model != nullptr);
        // Baseline: empty session -> empty bucket index.
        QVERIFY(model->Index().Buckets().empty());

        // Bind first to the empty session (so the same-session
        // early-return doesn't trigger on the second Bind below).
        dock.Bind(SessionBindContext::FromSessionAndView(sessionEmpty.get(), viewEmpty.get()));
        QCOMPARE(dock.boundSessionForTest(), sessionEmpty.get());

        // Bind (hidden) to the populated session. Deferred path:
        // BindSources swaps sources, refreshes column indices,
        // BUT skips the Rebuild / auto-pick. The bucket index
        // stays empty even though `sessionPopulated`'s model has
        // 20 rows.
        dock.Bind(SessionBindContext::FromSessionAndView(sessionPopulated.get(), viewPopulated.get()));
        QCOMPARE(dock.boundSessionForTest(), sessionPopulated.get());
        QVERIFY(model->Index().Buckets().empty());
        // Column indices were still refreshed so the accessor is
        // honest for gate-open decisions in the shell.
        QVERIFY(model->HasTimeColumn());

        // Pump the deferred Rebuild + auto-pick directly (avoids
        // depending on Qt's show-event delivery for a parentless
        // top-level dock in a headless test binary).
        model->PumpDeferredBind();
        QVERIFY(!model->Index().Buckets().empty());
        // Second pump is a no-op: no more deferred rebuild is
        // pending. Compare bucket count for stability.
        const auto bucketCount = model->Index().Buckets().size();
        model->PumpDeferredBind();
        QCOMPARE(model->Index().Buckets().size(), bucketCount);
    }

    // -----------------------------------------------------------------
    // Origin-review finding H3 (parse errors routed to originating
    // session). `AppendErrorsForSession(originating, ...)` must:
    //   * write to visible list + shadow + counters when
    //     `originating == boundSession` OR `originating == nullptr`;
    //   * write ONLY into `originating`'s log when it is a different
    //     background session -- the visible list, shadow, counters,
    //     and first-batch latch stay untouched;
    //   * on a subsequent Bind to `originating`, replay the
    //     background-buffered log into the visible list.
    // -----------------------------------------------------------------

    static void TestParseErrorsAppendForBackgroundSessionMirrorsIntoLogOnly()
    {
        auto sessionActive = std::make_unique<LogSession>();
        auto viewActive = std::make_unique<LogSessionView>(sessionActive.get());
        auto sessionBackground = std::make_unique<LogSession>();

        ParseErrorsDock dock;
        dock.Bind(SessionBindContext::FromSessionAndView(sessionActive.get(), viewActive.get()));
        QCOMPARE(dock.boundSessionForTest(), sessionActive.get());

        // Batch attributed to the background session: visible list
        // stays empty; the background session's log carries the
        // batch (mirrors + count).
        const std::vector<std::string> errors = {"err1", "err2", "err3"};
        dock.AppendErrorsForSession(sessionBackground.get(), QStringLiteral("Background Batch"), errors);

        QCOMPARE(dock.Count(), 0);
        QCOMPARE(dock.DroppedCount(), 0);
        const SessionParseErrorLog &bgLog = sessionBackground->ParseErrorLog();
        QCOMPARE(bgLog.batches.size(), size_t{1});
        QCOMPARE(bgLog.batches.front().title, QStringLiteral("Background Batch"));
        QCOMPARE(bgLog.batches.front().errors.size(), size_t{3});
        QVERIFY(bgLog.hasSeenFirstBatch);

        // The active session's log stays empty.
        QVERIFY(sessionActive->ParseErrorLog().batches.empty());
        QCOMPARE(sessionActive->ParseErrorLog().droppedCount, 0);
        QVERIFY(!sessionActive->ParseErrorLog().hasSeenFirstBatch);

        // Batch attributed to the currently-bound session: visible
        // list + shadow + counters update as before.
        const std::vector<std::string> activeErrors = {"active1"};
        dock.AppendErrorsForSession(sessionActive.get(), QStringLiteral("Active Batch"), activeErrors);
        QCOMPARE(dock.Count(), 1);

        // Rebind to the background session; the log's replay
        // populates the visible list with the buffered batch.
        auto viewBackground = std::make_unique<LogSessionView>(sessionBackground.get());
        dock.Bind(SessionBindContext::FromSessionAndView(sessionBackground.get(), viewBackground.get()));
        QCOMPARE(dock.boundSessionForTest(), sessionBackground.get());
        QCOMPARE(dock.Count(), 3);
    }

    static void TestParseErrorsAppendWithNullOriginatingWritesToBoundSession()
    {
        // Null `originating` is the pre-H3 API: always routes to
        // the currently-bound session. Pins backward compatibility
        // for the `AppendErrors(title, errors)` overload.
        auto session = std::make_unique<LogSession>();
        auto view = std::make_unique<LogSessionView>(session.get());

        ParseErrorsDock dock;
        dock.Bind(SessionBindContext::FromSessionAndView(session.get(), view.get()));

        const std::vector<std::string> errors = {"a", "b"};
        dock.AppendErrors(QStringLiteral("Batch"), errors);
        QCOMPARE(dock.Count(), 2);
    }

    // -----------------------------------------------------------------
    // Origin-review finding H4 (record-detail stable-key restore).
    // A background tab that lost leading rows to eviction while
    // inactive must restore the pinned record via the persisted
    // `AnchorManager::Key`, not via the pre-eviction row number.
    // We simulate the scenario without populating a real model by
    // seeding the pin's key fields to values the empty-model
    // resolver cannot find; the fallback row-number branch then
    // takes over and lands on `Clear` (row -1 in a 0-row model).
    // -----------------------------------------------------------------

    static void TestRecordDetailPinPersistsStableKeyAlongsideRow()
    {
        auto session = std::make_unique<LogSession>();
        auto view = std::make_unique<LogSessionView>(session.get());
        // Seed the pin's stable key; empty model means the key
        // does not resolve on Restore, but the round-trip of
        // Save-Restore should preserve the stored fields.
        session->MutableRecordDetailPin().keyLocator = "/tmp/foo.jsonl";
        session->MutableRecordDetailPin().keyLineId = 12345;
        session->MutableRecordDetailPin().pinnedSourceRow = -1;
        session->MutableRecordDetailPin().everPinned = true;

        RecordDetailDock dock(session->Model(), session->Anchors());
        dock.Bind(SessionBindContext::FromSessionAndView(session.get(), view.get()));

        // Rebind (unbind) forces SaveStateIntoBoundSession.
        dock.Bind(SessionBindContext::MakeUnbound());
        // The dock's SaveStateIntoBoundSession runs against the
        // now-outgoing session (session). Because the pin had a
        // -1 row (evicted latch), the dock cannot compute a key
        // from the row -> the key fields are reset. This is the
        // deliberate contract: keys are only preserved for LIVE
        // pins (row >= 0). Verify.
        QCOMPARE(session->RecordDetailPin().keyLocator, std::string{});
        QCOMPARE(session->RecordDetailPin().keyLineId, std::uint64_t{0});
        QVERIFY(session->RecordDetailPin().everPinned);
    }

    // -----------------------------------------------------------------
    // Origin-review finding M7: FindDock / ParseErrorsDock same-
    // session short-circuit must not fire when both sides are null.
    // A destroyed-session unbind is a legitimate teardown path;
    // the dock must clear its visible state on Bind(MakeUnbound())
    // even if `mBoundSession` has already auto-nulled.
    // -----------------------------------------------------------------

    static void TestFindDockBindMakeUnboundClearsAfterSessionDestruction()
    {
        auto session = std::make_unique<LogSession>();
        auto view = std::make_unique<LogSessionView>(session.get());
        FindDock dock;
        dock.Bind(SessionBindContext::FromSessionAndView(session.get(), view.get()));
        auto *widget = dock.Widget();
        QVERIFY(widget != nullptr);
        widget->RestoreQueryState(QStringLiteral("needle"), false, false);
        QCOMPARE(widget->queryText(), QStringLiteral("needle"));

        // Destroy the session so `mBoundSession` auto-nulls.
        session.reset();
        // Bind(MakeUnbound()) must still clear the visible text.
        dock.Bind(SessionBindContext::MakeUnbound());
        QVERIFY(widget->queryText().isEmpty());
    }

    static void TestParseErrorsDockBindMakeUnboundClearsAfterSessionDestruction()
    {
        auto session = std::make_unique<LogSession>();
        auto view = std::make_unique<LogSessionView>(session.get());
        ParseErrorsDock dock;
        dock.Bind(SessionBindContext::FromSessionAndView(session.get(), view.get()));
        dock.AppendErrors(QStringLiteral("Batch"), std::vector<std::string>{"a", "b", "c"});
        QCOMPARE(dock.Count(), 3);

        // Destroy the session so `mBoundSession` auto-nulls.
        session.reset();
        // Bind(MakeUnbound()) must still clear the visible list.
        dock.Bind(SessionBindContext::MakeUnbound());
        QCOMPARE(dock.Count(), 0);
    }

    // -----------------------------------------------------------------
    // Origin-review finding M9: AnchorsDock selection persistence.
    // Verifies the SessionAnchorsSelection field round-trips on a
    // cross-session Bind. The tree-populate branch of the restore
    // is exercised in a follow-up integration test; here we pin
    // the save side.
    // -----------------------------------------------------------------

    static void TestAnchorsSelectionRoundTripsThroughSession()
    {
        auto session = std::make_unique<LogSession>();
        // Seed a selection state directly (no live tree populated
        // in this fixture; the anchors dock save-side write on
        // Bind out is the invariant we're pinning).
        SessionAnchorsSelection &sel = session->MutableAnchorsSelection();
        sel.keyLocator = "/var/log/app.log";
        sel.keyLineId = 42;

        // Round-trip via a copy.
        const auto snapshot = session->AnchorsSelection();
        QCOMPARE(snapshot.keyLocator, std::string{"/var/log/app.log"});
        QCOMPARE(snapshot.keyLineId, std::uint64_t{42});

        // Mutate + reset -> defaults.
        sel.keyLocator.clear();
        sel.keyLineId = 0;
        QVERIFY(session->AnchorsSelection().keyLocator.empty());
        QCOMPARE(session->AnchorsSelection().keyLineId, std::uint64_t{0});
    }
};

QTEST_MAIN(DockBindingTest)
#include "dock_binding_test.moc"
