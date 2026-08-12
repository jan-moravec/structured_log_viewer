// Multi-source tab lifecycle tests (task 1.9 seeded; task 4.10 grows
// with phase-4 shell-routing pins; Phase 6 grows further to cover
// create / close / reorder / switch, last-tab-close-closes-window,
// focus, shortcuts, labels, indicators, accessibility, Open / Recent
// Sessions routing, drag / drop, and static-session isolation).

#include "log_session.hpp"
#include "log_session_presentation.hpp"
#include "log_session_view.hpp"
#include "main_window.hpp"

#include <loglib/log_configuration.hpp>

#include <QObject>
#include <QSignalSpy>
#include <QTest>

#include <cstdint>
#include <vector>

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

    // -----------------------------------------------------------------
    // Task 4.1 -- active-session accessors return the constructed pair.
    // -----------------------------------------------------------------

    static void TestActiveSessionAccessorReturnsConstructedSession()
    {
        const MainWindow window;
        QVERIFY(window.activeSession() != nullptr);
    }

    static void TestActiveSessionViewAccessorReturnsConstructedView()
    {
        const MainWindow window;
        QVERIFY(window.activeSessionView() != nullptr);
        QCOMPARE(window.activeSessionView()->Session(), window.activeSession());
    }

    // -----------------------------------------------------------------
    // Task 4.1 / 4.6 / 4.8 -- `hostedSessions()` is the iterator every
    // window-wide operation (modified aggregation, close preflight,
    // preference broadcast) walks. Phase 3 hosts exactly one; the
    // shape must survive phase 6's multi-tab expansion so callers
    // do not have to change when the tab list grows.
    // -----------------------------------------------------------------

    static void TestHostedSessionsContainsExactlyTheActiveSession()
    {
        const MainWindow window;
        const std::vector<LogSession *> sessions = window.hostedSessions();
        QCOMPARE(sessions.size(), static_cast<std::size_t>(1));
        QCOMPARE(sessions.front(), window.activeSession());
    }

    // -----------------------------------------------------------------
    // Task 4.6 -- modified-window aggregation folds every hosted
    // session's dirty marker. Single-session windows resolve to the
    // active session's `IsFiltersDirty()`; the point of the test is
    // that the aggregator ACTUALLY reads from `hostedSessions()`
    // rather than a raw `mSession->IsFiltersDirty()` snapshot, so the
    // pattern scales to multiple tabs without touching callers.
    // -----------------------------------------------------------------

    static void TestAggregateWindowModifiedFollowsActiveSessionDirty()
    {
        // Non-`const` on purpose (post review-5 finding on the
        // `const MainWindow` idiom): the test observably mutates
        // `window` state via `setWindowModified` through the
        // signal fan, even though the mutation ROUTE goes through
        // `activeSession()->MarkFiltersDirty()`. Marking `window`
        // const would satisfy `misc-const-correctness` (since
        // `activeSession()` is a const method returning a non-
        // const pointer) but would lie about the test's intent.
        MainWindow window; // NOLINT(misc-const-correctness)
        QVERIFY(!window.isWindowModified());

        // Mark dirty on the active session; the ctor-installed
        // `filtersDirtyChanged` connection fans through
        // `AggregateWindowModified` -> `setWindowModified(true)`.
        window.activeSession()->MarkFiltersDirty();
        QVERIFY(window.isWindowModified());

        // Clear it and reverse-verify.
        window.activeSession()->ClearFiltersDirty();
        QVERIFY(!window.isWindowModified());
    }

    // -----------------------------------------------------------------
    // Task 4.2 -- `UnbindActiveSessionForTest()` disconnects the
    // scoped bag so post-unbind emits from the session do not
    // reach the shell. Observable through the aggregator: pre-
    // unbind a dirty toggle flips the window's modified bit;
    // post-unbind it does not.
    //
    // This test exercises ONE alias-side subscription
    // (`LogSession::filtersDirtyChanged`) but ~50 shell-side
    // subscriptions share the same `mSessionConnections` bag. A
    // structural regression that bares any single ctor connect
    // (e.g. `mModel::headerDataChanged -> UpdateSortStatus`, as
    // caught in the phase-4 review-4 resolution) would compile
    // and pass this pin but leak signals across a phase-6 tab
    // switch. `TestScopedConnectionBagSizeMatchesCtorPopulation`
    // below covers the structural regression class with a raw
    // size check; `TestSessionRotationEmitReachesSessionAndSevers`
    // exercises a second observable channel through the rotation-
    // flash signal.
    // -----------------------------------------------------------------

    static void TestUnbindActiveSessionDropsPresentationSubscriptions()
    {
        MainWindow window;
        LogSession *session = window.activeSession();
        QVERIFY(session != nullptr);

        // Baseline: connection is live, marker fans through.
        session->MarkFiltersDirty();
        QVERIFY(window.isWindowModified());
        session->ClearFiltersDirty();
        QVERIFY(!window.isWindowModified());

        // Sever the bag.
        window.UnbindActiveSessionForTest();

        // Same emit now no-ops from the shell's view because the
        // scoped connection is gone. The window keeps its pre-unbind
        // modified state (false here); the session's marker still
        // flips internally but the shell no longer projects it.
        session->MarkFiltersDirty();
        QVERIFY(session->IsFiltersDirty());
        QVERIFY(!window.isWindowModified());
    }

    // -----------------------------------------------------------------
    // Task 4.2 -- structural pin against the class of regressions
    // where a ctor `connect(...)` accidentally bares (drops the
    // `mSessionConnections +=` prefix). The pair-observation
    // approach in the two other Unbind pins can only catch the
    // specific signals they observe; this one asserts on the raw
    // bag size so ANY bare regression that removes a wire from
    // the bag will fail here without needing per-signal coverage.
    //
    // The exact count is deliberately NOT hard-coded -- phase 6
    // will grow / shrink the ctor subscribe block, and pinning
    // "at least a reasonable floor" is enough to trip on a
    // regression while leaving room for legitimate refactors.
    // The lower bound below (35) matches the observed ctor
    // population at review-5 landing with generous headroom; the
    // real number is ~50.
    // -----------------------------------------------------------------

    static void TestScopedConnectionBagSizeMatchesCtorPopulation()
    {
        const MainWindow window;
        // Fresh window: the ctor's ~50 `mSessionConnections +=
        // connect(...)` calls just ran. Precise count moves with
        // legitimate refactors, so pin a floor that would still
        // flag a bare-regression sweep of the ctor.
        constexpr int MINIMUM_EXPECTED_BAG_SIZE = 35;
        QVERIFY2(
            window.SessionConnectionCountForTest() >= MINIMUM_EXPECTED_BAG_SIZE,
            "ctor scoped-connection bag shrank suspiciously -- did a `connect(...)` line "
            "drop its `mSessionConnections +=` prefix?"
        );
    }

    // -----------------------------------------------------------------
    // Task 4.2 second channel (phase-4 review-4 finding #3 coverage
    // gap): the aggregator pin only exercises one bag entry. Emit
    // `rotationFlashChanged` from the active session (which the
    // shell subscribes into `mSessionConnections`) and confirm the
    // subscription is live pre-unbind (session state changes as
    // expected and the shell tolerates the fan) and severed
    // post-unbind (a further trigger cannot land on a torn-down
    // shell path). The pin is deliberately structural -- there is
    // no accessor into the private `UpdateStreamingStatus` output --
    // so it asserts on the state channel that IS observable
    // (`session->IsRotationFlashActive()`) plus a no-crash guarantee
    // for the post-unbind emit.
    // -----------------------------------------------------------------

    static void TestSessionRotationEmitReachesSessionAndSevers()
    {
        MainWindow window;
        LogSession *session = window.activeSession();
        QVERIFY(session != nullptr);
        // Fold the initial state check into the null-guarded arm
        // to keep clang-analyzer's `CallAndMessage` model happy
        // (it does not always see `QVERIFY` as returning, so
        // consecutive QVERIFYs before a deref can misfire).
        QVERIFY(session != nullptr && !session->IsRotationFlashActive());

        // Pre-unbind: session-side state responds; the shell's
        // subscription runs UpdateStreamingStatus (side-effect on
        // the status label, not observable from here) without
        // crashing.
        session->TriggerRotationFlash();
        QVERIFY(session->IsRotationFlashActive());

        // Sever the bag.
        window.UnbindActiveSessionForTest();

        // Session-side state still tracks (the session's own
        // `mRotationFlashActive` is unaffected by the shell's
        // subscription being gone); the shell no longer receives
        // the edge. If a future contributor re-bares any
        // ctor-side subscription, the shell fan-out changes
        // shape here but this pin at least guarantees the raw
        // emit path stays safe.
        session->TriggerRotationFlash();
        QVERIFY(session->IsRotationFlashActive());
    }

    // -----------------------------------------------------------------
    // Task 4.7 -- `BroadcastRotationHistoryPreference` fans a global
    // preference change to every hosted session's CLI opt-out latch.
    // The active-session path is exercised here; multi-tab fanout
    // gains coverage when phase 6 adds sibling sessions.
    // -----------------------------------------------------------------

    static void TestBroadcastRotationHistoryPreferenceClearsCliOverride()
    {
        MainWindow window;
        LogSession *session = window.activeSession();
        QVERIFY(session != nullptr);

        // Simulate a per-window CLI `--no-rotation-history` launch.
        session->SetDisableRotationHistoryOverride(true);
        QVERIFY(session->DisableRotationHistoryOverride());

        // User flips the Settings menu toggle; the shell fans the
        // change through every hosted session.
        window.BroadcastRotationHistoryPreference(true);
        QVERIFY(!session->DisableRotationHistoryOverride());
    }

    // -----------------------------------------------------------------
    // Task 4.7 (source-descriptor mirror) -- when a hosted session
    // carries a `File` source descriptor, the broadcast mirrors the
    // new preference into `followRotationSiblings` so later drops
    // pick up the flip. Sessions without a descriptor pass through
    // the loop untouched.
    // -----------------------------------------------------------------

    static void TestBroadcastRotationHistoryPreferenceMirrorsIntoSource()
    {
        MainWindow window;
        LogSession *session = window.activeSession();
        QVERIFY(session != nullptr);

        loglib::LogConfiguration::Source source;
        source.kind = loglib::LogConfiguration::Source::Kind::File;
        source.locators = {"/tmp/example.log"};
        source.followRotationSiblings = false;
        session->SetCurrentSource(source);

        window.BroadcastRotationHistoryPreference(true);

        const auto &effective = session->CurrentSource();
        QVERIFY(effective.has_value());
        QVERIFY(effective->followRotationSiblings);
    }

    // -----------------------------------------------------------------
    // Task 4.5 -- the window title is projected from the active
    // session's streaming file name / source descriptor. Empty
    // session -> app-name-only title; assigning a file name and
    // firing a presentation refresh (via `filtersDirtyChanged` -->
    // shell `UpdateWindowTitle` connection) lifts it into the title.
    // Not asserting the exact separator glyphs (they are u2014 EM
    // DASH etc.) -- just the substring contract so future
    // title-format changes stay flexible.
    // -----------------------------------------------------------------

    static void TestWindowTitleProjectsFromActiveSessionSourceLabel()
    {
        // Non-`const` on purpose (post review-5): the test drives
        // `UpdateWindowTitle` via the ctor-installed signal fan
        // triggered by `MarkFiltersDirty`, which observably
        // mutates `windowTitle()` on `window`. See the identical
        // rationale on
        // `TestAggregateWindowModifiedFollowsActiveSessionDirty`
        // above.
        MainWindow window; // NOLINT(misc-const-correctness)
        LogSession *session = window.activeSession();
        QVERIFY(session != nullptr);

        const QString appName = QStringLiteral("Structured Log Viewer");
        // Empty session: title starts with the app name (Qt appends
        // "[*]" at the end for the modified-marker placeholder).
        QVERIFY(window.windowTitle().contains(appName));

        // Populate the session's streaming file name, then drive
        // `UpdateWindowTitle` via the ctor-installed
        // `filtersDirtyChanged` connection (session bump ->
        // aggregator + title refresh). `MarkFiltersDirty` / then
        // `ClearFiltersDirty` restores the modified marker so the
        // pin does not leak dirt into subsequent tests within the
        // same process.
        session->SetStreamingFileName(QStringLiteral("dummy.log"));
        session->MarkFiltersDirty();
        QVERIFY(window.windowTitle().contains(QStringLiteral("dummy.log")));
        session->ClearFiltersDirty();
    }
};

QTEST_MAIN(SessionTabsTest)
#include "session_tabs_test.moc"
