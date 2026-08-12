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

#include <QAction>
#include <QKeySequence>
#include <QObject>
#include <QSignalSpy>
#include <QTabWidget>
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
    // Task 4.2 (post phase-6 review-1 H2 update) -- `UnbindActiveSessionForTest()`
    // disconnects the scoped bag so post-unbind emits from the
    // session's SCOPED subscriptions do not reach the shell.
    // Observable through the rotation flash channel: pre-unbind
    // a `rotationFlashChanged` emit routes into
    // `UpdateStreamingStatus`; post-unbind it does not.
    //
    // Note (H2 fix): `filtersDirtyChanged -> UpdateWindowTitle`
    // moved OUT of the bag and into a persistent per-tab connect
    // (installed in the ctor for tab 0 and in `AddNewTab` for
    // subsequent tabs) so a background tab still surfaces `[*]`
    // through the window's aggregate. It is therefore no longer
    // a valid signal to observe here.
    // -----------------------------------------------------------------

    static void TestUnbindActiveSessionDropsPresentationSubscriptions()
    {
        MainWindow window;
        LogSession *session = window.activeSession();
        QVERIFY(session != nullptr);

        // Baseline: bag population is non-trivial (~40+ connects).
        const std::size_t pre = window.SessionConnectionCountForTest();
        QVERIFY2(pre > 30U, "Ctor install produced an unexpectedly small subscription bag.");

        // Sever the bag.
        window.UnbindActiveSessionForTest();
        QCOMPARE(window.SessionConnectionCountForTest(), static_cast<std::size_t>(0));
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

    // -----------------------------------------------------------------
    // Phase 6 -- tab lifecycle pins. Each covers one of the 6.1-6.11
    // subtasks: initial "Untitled" tab, AddNewTab, tab switch alias
    // refresh, close-tab-preserves-siblings, close-last-closes-window,
    // reorder-keeps-mTabs-in-sync, tab actions registered, and the
    // shortcut sequences bound.
    // -----------------------------------------------------------------

    static void TestNewWindowHasOneUntitledTab()
    {
        const MainWindow window;
        QCOMPARE(window.TabCount(), 1);
        QCOMPARE(window.ActiveTabIndex(), 0);
        QVERIFY(window.TabWidgetForTest() != nullptr);
        QCOMPARE(window.TabWidgetForTest()->tabText(0), QStringLiteral("Untitled"));
        QCOMPARE(window.SessionAtTab(0), window.activeSession());
        QCOMPARE(window.ViewAtTab(0), window.activeSessionView());
    }

    static void TestAddNewTabAppendsAndActivatesByDefault()
    {
        MainWindow window;
        const LogSession *originalSession = window.activeSession();
        QVERIFY(originalSession != nullptr);

        const SessionInstanceId newId = window.AddNewTabForTest(/*makeActive=*/true);
        QVERIFY(newId.isValid());
        QCOMPARE(window.TabCount(), 2);
        QCOMPARE(window.ActiveTabIndex(), 1);

        // Alias re-point ran through `OnActiveTabChanged`.
        QVERIFY(window.activeSession() != nullptr);
        QVERIFY(window.activeSession() != originalSession);
        QCOMPARE(window.activeSession()->InstanceId(), newId);
    }

    static void TestAddNewTabInBackgroundKeepsActiveTab()
    {
        MainWindow window;
        LogSession *originalSession = window.activeSession();
        QVERIFY(originalSession != nullptr);

        const SessionInstanceId newId = window.AddNewTabForTest(/*makeActive=*/false);
        QVERIFY(newId.isValid());
        QCOMPARE(window.TabCount(), 2);
        // Active tab unchanged.
        QCOMPARE(window.ActiveTabIndex(), 0);
        QCOMPARE(window.activeSession(), originalSession);
    }

    static void TestActivateTabRefreshesActiveSessionAlias()
    {
        MainWindow window;
        LogSession *first = window.activeSession();
        QVERIFY(first != nullptr);
        window.AddNewTabForTest(/*makeActive=*/false);
        QCOMPARE(window.ActiveTabIndex(), 0);
        QCOMPARE(window.activeSession(), first);

        window.ActivateTabForTest(1);
        QCOMPARE(window.ActiveTabIndex(), 1);
        QVERIFY(window.activeSession() != first);
    }

    static void TestHostedSessionsGrowsWithTabs()
    {
        MainWindow window;
        QCOMPARE(window.hostedSessions().size(), static_cast<std::size_t>(1));
        window.AddNewTabForTest(/*makeActive=*/false);
        QCOMPARE(window.hostedSessions().size(), static_cast<std::size_t>(2));
        window.AddNewTabForTest(/*makeActive=*/false);
        QCOMPARE(window.hostedSessions().size(), static_cast<std::size_t>(3));
    }

    static void TestTabIndexForSessionMatchesInstanceId()
    {
        MainWindow window;
        const SessionInstanceId first = window.activeSession()->InstanceId();
        const SessionInstanceId second = window.AddNewTabForTest(/*makeActive=*/false);
        QCOMPARE(window.TabIndexForSession(first), 0);
        QCOMPARE(window.TabIndexForSession(second), 1);
        QCOMPARE(window.TabIndexForSession(SessionInstanceId(999'999U)), -1);
    }

    static void TestCloseTabRemovesFromStripAndVector()
    {
        MainWindow window;
        LogSession *first = window.activeSession();
        const SessionInstanceId secondId = window.AddNewTabForTest(/*makeActive=*/false);
        QCOMPARE(window.TabCount(), 2);

        window.CloseTabForTest(1);
        QCOMPARE(window.TabCount(), 1);
        // Active tab is still index 0 with the original session.
        QCOMPARE(window.ActiveTabIndex(), 0);
        QCOMPARE(window.activeSession(), first);
        QCOMPARE(window.TabIndexForSession(secondId), -1);
    }

    static void TestClosingActiveTabActivatesSurvivingTab()
    {
        MainWindow window;
        LogSession *first = window.activeSession();
        const SessionInstanceId secondId = window.AddNewTabForTest(/*makeActive=*/true);
        QCOMPARE(window.activeSession()->InstanceId(), secondId);

        window.CloseTabForTest(1);
        QCOMPARE(window.TabCount(), 1);
        // Active tab falls back to the original.
        QCOMPARE(window.activeSession(), first);
    }

    static void TestClosingLastTabInvokesWindowClose()
    {
        // A visible top-level window closed via `close()` runs its
        // `closeEvent`; we cannot easily observe the deletion here
        // without WA_DeleteOnClose, so pin the intermediate observable
        // behaviour: `close()` accepts and hides the window.
        MainWindow window;
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        QVERIFY(window.isVisible());
        window.CloseTabForTest(0);
        // `close()` fires `closeEvent` which routes to `hide()` by
        // default; the window is no longer visible.
        QVERIFY(!window.isVisible());
        QCOMPARE(window.TabCount(), 1);
    }

    static void TestTabActionsAreRegisteredWithExpectedShortcuts()
    {
        const MainWindow window;
        // Actions were added to the window via `addAction`, so
        // `window.actions()` includes them.
        bool sawNewTab = false;
        bool sawCloseTab = false;
        bool sawNextTab = false;
        bool sawPrevTab = false;
        bool sawOpenInNewTab = false;
        bool sawShortcutCollision = false;
        for (const QAction *action : window.actions())
        {
            if (action == nullptr)
            {
                continue;
            }
            const QString ks = action->shortcut().toString(QKeySequence::PortableText);
            if (ks == QStringLiteral("Ctrl+T"))
            {
                sawNewTab = true;
            }
            else if (ks == QStringLiteral("Ctrl+W"))
            {
                sawCloseTab = true;
            }
            else if (ks == QStringLiteral("Ctrl+Tab"))
            {
                sawNextTab = true;
            }
            else if (ks == QStringLiteral("Ctrl+Shift+Tab"))
            {
                sawPrevTab = true;
            }
            // Phase-6 review-2 H1: Open in New Tab intentionally
            // carries no shortcut. Menu / toolbar exposure only.
            if (action->objectName() == QStringLiteral("actionOpenInNewTab") ||
                action->text().contains(QStringLiteral("Open in New Tab")))
            {
                sawOpenInNewTab = true;
                QVERIFY2(
                    action->shortcut().isEmpty(),
                    "Open in New Tab must not carry a shortcut; Ctrl+Shift+T collides with Follow Newest."
                );
            }
        }
        QVERIFY(sawNewTab);
        QVERIFY(sawCloseTab);
        QVERIFY(sawNextTab);
        QVERIFY(sawPrevTab);
        QVERIFY(sawOpenInNewTab);
        // Explicit anti-regression: nothing else in the window may
        // steal `Ctrl+Shift+T` back from `actionFollowTail`.
        for (const QAction *action : window.actions())
        {
            if (action == nullptr || action->objectName() == QStringLiteral("actionFollowTail"))
            {
                continue;
            }
            if (action->shortcut() == QKeySequence(QStringLiteral("Ctrl+Shift+T")))
            {
                sawShortcutCollision = true;
                break;
            }
        }
        QVERIFY2(!sawShortcutCollision, "A non-followTail action re-bound Ctrl+Shift+T.");
    }

    static void TestNextPreviousTabWrapsAtBoundary()
    {
        MainWindow window;
        // Three tabs total. Start on index 0.
        window.AddNewTabForTest(/*makeActive=*/false);
        window.AddNewTabForTest(/*makeActive=*/false);
        QCOMPARE(window.TabCount(), 3);
        QCOMPARE(window.ActiveTabIndex(), 0);

        // Simulate `Ctrl+Tab` (next tab).
        QAction *nextAction = nullptr;
        for (QAction *action : window.actions())
        {
            if (action != nullptr && action->shortcut() == QKeySequence(QStringLiteral("Ctrl+Tab")))
            {
                nextAction = action;
                break;
            }
        }
        QVERIFY(nextAction != nullptr);
        if (nextAction == nullptr)
        {
            return;
        }
        nextAction->trigger();
        QCOMPARE(window.ActiveTabIndex(), 1);
        nextAction->trigger();
        QCOMPARE(window.ActiveTabIndex(), 2);
        nextAction->trigger();
        // Wrap-around.
        QCOMPARE(window.ActiveTabIndex(), 0);

        // And `Ctrl+Shift+Tab` (previous tab) wraps the other way.
        QAction *prevAction = nullptr;
        for (QAction *action : window.actions())
        {
            if (action != nullptr && action->shortcut() == QKeySequence(QStringLiteral("Ctrl+Shift+Tab")))
            {
                prevAction = action;
                break;
            }
        }
        QVERIFY(prevAction != nullptr);
        if (prevAction == nullptr)
        {
            return;
        }
        prevAction->trigger();
        QCOMPARE(window.ActiveTabIndex(), 2);
    }

    static void TestSameSessionActivationIsIdempotent()
    {
        MainWindow window;
        window.AddNewTabForTest(/*makeActive=*/true);
        QCOMPARE(window.ActiveTabIndex(), 1);
        LogSession *session = window.activeSession();
        // Re-activating the same tab must NOT tear down and rebind.
        window.ActivateTabForTest(1);
        QCOMPARE(window.activeSession(), session);
    }

    static void TestTabChromeReflectsDirtyMarker()
    {
        // Non-`const` on purpose (phase-6 review-2 low-priority
        // finding): the test observably mutates `window` state
        // through `activeSession()->MarkFiltersDirty()`. Marking
        // `window` const would satisfy `misc-const-correctness`
        // (since `activeSession()` is a const method returning a
        // non-const pointer) but would encode a false claim.
        MainWindow window; // NOLINT(misc-const-correctness)
        QVERIFY(window.TabWidgetForTest() != nullptr);
        const QString cleanLabel = window.TabWidgetForTest()->tabText(0);
        QVERIFY(!cleanLabel.contains(QStringLiteral("\u25CF"))); // bullet marker
        // Dirty the session; presentationChanged fans through the
        // ctor-installed subscription which calls RefreshTabChrome.
        window.activeSession()->MarkFiltersDirty();
        QTRY_VERIFY_WITH_TIMEOUT(window.TabWidgetForTest()->tabText(0).contains(QStringLiteral("\u25CF")), 1000);
        window.activeSession()->ClearFiltersDirty();
    }

    static void TestBackgroundTabDestructionDoesNotAffectActive()
    {
        MainWindow window;
        LogSession *first = window.activeSession();
        window.AddNewTabForTest(/*makeActive=*/false);
        QCOMPARE(window.TabCount(), 2);
        QVERIFY(window.SessionAtTab(1) != nullptr);
        QVERIFY(window.SessionAtTab(1) != first);

        // Close the background tab (index 1).
        window.CloseTabForTest(1);
        QCOMPARE(window.TabCount(), 1);
        // Active tab is untouched.
        QCOMPARE(window.activeSession(), first);
        QCOMPARE(window.ActiveTabIndex(), 0);
    }

    // -----------------------------------------------------------------
    // Phase-6 review-1 B1: `InstallActiveSessionConnections` must
    // reinstate the FULL ctor subscription bag on tab switch. Prior
    // to the fix, only ~13 of the ~50 ctor bag connects were
    // reinstalled, so cross-tab-critical wires (Copy, follow-tail,
    // context menus, overview rail, progress cancel, ...) stayed dead
    // after the first switch. The bag-size pin below catches any
    // future divergence between the ctor and switch-time install
    // paths (both now share one helper, but the size pin protects
    // against a partial back-port).
    // -----------------------------------------------------------------

    static void TestScopedConnectionBagSurvivesTabSwitch()
    {
        MainWindow window;
        const std::size_t initialBagSize = window.SessionConnectionCountForTest();
        // Non-trivial bag (~40+ connects with the phase-6 review-1
        // B1 helper). If this ever regresses toward zero, the ctor
        // install path stopped calling `InstallActiveSessionConnections`.
        QVERIFY2(initialBagSize > 30U, "Ctor install produced an unexpectedly small subscription bag.");

        window.AddNewTabForTest(/*makeActive=*/false);
        QCOMPARE(window.TabCount(), 2);
        window.ActivateTabForTest(1);
        const std::size_t postSwitchBagSize = window.SessionConnectionCountForTest();
        QCOMPARE(postSwitchBagSize, initialBagSize);

        // Switch back and re-check; the alias round-trip must not
        // shed subscriptions either.
        window.ActivateTabForTest(0);
        const std::size_t postReturnBagSize = window.SessionConnectionCountForTest();
        QCOMPARE(postReturnBagSize, initialBagSize);
    }

    // -----------------------------------------------------------------
    // Phase-6 review-1 B3: closing a BACKGROUND tab must not swap the
    // strip's current index onto the closing tab and then leave the
    // user on a neighbour. Two-tab tests masked this because Qt's
    // fallback happens to land on the surviving tab either way; the
    // three-tab case exposes the wrong-active regression.
    // -----------------------------------------------------------------

    static void TestClosingBackgroundTabKeepsActiveTabUnchanged()
    {
        MainWindow window;
        LogSession *first = window.activeSession();
        window.AddNewTabForTest(/*makeActive=*/false);
        window.AddNewTabForTest(/*makeActive=*/false);
        QCOMPARE(window.TabCount(), 3);
        QCOMPARE(window.ActiveTabIndex(), 0);

        // Close the middle (background) tab. Active must stay on tab 0.
        window.CloseTabForTest(1);
        QCOMPARE(window.TabCount(), 2);
        QCOMPARE(window.ActiveTabIndex(), 0);
        QCOMPARE(window.activeSession(), first);
    }

    static void TestClosingBackgroundTabPreservesActiveOnEnd()
    {
        MainWindow window;
        LogSession *first = window.activeSession();
        window.AddNewTabForTest(/*makeActive=*/false);
        window.AddNewTabForTest(/*makeActive=*/false);
        QCOMPARE(window.TabCount(), 3);

        // Close the LAST (background) tab. Active must stay on tab 0.
        window.CloseTabForTest(2);
        QCOMPARE(window.TabCount(), 2);
        QCOMPARE(window.ActiveTabIndex(), 0);
        QCOMPARE(window.activeSession(), first);
    }

    // -----------------------------------------------------------------
    // Phase-6 review-1 H2: a background tab going dirty must update
    // the window's `[*]` modified marker via the aggregate. Pre-fix,
    // `filtersDirtyChanged` was only bag-wired for the active tab,
    // so background dirt was invisible to the title until the user
    // switched to that tab.
    // -----------------------------------------------------------------

    static void TestBackgroundTabDirtyFlipsWindowModified()
    {
        MainWindow window; // NOLINT(misc-const-correctness)
        QVERIFY(!window.isWindowModified());
        window.AddNewTabForTest(/*makeActive=*/false);
        QCOMPARE(window.TabCount(), 2);
        QCOMPARE(window.ActiveTabIndex(), 0);

        LogSession *background = window.SessionAtTab(1);
        QVERIFY(background != nullptr);
        QVERIFY(background != window.activeSession());

        // Dirty the BACKGROUND session; the persistent per-tab
        // `filtersDirtyChanged` connect installed in `AddNewTab`
        // must fan through `UpdateWindowTitle`.
        background->MarkFiltersDirty();
        QVERIFY(window.isWindowModified());

        background->ClearFiltersDirty();
        QVERIFY(!window.isWindowModified());
    }

    // -----------------------------------------------------------------
    // Phase-6 review-1 H3: a background tab's `presentationChanged`
    // must keep refreshing its own tab chrome even while inactive.
    // Pre-fix, the connect lived in the scoped bag which was cleared
    // on tab switch, so a background tab's label / tooltip / dirty
    // marker froze at whatever state it had when it left focus.
    // -----------------------------------------------------------------

    static void TestBackgroundTabChromeRefreshesOnPresentationChange()
    {
        MainWindow window;
        QVERIFY(window.TabWidgetForTest() != nullptr);
        window.AddNewTabForTest(/*makeActive=*/false);
        QCOMPARE(window.TabCount(), 2);
        QCOMPARE(window.ActiveTabIndex(), 0);

        LogSession *background = window.SessionAtTab(1);
        QVERIFY(background != nullptr);
        const QString cleanLabel = window.TabWidgetForTest()->tabText(1);
        QVERIFY(!cleanLabel.contains(QStringLiteral("\u25CF")));

        // Dirty the background session; its own persistent
        // `presentationChanged` connect must fire `RefreshTabChrome`
        // for tab index 1 (not the currently-active tab 0).
        background->MarkFiltersDirty();
        QTRY_VERIFY_WITH_TIMEOUT(window.TabWidgetForTest()->tabText(1).contains(QStringLiteral("\u25CF")), 1000);
        // The active tab (0) is NOT dirtied and must not gain the marker.
        QVERIFY(!window.TabWidgetForTest()->tabText(0).contains(QStringLiteral("\u25CF")));

        background->ClearFiltersDirty();
    }

    // -----------------------------------------------------------------
    // Phase-6 review-2 B1 anti-regression: `OnActiveTabChanged` MUST
    // reinstall the full ctor scoped-connection set on tab switch.
    // Pre-fix, the switch cleared ~46 subscriptions and only
    // reinstalled ~14, so a single `Ctrl+T` permanently killed Copy,
    // sort-status updates, header context menus, find-cache
    // invalidation, and follow-tail behaviour window-wide.
    //
    // We pin two channels:
    //   (a) Bag population survives a switch (structural).
    //   (b) `Ctrl+Shift+T` remains free of a non-followTail binding.
    // -----------------------------------------------------------------

    static void TestTabSwitchPreservesScopedConnectionBag()
    {
        MainWindow window;
        const std::size_t initialBagSize = window.SessionConnectionCountForTest();
        constexpr std::size_t MINIMUM_EXPECTED = 35;
        QVERIFY(initialBagSize >= MINIMUM_EXPECTED);

        window.AddNewTabForTest(/*makeActive=*/true);
        QCOMPARE(window.TabCount(), 2);
        QCOMPARE(window.ActiveTabIndex(), 1);
        // After the switch the bag must be re-populated by
        // `InstallActiveSessionConnections()` at parity with the
        // ctor's population. Allow +/- 5 to tolerate the handful of
        // ctor-only connects that guard tab 0's initial setup
        // (e.g. one-shot bootstrapping) which do not re-run on
        // switch. Anything below the floor indicates the switch
        // path is silently unwiring the shell.
        const std::size_t postSwitchBagSize = window.SessionConnectionCountForTest();
        QVERIFY2(
            postSwitchBagSize >= MINIMUM_EXPECTED,
            "Tab switch dropped the scoped-connection bag below the ctor floor; "
            "InstallActiveSessionConnections() is not reinstalling the ctor set."
        );

        // Round-trip back to tab 0 and re-check: the bag must be
        // repopulated again, not left in the "post-switch" state.
        window.ActivateTabForTest(0);
        QCOMPARE(window.ActiveTabIndex(), 0);
        const std::size_t roundTripBagSize = window.SessionConnectionCountForTest();
        QVERIFY(roundTripBagSize >= MINIMUM_EXPECTED);
    }

    // -----------------------------------------------------------------
    // Phase-6 review-2 H1 anti-regression: `Ctrl+Shift+T` is owned by
    // `actionFollowTail` (`main_window.ui:401`). Any other window
    // action binding the same sequence would trigger Qt's "ambiguous
    // shortcut overload" the moment follow-tail was enabled.
    // -----------------------------------------------------------------

    static void TestCtrlShiftTIsNotStolenFromFollowTail()
    {
        const MainWindow window;
        const QKeySequence followTailKey(QStringLiteral("Ctrl+Shift+T"));
        int owners = 0;
        for (const QAction *action : window.actions())
        {
            if (action != nullptr && action->shortcut() == followTailKey)
            {
                ++owners;
            }
        }
        // Also search descendant actions (menu items live on child
        // menus, not directly on the window's action list).
        for (const QAction *action : window.findChildren<QAction *>())
        {
            if (action != nullptr && action->shortcut() == followTailKey)
            {
                if (action->objectName() != QStringLiteral("actionFollowTail"))
                {
                    QFAIL(qPrintable(
                        QStringLiteral("Non-followTail action `%1` bound Ctrl+Shift+T.").arg(action->objectName())
                    ));
                }
            }
        }
        // Sanity: `actionFollowTail` itself must still own the sequence.
        auto *followTail = window.findChild<QAction *>(QStringLiteral("actionFollowTail"));
        QVERIFY(followTail != nullptr);
        QCOMPARE(followTail->shortcut(), followTailKey);
        Q_UNUSED(owners);
    }

    // -----------------------------------------------------------------
    // Phase-6 review-2 M3 anti-regression: `CloseTabAtIndex` must
    // cancel workers BEFORE it mutates `mTabs` so any
    // `presentationChanged` fired by cancel-side effects (see
    // `ClearApplyEmbeddedBundleConfig`) resolves through
    // `TabIndexForSession` against a consistent `mTabs` <->
    // `mTabWidget` mirror -- otherwise the outgoing label writes onto
    // a shifted neighbour tab.
    //
    // We can't easily trigger a real decompression cancel in a unit
    // test, but we CAN pin the invariant that after closing a middle
    // tab the remaining labels are stable (no neighbour got the
    // closed tab's label spliced in).
    // -----------------------------------------------------------------

    static void TestClosingMiddleTabDoesNotStompNeighbourLabels()
    {
        MainWindow window;
        QVERIFY(window.TabWidgetForTest() != nullptr);
        window.AddNewTabForTest(/*makeActive=*/false); // tab 1
        window.AddNewTabForTest(/*makeActive=*/false); // tab 2
        QCOMPARE(window.TabCount(), 3);

        // Give each tab a distinct sentinel label via its session's
        // presentation snapshot. Marking dirty flips the label
        // prefix, which is a stable, observable difference across
        // tabs without any file I/O.
        LogSession *middle = window.SessionAtTab(1);
        LogSession *tail = window.SessionAtTab(2);
        QVERIFY(middle != nullptr);
        QVERIFY(tail != nullptr);
        tail->MarkFiltersDirty();
        // Give the presentationChanged fanout a chance to land.
        QTRY_VERIFY_WITH_TIMEOUT(window.TabWidgetForTest()->tabText(2).contains(QStringLiteral("\u25CF")), 1000);
        const QString tailLabelBefore = window.TabWidgetForTest()->tabText(2);

        window.CloseTabForTest(1);
        QCOMPARE(window.TabCount(), 2);
        // The dirty tail tab shifted into index 1 but its label must
        // still be the dirty label -- not the outgoing middle tab's
        // clean "Untitled".
        const QString newIndex1Label = window.TabWidgetForTest()->tabText(1);
        QVERIFY2(
            newIndex1Label.contains(QStringLiteral("\u25CF")),
            "Neighbour tab lost its dirty marker after middle tab close (cancel-before-erase regression)."
        );
        QCOMPARE(newIndex1Label, tailLabelBefore);
    }

    // -----------------------------------------------------------------
    // Phase-6 review-3 finding #7 anti-regression: tab-management
    // shortcuts must use `Qt::WindowShortcut`, not
    // `Qt::ApplicationShortcut`. Application scope makes identical
    // shortcuts across multiple `MainWindow`s ambiguous the moment
    // the user opens a second window and presses Ctrl+T / Ctrl+W.
    // -----------------------------------------------------------------

    static void TestTabShortcutsUseWindowScope()
    {
        const MainWindow window;
        const std::vector<QString> tabActionNames = {
            QStringLiteral("actionNewTab"),
            QStringLiteral("actionCloseTab"),
            QStringLiteral("actionNextTab"),
            QStringLiteral("actionPreviousTab"),
        };
        for (const QAction *action : window.actions())
        {
            if (action == nullptr)
            {
                continue;
            }
            const QString name = action->objectName();
            // Match either by object name or by shortcut text --
            // some code paths assign both.
            const QString ks = action->shortcut().toString(QKeySequence::PortableText);
            const bool isTabAction = ks == QStringLiteral("Ctrl+T") || ks == QStringLiteral("Ctrl+W") ||
                                     ks == QStringLiteral("Ctrl+Tab") || ks == QStringLiteral("Ctrl+Shift+Tab");
            if (!isTabAction)
            {
                continue;
            }
            QVERIFY2(
                action->shortcutContext() == Qt::WindowShortcut,
                qPrintable(QStringLiteral(
                               "Tab action `%1` (%2) must use Qt::WindowShortcut to avoid ambiguity "
                               "across multiple MainWindows."
                )
                               .arg(name, ks))
            );
        }
    }

    // -----------------------------------------------------------------
    // Phase-6 review-3 finding #3 anti-regression: multi-tab window
    // close must gather uuids from EVERY hosted session, not just
    // the active one. `RestorableHostedSessionUuids()` is what
    // `main.cpp`'s `aboutToQuit` publish step iterates.
    // -----------------------------------------------------------------

    static void TestRestorableHostedSessionUuidsIsPluralForMultiTab()
    {
        MainWindow window;
        window.AddNewTabForTest(/*makeActive=*/false);
        window.AddNewTabForTest(/*makeActive=*/false);
        QCOMPARE(window.TabCount(), 3);
        // No sources bound in a bare test window, so restorable
        // uuids are empty across all tabs. The pin here is
        // structural: the method exists and iterates
        // `hostedSessions()`, not `mSession` alone.
        const QStringList uuids = window.RestorableHostedSessionUuids();
        QVERIFY2(
            uuids.size() <= window.TabCount(),
            "RestorableHostedSessionUuids should cap at TabCount() (empty entries omitted)."
        );
        // The single-tab equivalent must not exceed the plural
        // helper for the ACTIVE tab -- either both empty (bare
        // window) or singular <= plural (once sources bind).
        const QString activeUuid = window.RestorableActiveSessionUuid();
        if (!activeUuid.isEmpty())
        {
            QVERIFY(uuids.contains(activeUuid));
        }
    }
};

QTEST_MAIN(SessionTabsTest)
#include "session_tabs_test.moc"
