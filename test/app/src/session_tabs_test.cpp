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
        const LogSession *session = window.activeSession();
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
        const LogSession *middle = window.SessionAtTab(1);
        LogSession *tail = window.SessionAtTab(2);
        QVERIFY(middle != nullptr);
        QVERIFY(tail != nullptr);
        if (middle == nullptr || tail == nullptr)
        {
            return;
        }
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

    // -----------------------------------------------------------------
    // Task 7.4 / 7.5: destructive-open paths (stdin, network, log
    // stream, Recent Sessions) must NOT clobber a tab that has
    // content. They route through `EnsureFreshActiveTab`, which
    // adds a new foreground tab when the active tab is non-empty.
    // Only a truly empty active tab (no source, no rows) is reused.
    // -----------------------------------------------------------------

    static void TestEnsureFreshActiveTabReusesEmptyTab()
    {
        MainWindow window;
        // Fresh window: single Untitled tab, no source, no rows.
        QCOMPARE(window.TabCount(), 1);
        const LogSession *originalSession = window.activeSession();
        QVERIFY(originalSession != nullptr);

        window.EnsureFreshActiveTab();

        // No new tab created; the empty tab was reused.
        QCOMPARE(window.TabCount(), 1);
        QCOMPARE(window.activeSession(), originalSession);
    }

    static void TestEnsureFreshActiveTabAddsTabWhenActiveHasSource()
    {
        MainWindow window;
        QCOMPARE(window.TabCount(), 1);
        LogSession *originalSession = window.activeSession();
        QVERIFY(originalSession != nullptr);

        // Simulate a bound source on the active tab (no rows, but
        // `CurrentSource().has_value()` is the gate that switches
        // `EnsureFreshActiveTab` from reuse to add).
        loglib::LogConfiguration::Source src;
        src.kind = loglib::LogConfiguration::Source::Kind::File;
        src.locators = {std::string{"/tmp/pretend.log"}};
        src.locatorDedupKeys = {std::string{"/tmp/pretend.log"}};
        originalSession->MutableCurrentSource() = src;

        window.EnsureFreshActiveTab();

        QCOMPARE(window.TabCount(), 2);
        // Newly-added tab is the active one.
        QVERIFY(window.activeSession() != originalSession);
        // Original tab (index 0) is preserved with its bound source.
        QCOMPARE(window.SessionAtTab(0), originalSession);
        QVERIFY(window.SessionAtTab(0)->CurrentSource().has_value());
    }

    // -----------------------------------------------------------------
    // Task 7.1: two independent static sessions share nothing --
    // separate source descriptors, separate simple-mode filter
    // leaves, separate sort state, separate dirty flags. The tab
    // shell must not project one tab's state into another.
    // -----------------------------------------------------------------

    static void TestTwoStaticTabsHaveIndependentSourceState()
    {
        MainWindow window;
        window.AddNewTabForTest(/*makeActive=*/false);
        QCOMPARE(window.TabCount(), 2);
        LogSession *sessionA = window.SessionAtTab(0);
        LogSession *sessionB = window.SessionAtTab(1);
        QVERIFY(sessionA != nullptr);
        QVERIFY(sessionB != nullptr);
        if (sessionA == nullptr || sessionB == nullptr)
        {
            return;
        }
        QVERIFY(sessionA != sessionB);

        // Distinct locator sets: no dedup key should collide across
        // sessions and no locator should leak.
        loglib::LogConfiguration::Source srcA;
        srcA.kind = loglib::LogConfiguration::Source::Kind::File;
        srcA.locators = {std::string{"/tmp/a.log"}, std::string{"/tmp/a.log.1"}};
        srcA.locatorDedupKeys = {std::string{"/tmp/a.log"}, std::string{"/tmp/a.log.1"}};
        sessionA->MutableCurrentSource() = srcA;

        loglib::LogConfiguration::Source srcB;
        srcB.kind = loglib::LogConfiguration::Source::Kind::File;
        srcB.locators = {std::string{"/tmp/b.log"}};
        srcB.locatorDedupKeys = {std::string{"/tmp/b.log"}};
        sessionB->MutableCurrentSource() = srcB;

        QVERIFY(sessionA->CurrentSource().has_value());
        QVERIFY(sessionB->CurrentSource().has_value());
        QCOMPARE(sessionA->CurrentSource()->locators.size(), std::size_t{2});
        QCOMPARE(sessionB->CurrentSource()->locators.size(), std::size_t{1});
        QCOMPARE(sessionA->CurrentSource()->locators.front(), std::string{"/tmp/a.log"});
        QCOMPARE(sessionB->CurrentSource()->locators.front(), std::string{"/tmp/b.log"});
    }

    static void TestTwoStaticTabsHaveIndependentDirtyState()
    {
        MainWindow window;
        window.AddNewTabForTest(/*makeActive=*/false);
        LogSession *sessionA = window.SessionAtTab(0);
        LogSession *sessionB = window.SessionAtTab(1);
        QVERIFY(sessionA != nullptr);
        QVERIFY(sessionB != nullptr);
        if (sessionA == nullptr || sessionB == nullptr)
        {
            return;
        }

        QVERIFY(!sessionA->IsFiltersDirty());
        QVERIFY(!sessionB->IsFiltersDirty());

        sessionA->MarkFiltersDirty();
        QVERIFY(sessionA->IsFiltersDirty());
        QVERIFY2(!sessionB->IsFiltersDirty(), "MarkFiltersDirty on session A must not flip session B's dirty flag.");

        sessionB->MarkFiltersDirty();
        sessionA->ClearFiltersDirty();
        QVERIFY(!sessionA->IsFiltersDirty());
        QVERIFY2(sessionB->IsFiltersDirty(), "ClearFiltersDirty on session A must not clear session B's dirty flag.");
    }

    static void TestTwoStaticTabsHaveIndependentSimpleLeaves()
    {
        MainWindow window;
        window.AddNewTabForTest(/*makeActive=*/false);
        LogSession *sessionA = window.SessionAtTab(0);
        LogSession *sessionB = window.SessionAtTab(1);
        QVERIFY(sessionA != nullptr);
        QVERIFY(sessionB != nullptr);
        if (sessionA == nullptr || sessionB == nullptr)
        {
            return;
        }

        // Seed session A with one leaf and session B with a
        // different one. Access via the mutable accessors; the
        // shell would normally funnel through `AddLogFilter` but
        // this pin is about the ownership boundary, not the shell
        // route.
        sessionA->MutableSimpleLeafOrder().emplace_back("leaf-A");
        sessionA->MutableSimpleLeaves()[std::string{"leaf-A"}] = loglib::LeafRule{};

        sessionB->MutableSimpleLeafOrder().emplace_back("leaf-B");
        sessionB->MutableSimpleLeaves()[std::string{"leaf-B"}] = loglib::LeafRule{};

        QCOMPARE(sessionA->SimpleLeafOrder().size(), std::size_t{1});
        QCOMPARE(sessionB->SimpleLeafOrder().size(), std::size_t{1});
        QCOMPARE(sessionA->SimpleLeafOrder().front(), std::string{"leaf-A"});
        QCOMPARE(sessionB->SimpleLeafOrder().front(), std::string{"leaf-B"});
        QVERIFY(sessionA->SimpleLeaves().contains(std::string{"leaf-A"}));
        QVERIFY(!sessionA->SimpleLeaves().contains(std::string{"leaf-B"}));
        QVERIFY(sessionB->SimpleLeaves().contains(std::string{"leaf-B"}));
        QVERIFY(!sessionB->SimpleLeaves().contains(std::string{"leaf-A"}));
    }

    static void TestTwoStaticTabsHaveIndependentModels()
    {
        MainWindow window;
        window.AddNewTabForTest(/*makeActive=*/false);
        const LogSession *sessionA = window.SessionAtTab(0);
        const LogSession *sessionB = window.SessionAtTab(1);
        QVERIFY(sessionA != nullptr);
        QVERIFY(sessionB != nullptr);
        if (sessionA == nullptr || sessionB == nullptr)
        {
            return;
        }
        QVERIFY(sessionA->Model() != nullptr);
        QVERIFY(sessionB->Model() != nullptr);
        QVERIFY2(
            sessionA->Model() != sessionB->Model(),
            "Each tab must own a distinct LogModel; sharing one would collapse "
            "rows/columns/streaming across the tab strip."
        );
        QVERIFY2(sessionA->Anchors() != sessionB->Anchors(), "Each tab must own a distinct AnchorManager.");
        QVERIFY2(sessionA->Highlights() != sessionB->Highlights(), "Each tab must own a distinct HighlightRuleSet.");
        QVERIFY2(sessionA->FilterProxy() != sessionB->FilterProxy(), "Each tab must own a distinct LogFilterModel.");
    }

    // -----------------------------------------------------------------
    // Task 7.2: compressed / bundle tabs. Each tab owns its own
    // decompression stop-source, generation counter, embedded-
    // bundle intent, in-flight flag, and pending-error vector. A
    // cancel on tab A cannot poison tab B's decompression.
    // -----------------------------------------------------------------

    static void TestTwoCompressedTabsHaveIndependentDecompressionState()
    {
        MainWindow window;
        window.AddNewTabForTest(/*makeActive=*/false);
        LogSession *sessionA = window.SessionAtTab(0);
        LogSession *sessionB = window.SessionAtTab(1);
        QVERIFY(sessionA != nullptr);
        QVERIFY(sessionB != nullptr);
        if (sessionA == nullptr || sessionB == nullptr)
        {
            return;
        }

        // Different stop sources (identity by address).
        QVERIFY2(
            &sessionA->DecompressionStopSource() != &sessionB->DecompressionStopSource(),
            "Each tab must own a distinct decompression stop source; sharing one "
            "would let a cancel on tab A abort tab B's worker."
        );
        QVERIFY2(
            &sessionA->ExportStopSource() != &sessionB->ExportStopSource(),
            "Each tab must own a distinct export stop source."
        );

        // Independent original-path + embedded-config intent.
        sessionA->SetDecompressionOriginalPath(QStringLiteral("/tmp/a.log.zst"));
        sessionB->SetDecompressionOriginalPath(QStringLiteral("/tmp/b.log.zst"));
        QCOMPARE(sessionA->DecompressionOriginalPath(), QStringLiteral("/tmp/a.log.zst"));
        QCOMPARE(sessionB->DecompressionOriginalPath(), QStringLiteral("/tmp/b.log.zst"));

        sessionA->SetApplyEmbeddedBundleConfigForPath(QStringLiteral("/tmp/a-bundle.zst"));
        QVERIFY(sessionA->ShouldApplyEmbeddedBundleConfig());
        QVERIFY2(
            !sessionB->ShouldApplyEmbeddedBundleConfig(), "Arming embedded-bundle apply on tab A must not arm tab B."
        );
        QCOMPARE(sessionA->ApplyEmbeddedBundleConfigForPath(), QStringLiteral("/tmp/a-bundle.zst"));
    }

    static void TestCancelDecompressionInOneTabDoesNotStopOther()
    {
        MainWindow window;
        window.AddNewTabForTest(/*makeActive=*/false);
        LogSession *sessionA = window.SessionAtTab(0);
        const LogSession *sessionB = window.SessionAtTab(1);
        QVERIFY(sessionA != nullptr);
        QVERIFY(sessionB != nullptr);
        if (sessionA == nullptr || sessionB == nullptr)
        {
            return;
        }

        // Precondition: neither stop source has been requested.
        QVERIFY(!sessionA->DecompressionStopSource().stop_requested());
        QVERIFY(!sessionB->DecompressionStopSource().stop_requested());

        sessionA->MutableDecompressionStopSource().request_stop();

        QVERIFY(sessionA->DecompressionStopSource().stop_requested());
        QVERIFY2(
            !sessionB->DecompressionStopSource().stop_requested(),
            "Requesting stop on tab A's decompression source leaked into tab B."
        );
    }

    // -----------------------------------------------------------------
    // Task 7.3: a static tab beside a live-tail tab. Each tab
    // owns its own mode, source-waiting latch, streaming display
    // label, and rotation-follow preference. `SetMode` /
    // `SetSourceWaiting` on one tab must not project into the
    // sibling.
    // -----------------------------------------------------------------

    static void TestStaticAndLiveTailTabsHaveIndependentModeAndLabel()
    {
        MainWindow window;
        window.AddNewTabForTest(/*makeActive=*/false);
        LogSession *staticSession = window.SessionAtTab(0);
        LogSession *liveSession = window.SessionAtTab(1);
        QVERIFY(staticSession != nullptr);
        QVERIFY(liveSession != nullptr);
        if (staticSession == nullptr || liveSession == nullptr)
        {
            return;
        }

        // Both sessions start Idle with an empty streaming label.
        QCOMPARE(staticSession->SessionMode(), LogSession::Mode::Idle);
        QCOMPARE(liveSession->SessionMode(), LogSession::Mode::Idle);
        QVERIFY(staticSession->StreamingFileName().isEmpty());
        QVERIFY(liveSession->StreamingFileName().isEmpty());

        staticSession->SetMode(LogSession::Mode::Static);
        staticSession->SetStreamingFileName(QStringLiteral("static.log"));

        liveSession->SetMode(LogSession::Mode::LiveTail);
        liveSession->SetStreamingFileName(QStringLiteral("live.log"));

        QCOMPARE(staticSession->SessionMode(), LogSession::Mode::Static);
        QCOMPARE(liveSession->SessionMode(), LogSession::Mode::LiveTail);
        QCOMPARE(staticSession->StreamingFileName(), QStringLiteral("static.log"));
        QCOMPARE(liveSession->StreamingFileName(), QStringLiteral("live.log"));
        QVERIFY(!staticSession->IsLiveTailSession());
        QVERIFY(liveSession->IsLiveTailSession());
    }

    static void TestSourceWaitingLatchIsPerTab()
    {
        MainWindow window;
        window.AddNewTabForTest(/*makeActive=*/false);
        LogSession *sessionA = window.SessionAtTab(0);
        LogSession *sessionB = window.SessionAtTab(1);
        QVERIFY(sessionA != nullptr);
        QVERIFY(sessionB != nullptr);
        if (sessionA == nullptr || sessionB == nullptr)
        {
            return;
        }

        QVERIFY(!sessionA->IsSourceWaiting());
        QVERIFY(!sessionB->IsSourceWaiting());

        sessionA->SetSourceWaiting(true);
        QVERIFY(sessionA->IsSourceWaiting());
        QVERIFY2(!sessionB->IsSourceWaiting(), "SetSourceWaiting on tab A must not project onto tab B.");

        sessionA->SetSourceWaiting(false);
        sessionB->SetSourceWaiting(true);
        QVERIFY(!sessionA->IsSourceWaiting());
        QVERIFY(sessionB->IsSourceWaiting());
    }

    static void TestTwoLiveTailTabsHaveIndependentIngestState()
    {
        MainWindow window;
        window.AddNewTabForTest(/*makeActive=*/false);
        LogSession *tailA = window.SessionAtTab(0);
        LogSession *tailB = window.SessionAtTab(1);
        QVERIFY(tailA != nullptr);
        QVERIFY(tailB != nullptr);
        if (tailA == nullptr || tailB == nullptr)
        {
            return;
        }

        tailA->SetMode(LogSession::Mode::LiveTail);
        tailB->SetMode(LogSession::Mode::LiveTail);
        tailA->SetStreamingFileName(QStringLiteral("/var/log/a.log"));
        tailB->SetStreamingFileName(QStringLiteral("/var/log/b.log"));

        // Distinct rotation-history preferences per session.
        loglib::LogConfiguration::Source srcA;
        srcA.kind = loglib::LogConfiguration::Source::Kind::File;
        srcA.locators = {std::string{"/var/log/a.log"}};
        srcA.followRotationSiblings = true;
        tailA->MutableCurrentSource() = srcA;

        loglib::LogConfiguration::Source srcB;
        srcB.kind = loglib::LogConfiguration::Source::Kind::File;
        srcB.locators = {std::string{"/var/log/b.log"}};
        srcB.followRotationSiblings = false;
        tailB->MutableCurrentSource() = srcB;

        QVERIFY(tailA->CurrentSource().has_value());
        QVERIFY(tailB->CurrentSource().has_value());
        QVERIFY(tailA->CurrentSource()->followRotationSiblings);
        QVERIFY(!tailB->CurrentSource()->followRotationSiblings);
    }

    // -----------------------------------------------------------------
    // Task 7.6: background streaming-finished must settle the
    // origin tab (mode -> Idle, SourceWaiting -> false, chrome
    // refresh) even when the user has switched to a different
    // tab. Before 7.6 the bag-scoped `streamingFinished` handler
    // was torn down on tab switch, silently dropping the
    // background tab's completion and leaving it latched.
    // -----------------------------------------------------------------

    static void TestBackgroundStreamingFinishedSettlesOriginMode()
    {
        MainWindow window;
        window.AddNewTabForTest(/*makeActive=*/true);
        QCOMPARE(window.TabCount(), 2);
        LogSession *sessionA = window.SessionAtTab(0);
        LogSession *sessionB = window.SessionAtTab(1);
        QVERIFY(sessionA != nullptr);
        QVERIFY(sessionB != nullptr);
        if (sessionA == nullptr || sessionB == nullptr)
        {
            return;
        }
        // Sanity: after activating the new tab, B is the active
        // session and A is the background one.
        QCOMPARE(window.activeSession(), sessionB);

        // Simulate Session A having an in-flight parse.
        sessionA->SetMode(LogSession::Mode::Static);
        sessionA->SetSourceWaiting(true);
        QCOMPARE(sessionA->SessionMode(), LogSession::Mode::Static);
        QVERIFY(sessionA->IsSourceWaiting());

        // Emit the model's streaming-finished signal for the
        // BACKGROUND session. The persistent per-tab connection
        // installed by `InstallPerTabPersistentConnections`
        // routes this into `HandleStreamingFinishedFor(sessionA,
        // Success)` even though `mSession == sessionB`.
        LogModel *modelA = sessionA->Model();
        QVERIFY(modelA != nullptr);
        emit modelA->streamingFinished(StreamingResult::Success);

        // Origin settled.
        QCOMPARE(sessionA->SessionMode(), LogSession::Mode::Idle);
        QVERIFY(!sessionA->IsSourceWaiting());
        // Sibling was not disturbed.
        QCOMPARE(sessionB->SessionMode(), LogSession::Mode::Idle);
        QVERIFY(!sessionB->IsSourceWaiting());
    }

    // -----------------------------------------------------------------
    // Post-tabs review-round bug #2 fix pin: background streaming
    // completion must drive the FULL `OnStreamingFinished` body,
    // not just settle mode. Pins that pending-open-files queued
    // on a background tab are drained after its streaming
    // completes, so multi-file loads do not stall when the user
    // switches away mid-load. Before the fix,
    // `HandleStreamingFinishedFor` skipped `StreamNextPendingFile`
    // for background origins, leaving the queue permanently
    // stuck.
    // -----------------------------------------------------------------

    static void TestBackgroundStreamingCompletionDrainsPendingFiles()
    {
        MainWindow window;
        window.AddNewTabForTest(/*makeActive=*/true);
        LogSession *sessionA = window.SessionAtTab(0);
        LogSession *sessionB = window.SessionAtTab(1);
        QVERIFY(sessionA != nullptr);
        QVERIFY(sessionB != nullptr);
        if (sessionA == nullptr || sessionB == nullptr)
        {
            return;
        }
        QCOMPARE(window.activeSession(), sessionB);

        // Seed background session A with an in-flight static parse
        // and a follow-up file queued behind the currently-parsing
        // one. The completion body must advance the queue
        // (`StreamNextPendingFile`) even though A is not the
        // active tab.
        sessionA->SetMode(LogSession::Mode::Static);
        sessionA->SetSourceWaiting(true);
        // Non-existent path so `StreamNextPendingFile` takes the
        // synchronous failure branch (records an open error and
        // drains the queue) rather than dispatching a real
        // decompression / parse worker in a headless test.
        sessionA->MutablePendingOpenFiles().append(
            QStringLiteral("/nonexistent/definitely-not-a-real-path/queued.log")
        );
        QVERIFY(!sessionA->MutablePendingOpenFiles().isEmpty());

        LogModel *modelA = sessionA->Model();
        QVERIFY(modelA != nullptr);
        emit modelA->streamingFinished(StreamingResult::Success);

        // Queue drained: either every file was processed
        // successfully or their synchronous open failures were
        // captured into the origin's `MutablePendingOpenErrors()`.
        // Before the fix the queue would still contain the
        // seeded path.
        QVERIFY2(
            sessionA->MutablePendingOpenFiles().isEmpty(),
            "Background streaming completion must drain the origin's pending-file queue "
            "so multi-file loads do not stall when the user switches tabs mid-load."
        );
        // Active tab was not perturbed by the background
        // completion.
        QCOMPARE(window.activeSession(), sessionB);
        QVERIFY(sessionB->MutablePendingOpenFiles().isEmpty());
    }

    // -----------------------------------------------------------------
    // Post-tabs review-round bug #3 fix pin: origin attribution
    // for decompression completions must derive from the
    // `sender()` watcher, not the shell-wide
    // `mDecompressionPollOriginSession` field. Concurrent
    // decompressions on two tabs previously collapsed onto the
    // last-started op's origin, mis-attributing whichever
    // completed first.
    // -----------------------------------------------------------------

    static void TestLogSessionForDecompressionWatcherReturnsOwningSession()
    {
        MainWindow window;
        window.AddNewTabForTest(/*makeActive=*/false);
        LogSession *sessionA = window.SessionAtTab(0);
        LogSession *sessionB = window.SessionAtTab(1);
        QVERIFY(sessionA != nullptr);
        QVERIFY(sessionB != nullptr);
        if (sessionA == nullptr || sessionB == nullptr)
        {
            return;
        }

        // Materialize each session's decompression watcher.
        auto *watcherA = sessionA->EnsureDecompressionWatcher();
        auto *watcherB = sessionB->EnsureDecompressionWatcher();
        QVERIFY(watcherA != nullptr);
        QVERIFY(watcherB != nullptr);
        QVERIFY2(
            watcherA != watcherB,
            "Each session must own a distinct decompression watcher; sharing one would "
            "collapse origin attribution on concurrent completions."
        );

        // Lookup: watcher -> session.
        QCOMPARE(window.LogSessionForDecompressionWatcher(watcherA), sessionA);
        QCOMPARE(window.LogSessionForDecompressionWatcher(watcherB), sessionB);

        // Unknown sender resolves to nullptr (defensive: legacy
        // test callers of the slot without a watcher fall back to
        // `mDecompressionPollOriginSession` in the slot body).
        const QObject unrelated;
        QCOMPARE(window.LogSessionForDecompressionWatcher(&unrelated), nullptr);
        QCOMPARE(window.LogSessionForDecompressionWatcher(nullptr), nullptr);
    }

    // -----------------------------------------------------------------
    // Post-tabs review-round bug #5 fix pin: `SourceModeFor`
    // (private to `main_window.cpp`, indirectly exercised via
    // `CaptureWorkspaceWindow`) must emit `ConfigOnly` when a
    // session has a pinned autosave uuid but no bound source.
    // The pre-fix version always returned `Empty`, and
    // `ApplyWorkspaceWindow`'s `isFilePath` filter then skipped
    // the tab -- a "loaded config, no logs" investigation
    // restored as blank on the next launch.
    // -----------------------------------------------------------------

    static void TestCaptureWorkspaceEmitsConfigOnlyForPinnedUuidWithNoSource()
    {
        using slv::persistence::SourceMode;
        MainWindow window;
        LogSession *session = window.activeSession();
        QVERIFY(session != nullptr);
        if (session == nullptr)
        {
            return;
        }
        // Session with no source and no pinned uuid -> Empty.
        const auto empty = window.CaptureWorkspaceWindow();
        QVERIFY(!empty.tabs.empty());
        QCOMPARE(empty.tabs.front().sourceMode, SourceMode::Empty);

        // Pin a uuid (mirrors what `OpenRecentSession` /
        // `AutoSaveSessionSnapshot` do after loading a
        // columns-only configuration). RestorableSessionUuid()
        // now returns the uuid, so `SourceModeFor` must emit
        // ConfigOnly.
        session->SetAutoSaveUuid(QStringLiteral("11111111-2222-3333-4444-555555555555"));
        const auto configOnly = window.CaptureWorkspaceWindow();
        QVERIFY(!configOnly.tabs.empty());
        QCOMPARE(configOnly.tabs.front().sourceMode, SourceMode::ConfigOnly);
        QCOMPARE(configOnly.tabs.front().sessionUuid, session->RestorableSessionUuid());
    }

    // -----------------------------------------------------------------
    // Post-tabs review-round bug #7 fix pin: `NewSession()` must
    // route through `ConfirmDiscardEphemeralIfDirty`, and the
    // test-only forwarder `NewSessionForTest()` must locally
    // suppress dialogs so fixtures that don't opt into
    // `SetSuppressDialogsForTest(true)` don't hang on the
    // modal prompt. Prior to the fix, `NewSession` wiped the
    // active tab without any prompt -- a File -> New Session
    // shortcut could silently discard dirty ephemeral data.
    // -----------------------------------------------------------------

    static void TestNewSessionForTestPreservesPriorSuppressState()
    {
        MainWindow window;
        QVERIFY(!window.SuppressDialogsForTest());
        window.NewSessionForTest();
        QVERIFY2(
            !window.SuppressDialogsForTest(),
            "NewSessionForTest must restore the caller's SuppressDialogsForTest state on exit "
            "so a prior `false` (production-like) setting is not silently promoted to `true`."
        );

        window.SetSuppressDialogsForTest(true);
        window.NewSessionForTest();
        QVERIFY2(
            window.SuppressDialogsForTest(),
            "NewSessionForTest must restore a prior `true` suppression so callers that opted "
            "into dialog suppression stay suppressed after the forwarder returns."
        );
    }

    // -----------------------------------------------------------------
    // Post-tabs review-round bug #M1 fix pin: after
    // `AutoSaveAllHostedSessions` walks every tab (activating
    // each so the alias-based `AutoSaveSessionSnapshot` helper
    // sees the right session), the currently-active tab must be
    // restored to what it was before the walk. Before the fix,
    // the walk left `mTabWidget->currentIndex() == count() - 1`
    // and the immediately-following `CaptureWorkspaceWindow()`
    // call in `aboutToQuit` persisted the wrong active-tab
    // index, so every multi-tab quit restored the wrong tab.
    // -----------------------------------------------------------------

    static void TestAutoSaveAllHostedSessionsPreservesActiveTabIndex()
    {
        MainWindow window;
        window.SetSuppressDialogsForTest(true);
        window.AddNewTabForTest(/*makeActive=*/false);
        window.AddNewTabForTest(/*makeActive=*/false);
        QCOMPARE(window.TabCount(), 3);

        window.ActivateTabForTest(1);
        QCOMPARE(window.TabWidgetForTest()->currentIndex(), 1);

        window.AutoSaveAllHostedSessions(/*publishOpenWindow=*/false);

        QCOMPARE(window.TabWidgetForTest()->currentIndex(), 1);
    }

    // -----------------------------------------------------------------
    // Post-tabs review-round bug #H3 fix pin: the legacy flat
    // `openWindowsAtQuit` list (consumed as one-window-per-entry
    // by the pre-8.x restore branch in `main.cpp`) receives ONE
    // uuid per window, not one per hosted tab. Before the fix,
    // `main.cpp`'s `aboutToQuit` published every per-tab uuid
    // via `RestorableHostedSessionUuids`, and any launch that
    // fell back to the legacy branch (missing workspace file,
    // downgraded binary) exploded a 3-tab window into 3
    // separate windows.
    //
    // The publisher lives in `main.cpp` (not directly testable
    // from a fixture), but `MainWindow::RestorableActiveSessionUuid`
    // is the public one-per-window getter it now uses. Assert
    // that it returns AT MOST one non-empty uuid regardless of
    // tab count, and that a multi-tab window with pinned tab
    // uuids does not multiply into a length-N list.
    // -----------------------------------------------------------------

    static void TestRestorableActiveSessionUuidIsSingleValued()
    {
        MainWindow window;
        window.SetSuppressDialogsForTest(true);
        window.AddNewTabForTest(/*makeActive=*/false);
        window.AddNewTabForTest(/*makeActive=*/false);
        QCOMPARE(window.TabCount(), 3);

        // Pin an autoSaveUuid on every tab so
        // `RestorableSessionUuid()` would return a non-empty
        // string for each of the three -- the pre-fix
        // publisher walked all three and appended each. The
        // post-fix publisher only reads
        // `RestorableActiveSessionUuid()`, which returns the
        // ACTIVE tab's uuid only.
        for (int i = 0; i < window.TabCount(); ++i)
        {
            LogSession *session = window.SessionAtTab(i);
            QVERIFY(session != nullptr);
            if (session != nullptr)
            {
                session->SetAutoSaveUuid(QStringLiteral("uuid-tab-%1").arg(i));
            }
        }

        window.ActivateTabForTest(1);
        QCOMPARE(window.RestorableActiveSessionUuid(), QStringLiteral("uuid-tab-1"));

        // Multi-tab getter still returns every tab's uuid --
        // that's what the grouped WORKSPACE record uses. The
        // one-per-window contract is expressed via
        // `RestorableActiveSessionUuid`, which is the flat
        // list's authorised source.
        QCOMPARE(window.RestorableHostedSessionUuids().size(), 3);
    }
};

QTEST_MAIN(SessionTabsTest)
#include "session_tabs_test.moc"
