// Multi-source tab lifecycle and shell-routing tests.

#include "columns_manager_dialog.hpp"
#include "configuration_diagnostics_dialog.hpp"
#include "find_dock.hpp"
#include "highlight_rule_set.hpp"
#include "highlight_rules_editor.hpp"
#include "histogram_dock.hpp"
#include "histogram_model.hpp"
#include "log_filter_model.hpp"
#include "log_model.hpp"
#include "log_session.hpp"
#include "log_session_presentation.hpp"
#include "log_session_view.hpp"
#include "main_window.hpp"
#include "qstring_path.hpp"
#include "session_history_manager.hpp"
#include "workspace_persistence.hpp"

#include <loglib/filter_expression.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_table.hpp>
#include <loglib/parse_file.hpp>
#include <loglib/session_bundle.hpp>

#include <QAbstractItemModel>
#include <QAction>
#include <QApplication>
#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QDockWidget>
#include <QDropEvent>
#include <QEvent>
#include <QFile>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QObject>
#include <QPointF>
#include <QPointer>
#include <QPushButton>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTemporaryDir>
#include <QTest>
#include <QTextStream>
#include <QToolBar>
#include <QUrl>
#include <QUuid>

#include <zlib.h>

#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace
{

QString WriteJsonlFixture(const QString &path, int rowCount)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
    {
        return {};
    }
    QTextStream out(&file);
    for (int i = 0; i < rowCount; ++i)
    {
        out << "{\"n\":" << i << "}\n";
    }
    return path;
}

void LoadFileIntoActiveTab(MainWindow &window, const QString &path)
{
    LogModel *model = window.activeSession() != nullptr ? window.activeSession()->Model() : nullptr;
    QVERIFY(model != nullptr);
    if (model == nullptr)
    {
        return;
    }
    const QSignalSpy spy(model, &LogModel::streamingFinished);
    window.OpenFilesForTest({path}, MainWindow::OpenMode::Replace);
    QTRY_VERIFY_WITH_TIMEOUT(spy.count() >= 1, 5000);
    QVERIFY(model->rowCount() > 0);
}

QByteArray GzipCompress(const QByteArray &bytes)
{
    z_stream strm{};
    const int initRc = ::deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY);
    if (initRc != Z_OK)
    {
        qFatal("zlib deflateInit2 must succeed (rc=%d)", initRc);
    }
    QByteArray out;
    out.resize(static_cast<qsizetype>(::deflateBound(&strm, static_cast<uLong>(bytes.size()))));
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    strm.next_in = const_cast<Bytef *>(reinterpret_cast<const Bytef *>(bytes.constData()));
    strm.avail_in = static_cast<uInt>(bytes.size());
    strm.next_out = reinterpret_cast<Bytef *>(out.data());
    strm.avail_out = static_cast<uInt>(out.size());
    const int rc = ::deflate(&strm, Z_FINISH);
    if (rc != Z_STREAM_END)
    {
        ::deflateEnd(&strm);
        qFatal("zlib deflate(Z_FINISH) must consume the fixture (rc=%d)", rc);
    }
    out.resize(static_cast<qsizetype>(strm.total_out));
    ::deflateEnd(&strm);
    return out;
}

QString WriteGzipJsonl(const QString &path, int rowCount)
{
    QByteArray uncompressed;
    for (int i = 0; i < rowCount; ++i)
    {
        uncompressed += QByteArray("{\"n\":") + QByteArray::number(i) + "}\n";
    }
    const QByteArray gzipped = GzipCompress(uncompressed);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        return {};
    }
    if (file.write(gzipped) != gzipped.size())
    {
        return {};
    }
    return path;
}

QString WriteBundleFixture(const QString &path, int rowCount, const QString &filterToken)
{
    const QString jsonlPath = path + QStringLiteral(".jsonl");
    QFile jsonl(jsonlPath);
    if (!jsonl.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
    {
        return {};
    }
    QTextStream out(&jsonl);
    for (int i = 0; i < rowCount; ++i)
    {
        out << "{\"msg\":\"row-" << i << "\"}\n";
    }
    jsonl.close();
    try
    {
        loglib::ParseResult parsed = loglib::ParseFile(logapp::QStringToFsPath(jsonlPath));
        loglib::LogConfigurationManager manager;
        manager.Update(parsed.data);
        loglib::LogTable table(std::move(parsed.data), std::move(manager));
        loglib::LogConfiguration configuration = table.Configuration().Configuration();
        if (configuration.columns.empty())
        {
            return {};
        }
        loglib::LeafRule rule;
        rule.type = loglib::LeafRule::Type::String;
        rule.columnKeys = configuration.columns.front().keys;
        rule.filterString = filterToken.toStdString();
        rule.matchType = loglib::LeafRule::Match::Contains;
        configuration.expression = loglib::MakeAnd({loglib::MakeLeaf(std::move(rule))});
        loglib::WriteSessionBundle(table, configuration, logapp::QStringToFsPath(path));
        return path;
    }
    catch (const std::exception &e)
    {
        qWarning().noquote() << "WriteBundleFixture failed:" << e.what();
        return {};
    }
}

class InMemoryRecentsIndexStorage final : public IRecentsIndexStorage
{
public:
    QList<RecentSessionEntry> Read() const override
    {
        return mEntries;
    }

    void Write(const QList<RecentSessionEntry> &entries) override
    {
        mEntries = entries;
    }

    std::optional<QString> ReadLastUuid() const override
    {
        return mLastUuid;
    }

    void WriteLastUuid(const std::optional<QString> &uuid) override
    {
        mLastUuid = uuid;
    }

private:
    QList<RecentSessionEntry> mEntries;
    std::optional<QString> mLastUuid;
};

class ScopedWorkspaceTestPaths
{
public:
    ScopedWorkspaceTestPaths()
    {
        QStandardPaths::setTestModeEnabled(true);
        QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).mkpath(QStringLiteral("sessions"));
        slv::persistence::WorkspacePersistence::Clear();
        (void)slv::persistence::WorkspacePersistence::TakeDeferredWindows();
    }
    ScopedWorkspaceTestPaths(const ScopedWorkspaceTestPaths &) = delete;
    ScopedWorkspaceTestPaths &operator=(const ScopedWorkspaceTestPaths &) = delete;
    ScopedWorkspaceTestPaths(ScopedWorkspaceTestPaths &&) = delete;
    ScopedWorkspaceTestPaths &operator=(ScopedWorkspaceTestPaths &&) = delete;
    ~ScopedWorkspaceTestPaths()
    {
        slv::persistence::WorkspacePersistence::Clear();
        (void)slv::persistence::WorkspacePersistence::TakeDeferredWindows();
        QStandardPaths::setTestModeEnabled(false);
    }
};

QStringList FilterMenuTitles(const MainWindow &window)
{
    const auto *menu = window.findChild<QMenu *>(QStringLiteral("menuFilters"));
    QStringList titles;
    if (menu == nullptr)
    {
        return titles;
    }
    const auto actions = menu->actions();
    for (const QAction *action : actions)
    {
        if (action != nullptr && !action->data().toString().isNull())
        {
            titles.append(action->text());
        }
    }
    return titles;
}

void AddContainsFilter(MainWindow &window, const QString &token)
{
    loglib::LeafRule rule;
    rule.type = loglib::LeafRule::Type::String;
    rule.columnKeys = {"n"};
    rule.filterString = token.toStdString();
    rule.matchType = loglib::LeafRule::Match::Contains;
    window.AddLogFilterForTest(QUuid::createUuid().toString(), rule);
}

[[nodiscard]] QStringList SimpleLeafFilterStrings(const LogSession &session)
{
    QStringList values;
    for (const auto &entry : session.SimpleLeaves())
    {
        if (entry.second.filterString.has_value())
        {
            values.append(QString::fromStdString(*entry.second.filterString));
        }
    }
    return values;
}

[[nodiscard]] bool ExpressionContainsFilterString(const loglib::FilterExpression &expression, const std::string &needle)
{
    const auto visit = [&](const loglib::FilterExpression &node, const auto &self) -> bool {
        if (const auto *leaf = std::get_if<loglib::FilterExpression::Leaf>(&node.node); leaf != nullptr)
        {
            return leaf->rule.filterString.has_value() && *leaf->rule.filterString == needle;
        }
        if (const auto *asAnd = std::get_if<loglib::FilterExpression::And>(&node.node); asAnd != nullptr)
        {
            for (const auto &child : asAnd->children)
            {
                if (self(child, self))
                {
                    return true;
                }
            }
        }
        else if (const auto *asOr = std::get_if<loglib::FilterExpression::Or>(&node.node); asOr != nullptr)
        {
            for (const auto &child : asOr->children)
            {
                if (self(child, self))
                {
                    return true;
                }
            }
        }
        else if (
            const auto *asNot = std::get_if<loglib::FilterExpression::Not>(&node.node);
            asNot != nullptr && asNot->child != nullptr
        )
        {
            return self(*asNot->child, self);
        }
        return false;
    };
    return visit(expression, visit);
}

[[nodiscard]] loglib::LogConfiguration::HighlightRule MakeDraftHighlightRule()
{
    loglib::LogConfiguration::HighlightRule rule;
    rule.name = "draft-rule";
    rule.columnKeys = {"n"};
    rule.type = loglib::LogConfiguration::HighlightRule::Type::String;
    rule.matchType = loglib::LogConfiguration::HighlightRule::Match::Contains;
    rule.filterString = std::string{"draft-needle"};
    return rule;
}

[[nodiscard]] HighlightRulesEditorDraft MakeDirtyHighlightDraft(
    const std::vector<loglib::LogConfiguration::HighlightRule> &committed
)
{
    HighlightRulesEditorDraft draft;
    draft.baseline = committed;
    draft.localRules = committed;
    draft.localRules.push_back(MakeDraftHighlightRule());
    draft.currentRow = static_cast<int>(draft.localRules.size()) - 1;
    return draft;
}

[[nodiscard]] std::vector<bool> ColumnVisibility(const LogSession &session)
{
    std::vector<bool> visible;
    const LogModel *model = session.Model();
    if (model == nullptr)
    {
        return visible;
    }
    for (const auto &column : model->Configuration().columns)
    {
        visible.push_back(column.visible);
    }
    return visible;
}

} // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage): `Q_OBJECT` QtTest fixture.
class SessionTabsTest : public QObject
{
    Q_OBJECT

private slots:
    static void TestSessionOperationStateFlagsPack()
    {
        // Compact indicators combine independent operation-state bits.
        const std::uint32_t combined = static_cast<std::uint32_t>(SessionOperationState::Ingesting) |
                                       static_cast<std::uint32_t>(SessionOperationState::Paused);
        QVERIFY((combined & static_cast<std::uint32_t>(SessionOperationState::Ingesting)) != 0U);
        QVERIFY((combined & static_cast<std::uint32_t>(SessionOperationState::Paused)) != 0U);
        QVERIFY((combined & static_cast<std::uint32_t>(SessionOperationState::Exporting)) == 0U);
    }

    // Active-session accessors return the constructed pair.

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

    // `hostedSessions()` is the collection every
    // window-wide operation (modified aggregation, close preflight,
    // and preference broadcast) walks.

    static void TestHostedSessionsContainsExactlyTheActiveSession()
    {
        const MainWindow window;
        const std::vector<LogSession *> sessions = window.hostedSessions();
        QCOMPARE(sessions.size(), static_cast<std::size_t>(1));
        QCOMPARE(sessions.front(), window.activeSession());
    }

    // Modified-window aggregation folds every hosted
    // session's dirty marker. Single-session windows resolve to the
    // active session's `IsFiltersDirty()`.

    static void TestAggregateWindowModifiedFollowsActiveSessionDirty()
    {
        // Non-`const` because the test observably mutates
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

    // `UnbindActiveSessionForTest()`
    // disconnects the scoped bag so post-unbind emits from the
    // session's SCOPED subscriptions do not reach the shell.
    // Observable through the rotation flash channel: pre-unbind
    // a `rotationFlashChanged` emit routes into
    // `UpdateStreamingStatus`; post-unbind it does not.
    // `filtersDirtyChanged -> UpdateWindowTitle` is persistent per
    // tab and is therefore not a valid scoped signal to observe here.

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

    // A minimum bag size catches constructor connections that omit
    // the `mSessionConnections +=` ownership prefix. The exact count
    // remains flexible for legitimate connection changes.

    static void TestScopedConnectionBagSizeMatchesCtorPopulation()
    {
        const MainWindow window;
        // The exact connection count can vary, so enforce a floor
        // that still detects unowned constructor connections.
        constexpr int MINIMUM_EXPECTED_BAG_SIZE = 35;
        QVERIFY2(
            window.SessionConnectionCountForTest() >= MINIMUM_EXPECTED_BAG_SIZE,
            "ctor scoped-connection bag shrank suspiciously -- did a `connect(...)` line "
            "drop its `mSessionConnections +=` prefix?"
        );
    }

    // The aggregate-state test exercises only one bag entry. Emit
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
        // the edge. The raw emit path remains safe after unbinding.
        session->TriggerRotationFlash();
        QVERIFY(session->IsRotationFlashActive());
    }

    // `BroadcastRotationHistoryPreference` fans a global
    // preference change to every hosted session's CLI opt-out latch.

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

    // When a hosted session
    // carries a `File` source descriptor, the broadcast mirrors the
    // new preference into `followRotationSiblings` so later drops
    // pick up the flip. Sessions without a descriptor pass through
    // the loop untouched.

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

    // The window title is projected from the active
    // session's streaming file name / source descriptor. Empty
    // session -> app-name-only title; assigning a file name and
    // firing a presentation refresh (via `filtersDirtyChanged` -->
    // shell `UpdateWindowTitle` connection) lifts it into the title.
    // Not asserting the exact separator glyphs (they are u2014 EM
    // DASH etc.) -- only the source-label substring is invariant.

    static void TestWindowTitleProjectsFromActiveSessionSourceLabel()
    {
        // Non-`const` because the test drives
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

    // Tab creation, activation, closure, and shortcut behavior.

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

    static void TestClosePromptDoesNotMentionSessionBundleExport()
    {
        LogSession session;
        session.MarkFiltersDirty();
        const QString text = MainWindow::ClosePromptInformativeTextForTest(session);
        QVERIFY(!text.isEmpty());
        QVERIFY2(
            !text.contains(QStringLiteral("Session Bundle"), Qt::CaseInsensitive) &&
                !text.contains(QStringLiteral("slvbundle"), Qt::CaseInsensitive),
            qPrintable(text)
        );
    }

    static void TestFileBackedDirtyTabAutosavesOnClose()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());
        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());
        MainWindow window(nullptr, &manager, nullptr);
        window.SetSuppressDialogsForTest(true);

        const QTemporaryDir logs;
        QVERIFY(logs.isValid());
        const QString path = WriteJsonlFixture(logs.filePath(QStringLiteral("app.jsonl")), 8);
        QVERIFY(!path.isEmpty());
        LoadFileIntoActiveTab(window, path);

        LogSession *session = window.activeSession();
        QVERIFY(session != nullptr);
        if (session == nullptr)
        {
            return;
        }
        session->MarkFiltersDirty();
        QCOMPARE(session->CloseDecision(), SessionCloseDecision::Autosave);

        window.AddNewTabForTest(/*makeActive=*/false);
        QCOMPARE(window.TabCount(), 2);
        window.CloseTabForTest(0);
        QCOMPARE(window.TabCount(), 1);
        QVERIFY(!manager.List().isEmpty());
    }

    static void TestFailedAutoSaveVetoesTabCloseAndKeepsUuid()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());
        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());
        MainWindow window(nullptr, &manager, nullptr);
        window.SetSuppressDialogsForTest(true);

        const QTemporaryDir logs;
        QVERIFY(logs.isValid());
        const QString path = WriteJsonlFixture(logs.filePath(QStringLiteral("app.jsonl")), 8);
        QVERIFY(!path.isEmpty());
        LoadFileIntoActiveTab(window, path);

        LogSession *session = window.activeSession();
        QVERIFY(session != nullptr);
        if (session == nullptr)
        {
            return;
        }
        const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
        session->SetAutoSaveUuid(uuid);
        session->MarkFiltersDirty();
        QCOMPARE(session->CloseDecision(), SessionCloseDecision::Autosave);

        window.AddNewTabForTest(/*makeActive=*/false);
        window.SetFailNextAutoSaveForTest(true);
        window.CloseTabForTest(0);
        QCOMPARE(window.TabCount(), 2);
        QCOMPARE(window.SessionAtTab(0), session);
        QCOMPARE(session->AutoSaveUuid(), uuid);
        QVERIFY(session->IsFiltersDirty());
    }

    static void TestFailedAutoSaveVetoesNewSession()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());
        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());
        MainWindow window(nullptr, &manager, nullptr);
        window.SetSuppressDialogsForTest(true);

        const QTemporaryDir logs;
        QVERIFY(logs.isValid());
        const QString path = WriteJsonlFixture(logs.filePath(QStringLiteral("app.jsonl")), 8);
        QVERIFY(!path.isEmpty());
        LoadFileIntoActiveTab(window, path);

        LogSession *session = window.activeSession();
        QVERIFY(session != nullptr);
        if (session == nullptr)
        {
            return;
        }
        session->MarkFiltersDirty();
        window.SetFailNextAutoSaveForTest(true);
        window.InvokeNewSessionForTest();
        QCOMPARE(window.activeSession(), session);
        QVERIFY(session->IsFiltersDirty());
        QCOMPARE(window.TabCount(), 1);
    }

    static void TestFailedAutoSaveVetoesLastTabWindowClose()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());
        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());
        MainWindow window(nullptr, &manager, nullptr);
        window.SetSuppressDialogsForTest(true);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        const QTemporaryDir logs;
        QVERIFY(logs.isValid());
        const QString path = WriteJsonlFixture(logs.filePath(QStringLiteral("app.jsonl")), 8);
        QVERIFY(!path.isEmpty());
        LoadFileIntoActiveTab(window, path);

        LogSession *session = window.activeSession();
        QVERIFY(session != nullptr);
        if (session == nullptr)
        {
            return;
        }
        const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
        session->SetAutoSaveUuid(uuid);
        session->MarkFiltersDirty();
        window.SetFailNextAutoSaveForTest(true);
        window.CloseTabForTest(0);
        QVERIFY(window.isVisible());
        QCOMPARE(window.TabCount(), 1);
        QCOMPARE(window.activeSession(), session);
        QCOMPARE(session->AutoSaveUuid(), uuid);
    }

    static void TestCancelCloseTabPreservesSessionIdentity()
    {
        MainWindow window;
        LogSession *first = window.activeSession();
        QVERIFY(first != nullptr);
        if (first == nullptr)
        {
            return;
        }
        const SessionInstanceId id = first->InstanceId();
        first->MarkFiltersDirty();
        QCOMPARE(first->CloseDecision(), SessionCloseDecision::Prompt);

        window.AddNewTabForTest(/*makeActive=*/false);
        window.QueueClosePromptChoiceForTest(MainWindow::ClosePromptChoiceForTest::Cancel);
        window.CloseTabForTest(0);
        QCOMPARE(window.TabCount(), 2);
        QCOMPARE(window.SessionAtTab(0), first);
        QCOMPARE(first->InstanceId(), id);
        QVERIFY(first->IsFiltersDirty());
    }

    static void TestCancelNewSessionLeavesCurrentSessionSelected()
    {
        MainWindow window;
        LogSession *session = window.activeSession();
        QVERIFY(session != nullptr);
        if (session == nullptr)
        {
            return;
        }
        session->MarkFiltersDirty();
        window.QueueClosePromptChoiceForTest(MainWindow::ClosePromptChoiceForTest::Cancel);
        window.InvokeNewSessionForTest();
        QCOMPARE(window.activeSession(), session);
        QVERIFY(session->IsFiltersDirty());
        QCOMPARE(window.TabCount(), 1);
    }

    static void TestCancelLastTabCloseLeavesWindowOpen()
    {
        MainWindow window;
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        LogSession *session = window.activeSession();
        QVERIFY(session != nullptr);
        if (session == nullptr)
        {
            return;
        }
        session->MarkFiltersDirty();
        window.QueueClosePromptChoiceForTest(MainWindow::ClosePromptChoiceForTest::Cancel);
        window.CloseTabForTest(0);
        QVERIFY(window.isVisible());
        QCOMPARE(window.TabCount(), 1);
        QCOMPARE(window.activeSession(), session);
        QVERIFY(session->IsFiltersDirty());
    }

    static void TestCancelSecondPromptAbortsWholeQuit()
    {
        MainWindow window;
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        LogSession *first = window.activeSession();
        QVERIFY(first != nullptr);
        if (first == nullptr)
        {
            return;
        }
        first->MarkFiltersDirty();
        window.AddNewTabForTest(/*makeActive=*/true);
        LogSession *second = window.activeSession();
        QVERIFY(second != nullptr && second != first);
        if (second == nullptr)
        {
            return;
        }
        second->MarkFiltersDirty();

        window.QueueClosePromptChoiceForTest(MainWindow::ClosePromptChoiceForTest::Discard);
        window.QueueClosePromptChoiceForTest(MainWindow::ClosePromptChoiceForTest::Cancel);
        QVERIFY(!window.close());
        QVERIFY(window.isVisible());
        QCOMPARE(window.TabCount(), 2);
        QCOMPARE(window.SessionAtTab(0), first);
        QCOMPARE(window.SessionAtTab(1), second);
        QVERIFY(first->IsFiltersDirty());
        QVERIFY(second->IsFiltersDirty());
    }

    static void TestDirtyBusyTabCancelLeavesWorkersRunning()
    {
        const QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString gzipPath = WriteGzipJsonl(temp.filePath(QStringLiteral("busy.jsonl.gz")), 12000);
        QVERIFY(!gzipPath.isEmpty());

        MainWindow window;
        window.SetSuppressDialogsForTest(true);
        window.AddNewTabForTest(/*makeActive=*/false);
        window.OpenFilesForTest({gzipPath}, MainWindow::OpenMode::Replace);
        LogSession *sessionA = window.SessionAtTab(0);
        QVERIFY(sessionA != nullptr);
        if (sessionA == nullptr)
        {
            return;
        }
        QVERIFY(sessionA->IsDecompressionInFlight());
        sessionA->MarkFiltersDirty();

        window.QueueClosePromptChoiceForTest(MainWindow::ClosePromptChoiceForTest::Cancel);
        window.CloseTabForTest(0);
        QCOMPARE(window.TabCount(), 2);
        QCOMPARE(window.SessionAtTab(0), sessionA);
        QVERIFY(sessionA->IsDecompressionInFlight());
        QVERIFY(sessionA->IsFiltersDirty());

        QTRY_VERIFY_WITH_TIMEOUT(!sessionA->IsDecompressionInFlight(), 20000);
    }

    static void TestDirtyBusyTabDiscardDrainsWorkers()
    {
        const QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString gzipPath = WriteGzipJsonl(temp.filePath(QStringLiteral("busy.jsonl.gz")), 12000);
        QVERIFY(!gzipPath.isEmpty());

        MainWindow window;
        window.SetSuppressDialogsForTest(true);
        window.AddNewTabForTest(/*makeActive=*/false);
        window.OpenFilesForTest({gzipPath}, MainWindow::OpenMode::Replace);
        LogSession *sessionA = window.SessionAtTab(0);
        QVERIFY(sessionA != nullptr);
        if (sessionA == nullptr)
        {
            return;
        }
        QVERIFY(sessionA->IsDecompressionInFlight());
        sessionA->MarkFiltersDirty();
        const SessionInstanceId closedId = sessionA->InstanceId();

        window.QueueClosePromptChoiceForTest(MainWindow::ClosePromptChoiceForTest::Discard);
        QVERIFY(window.PrepareSessionClose(sessionA));
        QVERIFY2(sessionA->IsDecompressionInFlight(), "The close prompt must complete before workers are cancelled.");

        window.CloseTabForTest(0);
        QCOMPARE(window.TabCount(), 1);
        QCOMPARE(window.TabIndexForSession(closedId), -1);
        LogSession *remaining = window.activeSession();
        QVERIFY(remaining != nullptr);
        if (remaining == nullptr)
        {
            return;
        }
        QVERIFY(!remaining->IsDecompressionInFlight());
        QVERIFY(!remaining->IsExportInFlight());
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
        bool sawRenameTab = false;
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
            const QList<QKeySequence> sequences = action->shortcuts();
            if (sequences.contains(QKeySequence(QStringLiteral("Ctrl+PgDown"))))
            {
                sawNextTab = true;
            }
            if (sequences.contains(QKeySequence(QStringLiteral("Ctrl+PgUp"))))
            {
                sawPrevTab = true;
            }
            if (action->objectName() == QStringLiteral("actionRenameTab") ||
                action->text().contains(QStringLiteral("Rename Tab")))
            {
                sawRenameTab = true;
                QVERIFY2(
                    action->shortcut().isEmpty(),
                    "Rename Tab must not take window-scope F2; that shortcut jumps to the next anchor."
                );
            }
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
        QVERIFY(sawRenameTab);
        // Nothing else in the window may own `Ctrl+Shift+T`.
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
        // Non-`const` because the test observably mutates `window` state
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

    // `InstallActiveSessionConnections` reinstates the full scoped
    // subscription bag on every tab switch.

    static void TestScopedConnectionBagSurvivesTabSwitch()
    {
        MainWindow window;
        const std::size_t initialBagSize = window.SessionConnectionCountForTest();
        // A non-trivial bag confirms that the constructor installed
        // the active-session connections.
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

    // Closing a background tab must not swap the
    // strip's current index onto the closing tab and then leave the
    // user on a neighbour. The three-tab case distinguishes the
    // active tab from both closing and surviving background tabs.

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

    // A background tab's dirty state contributes to the window's
    // aggregate modified marker.

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

    // A background tab's `presentationChanged` signal refreshes its
    // own chrome while the tab remains inactive.

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

    // Tab switches preserve the active session's full scoped
    // connection bag.

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

    // `Ctrl+Shift+T` is owned by
    // `actionFollowTail` (`main_window.ui:401`). Any other window
    // action binding the same sequence would trigger Qt's "ambiguous
    // shortcut overload" the moment follow-tail was enabled.

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

    // `CloseTabAtIndex` must
    // cancel workers BEFORE it mutates `mTabs` so any
    // `presentationChanged` fired by cancel-side effects (see
    // `ClearApplyEmbeddedBundleConfig`) resolves through
    // `TabIndexForSession` against a consistent `mTabs` <->
    // `mTabWidget` mirror -- otherwise the outgoing label writes onto
    // a shifted neighbour tab.
    // A real decompression cancel is not required to verify that
    // after closing a middle
    // tab the remaining labels are stable (no neighbour got the
    // closed tab's label spliced in).

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

    // Tab-management shortcuts use `Qt::WindowShortcut`, not
    // `Qt::ApplicationShortcut`. Application scope makes identical
    // shortcuts across multiple `MainWindow`s ambiguous the moment
    // the user opens a second window and presses Ctrl+T / Ctrl+W.

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

    // Multi-tab window closure gathers UUIDs from every hosted session.
    // `RestorableHostedSessionUuids()` is used by
    // `main.cpp`'s `aboutToQuit` publish step iterates.

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

    // Destructive-open paths (stdin, network, log
    // stream, Recent Sessions) must NOT clobber a tab that has
    // content. They route through `EnsureFreshActiveTab`, which
    // adds a new foreground tab when the active tab is non-empty.
    // Only a truly empty active tab (no source, no rows) is reused.

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

    // Two independent static sessions share no mutable state:
    // separate source descriptors, separate simple-mode filter
    // leaves, separate sort state, separate dirty flags. The tab
    // shell must not project one tab's state into another.

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

    // Each compressed or bundle tab owns its own
    // decompression stop-source, generation counter, embedded-
    // bundle intent, in-flight flag, and pending-error vector. A
    // cancel on tab A cannot poison tab B's decompression.

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

    // A static tab beside a live-tail tab has independent mode,
    // source-waiting latch, streaming display
    // label, and rotation-follow preference. `SetMode` /
    // `SetSourceWaiting` on one tab must not project into the
    // sibling.

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

    // Background streaming completion settles the
    // origin tab (mode -> Idle, SourceWaiting -> false, chrome
    // refresh) even when the user has switched to a different
    // tab.

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

    // Background streaming completion runs the full
    // `OnStreamingFinished` behavior, including draining files
    // queued on the originating tab.

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

        // The queue drains after files are processed or their
        // synchronous open failures are captured in the origin's
        // `MutablePendingOpenErrors()`.
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

    // Persistent connections live on each WindowTab, not the active-session bag.
    // Unbinding the active bag and closing a sibling must not drop them.

    static void TestPerTabConnectionsSurviveActiveUnbindAndSiblingClose()
    {
        MainWindow window;
        QVERIFY(window.PerTabConnectionCountForTest(0) >= 3U);
        window.UnbindActiveSessionForTest();
        QCOMPARE(window.SessionConnectionCountForTest(), static_cast<std::size_t>(0));
        QVERIFY(window.PerTabConnectionCountForTest(0) >= 3U);

        window.AddNewTabForTest(/*makeActive=*/true);
        QCOMPARE(window.TabCount(), 2);
        const std::size_t tab0Count = window.PerTabConnectionCountForTest(0);
        const std::size_t tab1Count = window.PerTabConnectionCountForTest(1);
        QVERIFY(tab0Count >= 3U);
        QVERIFY(tab1Count >= 3U);

        window.CloseTabForTest(1);
        QCOMPARE(window.TabCount(), 1);
        QCOMPARE(window.PerTabConnectionCountForTest(0), tab0Count);
        QCOMPARE(window.PerTabConnectionCountForTest(1), static_cast<std::size_t>(0));
    }

    // Closing a tab with a queued follow-up file must not start that file
    // and must not mutate the surviving session or view.

    static void TestClosingTabDoesNotStartQueuedFileOrMutateSurvivor()
    {
        MainWindow window;
        window.AddNewTabForTest(/*makeActive=*/true);
        LogSession *sessionA = window.SessionAtTab(0);
        LogSession *sessionB = window.SessionAtTab(1);
        QVERIFY(sessionA != nullptr && sessionB != nullptr);
        if (sessionA == nullptr || sessionB == nullptr)
        {
            return;
        }
        QCOMPARE(window.activeSession(), sessionB);
        LogSessionView *viewB = qobject_cast<LogSessionView *>(window.TabWidgetForTest()->widget(1));
        QVERIFY(viewB != nullptr);
        const bool viewBEnabled = viewB->IsContentEnabled();
        const auto modeB = sessionB->SessionMode();
        const bool waitingB = sessionB->IsSourceWaiting();
        LogModel *modelB = sessionB->Model();
        QVERIFY(modelB != nullptr);
        const int rowsB = modelB->rowCount();

        sessionA->SetMode(LogSession::Mode::Static);
        sessionA->SetSourceWaiting(true);
        sessionA->MutablePendingOpenFiles().append(
            QStringLiteral("/nonexistent/definitely-not-a-real-path/queued-after-close.log")
        );
        LogModel *modelA = sessionA->Model();
        QVERIFY(modelA != nullptr);
        const SessionInstanceId idA = sessionA->InstanceId();
        const SessionInstanceId idB = sessionB->InstanceId();

        window.CloseTabForTest(0);
        QCOMPARE(window.TabCount(), 1);
        QCOMPARE(window.HostedSession(idA), nullptr);
        QCOMPARE(window.HostedSession(idB), sessionB);
        QCOMPARE(window.activeSession(), sessionB);

        // Simulate a queued completion arriving after close. The hosted-registry
        // check and disconnected per-tab subscriptions must both ignore it.
        sessionA->MutablePendingOpenFiles().append(
            QStringLiteral("/nonexistent/definitely-not-a-real-path/queued-after-close.log")
        );
        sessionA->SetMode(LogSession::Mode::Static);
        sessionA->SetSourceWaiting(true);
        emit modelA->streamingFinished(StreamingResult::Success);

        QVERIFY2(
            !sessionA->MutablePendingOpenFiles().isEmpty(),
            "A queued completion after close must not run StreamNextPendingFile on the "
            "unhosted session."
        );
        QVERIFY(!modelA->IsStreamingActive());
        QCOMPARE(sessionA->SessionMode(), LogSession::Mode::Static);
        QVERIFY(sessionA->IsSourceWaiting());

        QCOMPARE(window.activeSession(), sessionB);
        QCOMPARE(sessionB->SessionMode(), modeB);
        QCOMPARE(sessionB->IsSourceWaiting(), waitingB);
        QVERIFY(sessionB->MutablePendingOpenFiles().isEmpty());
        QCOMPARE(modelB->rowCount(), rowsB);
        QVERIFY(!modelB->IsStreamingActive());
        QCOMPARE(viewB->IsContentEnabled(), viewBEnabled);
    }

    // Decompression completion origin comes from the watcher that
    // finished, looked up in the hosted-tab registry.

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

        const QObject unrelated;
        QCOMPARE(window.LogSessionForDecompressionWatcher(&unrelated), nullptr);
        QCOMPARE(window.LogSessionForDecompressionWatcher(nullptr), nullptr);
    }

    // `SourceModeFor` (private to `main_window.cpp`, indirectly exercised via
    // `CaptureWorkspaceWindow`) must emit `ConfigOnly` when a
    // session has a pinned autosave uuid but no bound source.

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
        QVERIFY(empty.tabs.front().label.isEmpty());

        // Pin a uuid (mirrors what `OpenRecentSession` /
        // `AutoSaveSessionSnapshot` do after loading a
        // columns-only configuration). RestorableSessionUuid()
        // returns the uuid, so `SourceModeFor` must emit
        // ConfigOnly.
        session->SetAutoSaveUuid(QStringLiteral("11111111-2222-3333-4444-555555555555"));
        const auto configOnly = window.CaptureWorkspaceWindow();
        QVERIFY(!configOnly.tabs.empty());
        QCOMPARE(configOnly.tabs.front().sourceMode, SourceMode::ConfigOnly);
        QCOMPARE(configOnly.tabs.front().sessionUuid, session->RestorableSessionUuid());
    }

    static void TestCaptureWorkspaceRecordsAutomaticTabLabels()
    {
        MainWindow window;
        LogSession *session = window.activeSession();
        QVERIFY(session != nullptr);
        if (session == nullptr)
        {
            return;
        }

        loglib::LogConfiguration::Source fileSource;
        fileSource.kind = loglib::LogConfiguration::Source::Kind::File;
        fileSource.locators = {std::string{"C:/logs/app.log"}, std::string{"C:/logs/app.log.1"}};
        session->MutableCurrentSource() = fileSource;
        session->NotifyPresentationChanged();
        const auto captured = window.CaptureWorkspaceWindow();
        QVERIFY(!captured.tabs.empty());
        QCOMPARE(captured.tabs.front().label, QStringLiteral("app.log + 1 more"));
        QVERIFY(captured.tabs.front().customLabel.isEmpty());
    }

    static void TestCustomTabRenamePersistsInWorkspaceSnapshot()
    {
        MainWindow window;
        LogSession *session = window.activeSession();
        QVERIFY(session != nullptr);
        if (session == nullptr)
        {
            return;
        }

        loglib::LogConfiguration::Source fileSource;
        fileSource.kind = loglib::LogConfiguration::Source::Kind::File;
        fileSource.locators = {std::string{"C:/logs/app.log"}};
        session->MutableCurrentSource() = fileSource;
        session->NotifyPresentationChanged();
        window.RenameTabForTest(0, QStringLiteral("Incident 42"));
        QCOMPARE(window.TabWidgetForTest()->tabText(0), QStringLiteral("Incident 42"));
        QCOMPARE(session->CustomTabLabel(), QStringLiteral("Incident 42"));

        const auto captured = window.CaptureWorkspaceWindow();
        QVERIFY(!captured.tabs.empty());
        QCOMPARE(captured.tabs.front().customLabel, QStringLiteral("Incident 42"));
        QCOMPARE(captured.tabs.front().label, QStringLiteral("Incident 42"));

        window.RenameTabForTest(0, QString{});
        QCOMPARE(session->CustomTabLabel(), QString{});
        QCOMPARE(window.TabWidgetForTest()->tabText(0), QStringLiteral("app.log"));
    }

    static void TestWorkspaceRestoreAppliesCustomTabLabel()
    {
        slv::persistence::WorkspaceWindow snapshot;
        snapshot.windowUuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
        snapshot.tabs.resize(1);
        snapshot.tabs[0].label = QStringLiteral("app.log");
        snapshot.tabs[0].customLabel = QStringLiteral("Incident 42");
        snapshot.tabs[0].sourceMode = slv::persistence::SourceMode::File;

        MainWindow restored;
        restored.SetSuppressDialogsForTest(true);
        restored.ApplyWorkspaceWindow(snapshot, /*generation=*/0);
        QCOMPARE(restored.TabCount(), 1);
        QCOMPARE(restored.TabWidgetForTest()->tabText(0), QStringLiteral("Incident 42"));
        QVERIFY(restored.activeSession() != nullptr);
        QCOMPARE(restored.activeSession()->CustomTabLabel(), QStringLiteral("Incident 42"));
    }

    static void TestNewSessionClearsCustomTabLabel()
    {
        MainWindow window;
        window.SetSuppressDialogsForTest(true);
        window.RenameTabForTest(0, QStringLiteral("Keep this"));
        QCOMPARE(window.TabWidgetForTest()->tabText(0), QStringLiteral("Keep this"));
        window.NewSessionForTest();
        QVERIFY(window.activeSession() != nullptr);
        QVERIFY(window.activeSession()->CustomTabLabel().isEmpty());
        QVERIFY(window.TabWidgetForTest()->tabText(0).startsWith(QStringLiteral("Untitled")));
    }

    static void TestTabBarF2StartsInlineRename()
    {
        MainWindow window;
        QVERIFY(window.TabWidgetForTest() != nullptr);
        QTabBar *bar = window.TabWidgetForTest()->tabBar();
        QVERIFY(bar != nullptr);
        bar->setFocus(Qt::OtherFocusReason);

        QKeyEvent overrideEvent(QEvent::ShortcutOverride, Qt::Key_F2, Qt::NoModifier);
        QApplication::sendEvent(bar, &overrideEvent);
        QVERIFY2(overrideEvent.isAccepted(), "F2 on the tab strip must shadow jump-to-next-anchor");

        window.StartTabRenameForTest(0);
        auto *editor = bar->findChild<QLineEdit *>(QStringLiteral("tabRenameEditor"));
        QVERIFY(editor != nullptr);
        QCOMPARE(editor->text(), QStringLiteral("Untitled"));
        editor->setText(QStringLiteral("Renamed"));
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(editor, &enter);
        QTRY_COMPARE(window.TabWidgetForTest()->tabText(0), QStringLiteral("Renamed"));
        QVERIFY(window.activeSession() != nullptr);
        QCOMPARE(window.activeSession()->CustomTabLabel(), QStringLiteral("Renamed"));
    }

    // `NewSession()` must
    // route through `PrepareSessionClose`, and the
    // test-only forwarder `NewSessionForTest()` must locally
    // suppress dialogs so fixtures that don't opt into
    // `SetSuppressDialogsForTest(true)` do not hang on a modal prompt.

    static void TestNewSessionForTestPreservesPriorSuppressState()
    {
        MainWindow window;
        // Offscreen construction may start suppressed. Pin `false`
        // so this covers restoring a production-like caller.
        window.SetSuppressDialogsForTest(false);
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

    // Autosave walks hosted sessions in place. It must not activate
    // tabs or rebind shared docks.

    static void TestAutoSaveAllHostedSessionsPreservesActiveTabIndex()
    {
        MainWindow window;
        window.SetSuppressDialogsForTest(true);
        window.AddNewTabForTest(/*makeActive=*/false);
        window.AddNewTabForTest(/*makeActive=*/false);
        QCOMPARE(window.TabCount(), 3);

        window.ActivateTabForTest(1);
        QCOMPARE(window.TabWidgetForTest()->currentIndex(), 1);

        auto *histogram = window.findChild<HistogramDock *>();
        auto *findDock = window.findChild<FindDock *>();
        QVERIFY(histogram != nullptr && findDock != nullptr);
        LogSession *const boundHistogram = histogram->boundSessionForTest();
        LogSession *const boundFind = findDock->boundSessionForTest();
        LogSession *const active = window.activeSession();

        QSignalSpy tabChanged(window.TabWidgetForTest(), &QTabWidget::currentChanged);
        window.AutoSaveAllHostedSessions(/*publishOpenWindow=*/false);

        QCOMPARE(window.TabWidgetForTest()->currentIndex(), 1);
        QCOMPARE(tabChanged.count(), 0);
        QCOMPARE(window.activeSession(), active);
        QCOMPARE(histogram->boundSessionForTest(), boundHistogram);
        QCOMPARE(findDock->boundSessionForTest(), boundFind);
    }

    static void TestTabSwitchPreservesSessionRowsFiltersAndSources()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString pathA = WriteJsonlFixture(temp.filePath(QStringLiteral("a.jsonl")), 200);
        const QString pathB = WriteJsonlFixture(temp.filePath(QStringLiteral("b.jsonl")), 300);
        QVERIFY(!pathA.isEmpty() && !pathB.isEmpty());

        MainWindow window;
        window.SetSuppressDialogsForTest(true);
        LoadFileIntoActiveTab(window, pathA);
        AddContainsFilter(window, QStringLiteral("1"));

        LogSession *sessionA = window.SessionAtTab(0);
        QVERIFY(sessionA != nullptr && sessionA->Model() != nullptr && sessionA->FilterProxy() != nullptr);
        const int rowsA = sessionA->Model()->rowCount();
        const int filteredA = sessionA->FilterProxy()->rowCount();
        QVERIFY(sessionA->CurrentSource().has_value());
        const auto locatorsA = sessionA->CurrentSource()->locators;
        QVERIFY(!sessionA->SimpleLeaves().empty());
        QVERIFY(filteredA < rowsA);

        window.AddNewTabForTest(/*makeActive=*/true);
        LoadFileIntoActiveTab(window, pathB);
        AddContainsFilter(window, QStringLiteral("2"));

        LogSession *sessionB = window.SessionAtTab(1);
        QVERIFY(sessionB != nullptr && sessionB->Model() != nullptr && sessionB->FilterProxy() != nullptr);
        const int rowsB = sessionB->Model()->rowCount();
        const int filteredB = sessionB->FilterProxy()->rowCount();
        QVERIFY(sessionB->CurrentSource().has_value());
        const auto locatorsB = sessionB->CurrentSource()->locators;
        QVERIFY(!sessionB->SimpleLeaves().empty());
        QVERIFY(filteredB < rowsB);

        QSignalSpy resetA(sessionA->Model(), &QAbstractItemModel::modelReset);
        QSignalSpy resetB(sessionB->Model(), &QAbstractItemModel::modelReset);
        QSignalSpy streamA(sessionA->Model(), &LogModel::streamingFinished);
        QSignalSpy streamB(sessionB->Model(), &LogModel::streamingFinished);
        QSignalSpy layoutA(sessionA->FilterProxy(), &QAbstractItemModel::layoutAboutToBeChanged);
        QSignalSpy layoutB(sessionB->FilterProxy(), &QAbstractItemModel::layoutAboutToBeChanged);

        window.ActivateTabForTest(0);
        window.ActivateTabForTest(1);
        window.ActivateTabForTest(0);

        QCOMPARE(resetA.count(), 0);
        QCOMPARE(resetB.count(), 0);
        QCOMPARE(streamA.count(), 0);
        QCOMPARE(streamB.count(), 0);
        QCOMPARE(layoutA.count(), 0);
        QCOMPARE(layoutB.count(), 0);
        QCOMPARE(sessionA->Model()->rowCount(), rowsA);
        QCOMPARE(sessionB->Model()->rowCount(), rowsB);
        QCOMPARE(sessionA->FilterProxy()->rowCount(), filteredA);
        QCOMPARE(sessionB->FilterProxy()->rowCount(), filteredB);
        QCOMPARE(sessionA->CurrentSource()->locators, locatorsA);
        QCOMPARE(sessionB->CurrentSource()->locators, locatorsB);
    }

    static void TestTabSwitchDefersHiddenHistogramRebuild()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString pathA = WriteJsonlFixture(temp.filePath(QStringLiteral("hist-a.jsonl")), 80);
        const QString pathB = WriteJsonlFixture(temp.filePath(QStringLiteral("hist-b.jsonl")), 90);
        QVERIFY(!pathA.isEmpty() && !pathB.isEmpty());

        MainWindow window;
        window.SetSuppressDialogsForTest(true);
        LoadFileIntoActiveTab(window, pathA);
        window.AddNewTabForTest(/*makeActive=*/true);
        LoadFileIntoActiveTab(window, pathB);

        auto *histogram = window.findChild<HistogramDock *>();
        QVERIFY(histogram != nullptr && histogram->ModelForTest() != nullptr);
        histogram->hide();
        QVERIFY(!histogram->isVisible());

        QSignalSpy bucketsChanged(histogram->ModelForTest(), &HistogramModel::bucketsChanged);
        window.ActivateTabForTest(0);
        QVERIFY(histogram->ModelForTest()->IsDeferredBindPending());
        QCOMPARE(bucketsChanged.count(), 0);

        window.ActivateTabForTest(1);
        QVERIFY(histogram->ModelForTest()->IsDeferredBindPending());
        QCOMPARE(bucketsChanged.count(), 0);
        QCOMPARE(histogram->boundSessionForTest(), window.SessionAtTab(1));
    }

    // `RestorableActiveSessionUuid` returns one UUID per window,
    // while `RestorableHostedSessionUuids` returns per-tab UUIDs.

    static void TestRestorableActiveSessionUuidIsSingleValued()
    {
        MainWindow window;
        window.SetSuppressDialogsForTest(true);
        window.AddNewTabForTest(/*makeActive=*/false);
        window.AddNewTabForTest(/*makeActive=*/false);
        QCOMPARE(window.TabCount(), 3);

        // Give every tab a UUID so the active-only and all-hosted
        // accessors have observably different results.
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

        // The grouped workspace record uses every hosted tab's UUID.
        QCOMPARE(window.RestorableHostedSessionUuids().size(), 3);
    }

    static void TestExportingTabKeepsOwnershipAfterSwitch()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString pathA = WriteJsonlFixture(temp.filePath(QStringLiteral("a.jsonl")), 4000);
        const QString pathB = WriteJsonlFixture(temp.filePath(QStringLiteral("b.jsonl")), 8);
        QVERIFY(!pathA.isEmpty());
        QVERIFY(!pathB.isEmpty());

        MainWindow window;
        window.SetSuppressDialogsForTest(true);
        LoadFileIntoActiveTab(window, pathA);
        window.AddNewTabForTest(/*makeActive=*/true);
        LoadFileIntoActiveTab(window, pathB);

        LogSession *sessionA = window.SessionAtTab(0);
        LogSession *sessionB = window.SessionAtTab(1);
        QVERIFY(sessionA != nullptr && sessionB != nullptr);
        LogSessionView *viewA = qobject_cast<LogSessionView *>(window.TabWidgetForTest()->widget(0));
        LogSessionView *viewB = qobject_cast<LogSessionView *>(window.TabWidgetForTest()->widget(1));
        QVERIFY(viewA != nullptr && viewB != nullptr);

        window.ActivateTabForTest(0);
        window.ExportSessionBundleToPathForTest(temp.filePath(QStringLiteral("a.slvbundle")));
        QVERIFY(sessionA->IsExportInFlight());
        QVERIFY(!viewA->IsContentEnabled());

        window.ActivateTabForTest(1);
        QCOMPARE(window.activeSession(), sessionB);
        QVERIFY(viewB->IsContentEnabled());
        QVERIFY(sessionA->IsExportInFlight());
        QVERIFY(!viewA->IsContentEnabled());
        QVERIFY(viewA->IsOperationProgressVisible());

        QTRY_VERIFY_WITH_TIMEOUT(!sessionA->IsExportInFlight(), 15000);
        QVERIFY(viewA->IsContentEnabled());
        QVERIFY(viewB->IsContentEnabled());
        QVERIFY(!viewA->IsOperationProgressVisible());
    }

    static void TestConcurrentExportsLockOnlyTheirOwnViews()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString pathA = WriteJsonlFixture(temp.filePath(QStringLiteral("a.jsonl")), 6000);
        const QString pathB = WriteJsonlFixture(temp.filePath(QStringLiteral("b.jsonl")), 6000);
        QVERIFY(!pathA.isEmpty());
        QVERIFY(!pathB.isEmpty());

        MainWindow window;
        window.SetSuppressDialogsForTest(true);
        LoadFileIntoActiveTab(window, pathA);
        window.AddNewTabForTest(/*makeActive=*/true);
        LoadFileIntoActiveTab(window, pathB);

        LogSession *sessionA = window.SessionAtTab(0);
        LogSession *sessionB = window.SessionAtTab(1);
        QVERIFY(sessionA != nullptr && sessionB != nullptr);
        LogSessionView *viewA = qobject_cast<LogSessionView *>(window.TabWidgetForTest()->widget(0));
        LogSessionView *viewB = qobject_cast<LogSessionView *>(window.TabWidgetForTest()->widget(1));
        QVERIFY(viewA != nullptr && viewB != nullptr);

        window.ActivateTabForTest(0);
        window.ExportSessionBundleToPathForTest(temp.filePath(QStringLiteral("a.slvbundle")));
        window.ActivateTabForTest(1);
        window.ExportSessionBundleToPathForTest(temp.filePath(QStringLiteral("b.slvbundle")));

        QVERIFY(sessionA->IsExportInFlight() || sessionB->IsExportInFlight());
        if (sessionA->IsExportInFlight())
        {
            QVERIFY(!viewA->IsContentEnabled());
        }
        if (sessionB->IsExportInFlight())
        {
            QVERIFY(!viewB->IsContentEnabled());
        }
        if (sessionA->IsExportInFlight() && sessionB->IsExportInFlight())
        {
            QVERIFY(!viewA->IsContentEnabled());
            QVERIFY(!viewB->IsContentEnabled());
        }

        QTRY_VERIFY_WITH_TIMEOUT(!sessionA->IsExportInFlight() && !sessionB->IsExportInFlight(), 20000);
        QVERIFY(viewA->IsContentEnabled());
        QVERIFY(viewB->IsContentEnabled());
        QVERIFY(!viewA->IsOperationProgressVisible());
        QVERIFY(!viewB->IsOperationProgressVisible());
    }

    static void TestConcurrentExportCompletionOrderAThenB()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString pathA = WriteJsonlFixture(temp.filePath(QStringLiteral("a.jsonl")), 200);
        const QString pathB = WriteJsonlFixture(temp.filePath(QStringLiteral("b.jsonl")), 8000);
        QVERIFY(!pathA.isEmpty());
        QVERIFY(!pathB.isEmpty());

        MainWindow window;
        window.SetSuppressDialogsForTest(true);
        LoadFileIntoActiveTab(window, pathA);
        window.AddNewTabForTest(/*makeActive=*/true);
        LoadFileIntoActiveTab(window, pathB);

        LogSession *sessionA = window.SessionAtTab(0);
        LogSession *sessionB = window.SessionAtTab(1);
        QVERIFY(sessionA != nullptr && sessionB != nullptr);
        LogSessionView *viewA = qobject_cast<LogSessionView *>(window.TabWidgetForTest()->widget(0));
        LogSessionView *viewB = qobject_cast<LogSessionView *>(window.TabWidgetForTest()->widget(1));
        QVERIFY(viewA != nullptr && viewB != nullptr);

        window.ActivateTabForTest(0);
        window.ExportSessionBundleToPathForTest(temp.filePath(QStringLiteral("a.slvbundle")));
        window.ActivateTabForTest(1);
        window.ExportSessionBundleToPathForTest(temp.filePath(QStringLiteral("b.slvbundle")));

        QTRY_VERIFY_WITH_TIMEOUT(!sessionA->IsExportInFlight(), 15000);
        QVERIFY(viewA->IsContentEnabled());
        if (sessionB->IsExportInFlight())
        {
            QVERIFY(!viewB->IsContentEnabled());
        }
        QTRY_VERIFY_WITH_TIMEOUT(!sessionB->IsExportInFlight(), 20000);
        QVERIFY(viewB->IsContentEnabled());
    }

    static void TestConcurrentExportCompletionOrderBThenA()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString pathA = WriteJsonlFixture(temp.filePath(QStringLiteral("a.jsonl")), 8000);
        const QString pathB = WriteJsonlFixture(temp.filePath(QStringLiteral("b.jsonl")), 200);
        QVERIFY(!pathA.isEmpty());
        QVERIFY(!pathB.isEmpty());

        MainWindow window;
        window.SetSuppressDialogsForTest(true);
        LoadFileIntoActiveTab(window, pathA);
        window.AddNewTabForTest(/*makeActive=*/true);
        LoadFileIntoActiveTab(window, pathB);

        LogSession *sessionA = window.SessionAtTab(0);
        LogSession *sessionB = window.SessionAtTab(1);
        QVERIFY(sessionA != nullptr && sessionB != nullptr);
        LogSessionView *viewA = qobject_cast<LogSessionView *>(window.TabWidgetForTest()->widget(0));
        LogSessionView *viewB = qobject_cast<LogSessionView *>(window.TabWidgetForTest()->widget(1));
        QVERIFY(viewA != nullptr && viewB != nullptr);

        window.ActivateTabForTest(0);
        window.ExportSessionBundleToPathForTest(temp.filePath(QStringLiteral("a.slvbundle")));
        window.ActivateTabForTest(1);
        window.ExportSessionBundleToPathForTest(temp.filePath(QStringLiteral("b.slvbundle")));

        QTRY_VERIFY_WITH_TIMEOUT(!sessionB->IsExportInFlight(), 15000);
        QVERIFY(viewB->IsContentEnabled());
        if (sessionA->IsExportInFlight())
        {
            QVERIFY(!viewA->IsContentEnabled());
        }
        QTRY_VERIFY_WITH_TIMEOUT(!sessionA->IsExportInFlight(), 20000);
        QVERIFY(viewA->IsContentEnabled());
    }

    static void TestDecompressingTabKeepsOwnershipAfterSwitch()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString pathA = WriteGzipJsonl(temp.filePath(QStringLiteral("a.jsonl.gz")), 8000);
        const QString pathB = WriteJsonlFixture(temp.filePath(QStringLiteral("b.jsonl")), 8);
        QVERIFY(!pathA.isEmpty());
        QVERIFY(!pathB.isEmpty());

        MainWindow window;
        window.SetSuppressDialogsForTest(true);
        window.OpenFilesForTest({pathA}, MainWindow::OpenMode::Replace);
        LogSession *sessionA = window.SessionAtTab(0);
        QVERIFY(sessionA != nullptr);
        QVERIFY(sessionA->IsDecompressionInFlight());
        LogSessionView *viewA = qobject_cast<LogSessionView *>(window.TabWidgetForTest()->widget(0));
        QVERIFY(viewA != nullptr);
        QVERIFY(!viewA->IsContentEnabled());

        window.AddNewTabForTest(/*makeActive=*/true);
        LoadFileIntoActiveTab(window, pathB);
        LogSession *sessionB = window.SessionAtTab(1);
        LogSessionView *viewB = qobject_cast<LogSessionView *>(window.TabWidgetForTest()->widget(1));
        QVERIFY(sessionB != nullptr && viewB != nullptr);
        QCOMPARE(window.activeSession(), sessionB);
        QVERIFY(viewB->IsContentEnabled());
        if (sessionA->IsDecompressionInFlight())
        {
            QVERIFY(!viewA->IsContentEnabled());
            QVERIFY(viewA->IsOperationProgressVisible());
        }

        QTRY_VERIFY_WITH_TIMEOUT(!sessionA->IsDecompressionInFlight(), 15000);
        QVERIFY(viewA->IsContentEnabled());
        QVERIFY(viewB->IsContentEnabled());
    }

    static void TestNewSessionCancelsDecompressionBeforeModelReset()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString path = WriteGzipJsonl(temp.filePath(QStringLiteral("in-flight.jsonl.gz")), 12000);
        QVERIFY(!path.isEmpty());

        MainWindow window;
        window.SetSuppressDialogsForTest(true);
        window.OpenFilesForTest({path}, MainWindow::OpenMode::Replace);
        LogSession *session = window.activeSession();
        QVERIFY(session != nullptr);
        QVERIFY(session->IsDecompressionInFlight());

        window.NewSessionForTest();
        QVERIFY(!session->IsDecompressionInFlight());
        QVERIFY(session->Model() != nullptr);
        QCOMPARE(session->Model()->rowCount(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(!session->IsDecompressionInFlight(), 5000);
        QCOMPARE(session->Model()->rowCount(), 0);
    }

    static void TestConcurrentDecompressionLocksOnlyOwnViews()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString pathA = WriteGzipJsonl(temp.filePath(QStringLiteral("a.jsonl.gz")), 12000);
        const QString pathB = WriteGzipJsonl(temp.filePath(QStringLiteral("b.jsonl.gz")), 12000);
        QVERIFY(!pathA.isEmpty());
        QVERIFY(!pathB.isEmpty());

        MainWindow window;
        window.SetSuppressDialogsForTest(true);
        window.OpenFilesForTest({pathA}, MainWindow::OpenMode::Replace);
        window.AddNewTabForTest(/*makeActive=*/true);
        window.OpenFilesForTest({pathB}, MainWindow::OpenMode::Replace);

        LogSession *sessionA = window.SessionAtTab(0);
        LogSession *sessionB = window.SessionAtTab(1);
        QVERIFY(sessionA != nullptr && sessionB != nullptr);
        LogSessionView *viewA = qobject_cast<LogSessionView *>(window.TabWidgetForTest()->widget(0));
        LogSessionView *viewB = qobject_cast<LogSessionView *>(window.TabWidgetForTest()->widget(1));
        QVERIFY(viewA != nullptr && viewB != nullptr);

        QVERIFY(sessionA->IsDecompressionInFlight() || sessionB->IsDecompressionInFlight());
        if (sessionA->IsDecompressionInFlight())
        {
            QVERIFY(!viewA->IsContentEnabled());
        }
        if (sessionB->IsDecompressionInFlight())
        {
            QVERIFY(!viewB->IsContentEnabled());
        }

        QTRY_VERIFY_WITH_TIMEOUT(!sessionA->IsDecompressionInFlight() && !sessionB->IsDecompressionInFlight(), 20000);
        QVERIFY(viewA->IsContentEnabled());
        QVERIFY(viewB->IsContentEnabled());
    }

    static void TestConcurrentStreamCompletionAcrossTabs()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString pathA = WriteJsonlFixture(temp.filePath(QStringLiteral("a.jsonl")), 20000);
        const QString pathB = WriteJsonlFixture(temp.filePath(QStringLiteral("b.jsonl")), 20000);
        QVERIFY(!pathA.isEmpty());
        QVERIFY(!pathB.isEmpty());

        MainWindow window;
        window.SetSuppressDialogsForTest(true);
        window.OpenFilesForTest({pathA}, MainWindow::OpenMode::Replace);
        window.AddNewTabForTest(/*makeActive=*/true);
        window.OpenFilesForTest({pathB}, MainWindow::OpenMode::Replace);

        LogSession *sessionA = window.SessionAtTab(0);
        LogSession *sessionB = window.SessionAtTab(1);
        QVERIFY(sessionA != nullptr && sessionB != nullptr);
        LogModel *modelA = sessionA->Model();
        LogModel *modelB = sessionB->Model();
        QVERIFY(modelA != nullptr && modelB != nullptr);

        QTRY_VERIFY_WITH_TIMEOUT(!modelA->IsStreamingActive() && !modelB->IsStreamingActive(), 20000);
        QVERIFY(modelA->rowCount() > 0);
        QVERIFY(modelB->rowCount() > 0);
        QCOMPARE(window.activeSession(), sessionB);
    }

    static void TestBackgroundBundleCompletionDoesNotMutateActiveShellUi()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString bundlePath = WriteBundleFixture(
            temp.filePath(QStringLiteral("origin.slvbundle")), 200, QStringLiteral("bundle-only-token")
        );
        const QString activePath = WriteJsonlFixture(temp.filePath(QStringLiteral("active.jsonl")), 12);
        QVERIFY(!bundlePath.isEmpty());
        QVERIFY(!activePath.isEmpty());

        MainWindow window;
        window.SetSuppressDialogsForTest(true);
        window.AddNewTabForTest(/*makeActive=*/true);
        LoadFileIntoActiveTab(window, activePath);
        AddContainsFilter(window, QStringLiteral("active-tab-only"));

        LogSession *sessionA = window.SessionAtTab(0);
        LogSession *sessionB = window.SessionAtTab(1);
        QVERIFY(sessionA != nullptr && sessionB != nullptr);
        QCOMPARE(window.activeSession(), sessionB);

        const QStringList activeFilterTitles = FilterMenuTitles(window);
        QVERIFY2(
            activeFilterTitles.join(QLatin1Char('|')).contains(QStringLiteral("active-tab-only")),
            "The selected tab must own a distinctive Filters menu entry before background completion."
        );
        auto *clearFilters = window.findChild<QAction *>(QStringLiteral("actionClearAllFilters"));
        auto *rotationHistory = window.findChild<QAction *>(QStringLiteral("actionAutoDetectRotationHistory"));
        auto *pauseStream = window.findChild<QAction *>(QStringLiteral("actionPauseStream"));
        auto *streamToolbar = window.findChild<QToolBar *>(QStringLiteral("streamToolbar"));
        auto *parseErrorsDock = window.findChild<QDockWidget *>(QStringLiteral("parseErrorsDock"));
        auto *histogramDock = window.findChild<QDockWidget *>(QStringLiteral("histogramDock"));
        auto *findDock = window.findChild<QDockWidget *>(QStringLiteral("findDock"));
        QVERIFY(clearFilters != nullptr && rotationHistory != nullptr && pauseStream != nullptr);
        QVERIFY(streamToolbar != nullptr && parseErrorsDock != nullptr);
        const bool clearEnabled = clearFilters->isEnabled();
        const bool rotationChecked = rotationHistory->isChecked();
        const bool pauseEnabled = pauseStream->isEnabled();
        const bool streamVisible = streamToolbar->isVisible();
        const bool parseVisible = parseErrorsDock->isVisible();
        const bool histogramVisible = histogramDock != nullptr && histogramDock->isVisible();
        const bool findVisible = findDock != nullptr && findDock->isVisible();
        const QString statusText = window.statusBar()->currentMessage();
        const QString title = window.windowTitle();

        window.ActivateTabForTest(0);
        LogModel *originModel = sessionA->Model();
        QVERIFY(originModel != nullptr);
        const QSignalSpy originFinished(originModel, &LogModel::streamingFinished);
        QCOMPARE(
            window.OpenMixedFilesForTest({bundlePath}, MainWindow::OpenMode::Replace),
            MainWindow::MixedInputDispatch::QueuedLogsOnly
        );
        window.ActivateTabForTest(1);
        QCOMPARE(window.activeSession(), sessionB);

        QTRY_VERIFY_WITH_TIMEOUT(originFinished.count() >= 1, 20000);
        QVERIFY(originModel->rowCount() > 0);

        QCOMPARE(FilterMenuTitles(window), activeFilterTitles);
        QCOMPARE(clearFilters->isEnabled(), clearEnabled);
        QCOMPARE(rotationHistory->isChecked(), rotationChecked);
        QCOMPARE(pauseStream->isEnabled(), pauseEnabled);
        QCOMPARE(streamToolbar->isVisible(), streamVisible);
        QCOMPARE(parseErrorsDock->isVisible(), parseVisible);
        if (histogramDock != nullptr)
        {
            QCOMPARE(histogramDock->isVisible(), histogramVisible);
        }
        if (findDock != nullptr)
        {
            QCOMPARE(findDock->isVisible(), findVisible);
        }
        QCOMPARE(window.statusBar()->currentMessage(), statusText);
        QCOMPARE(window.windowTitle(), title);
        QCOMPARE(window.activeSession(), sessionB);
        QVERIFY2(
            !FilterMenuTitles(window).join(QLatin1Char('|')).contains(QStringLiteral("bundle-only-token")),
            "Background bundle filters must not appear in the selected tab's Filters menu."
        );

        const QStringList originLeafStrings = SimpleLeafFilterStrings(*sessionA);
        const bool originHasBundleFilter =
            originLeafStrings.contains(QStringLiteral("bundle-only-token")) ||
            ExpressionContainsFilterString(originModel->Configuration().expression, "bundle-only-token");
        QVERIFY2(
            originHasBundleFilter,
            qPrintable(QStringLiteral(
                           "The originating session must still receive the bundle's embedded filters. "
                           "simpleLeaves=[%1] matchAll=%2 dropped=%3 rows=%4"
            )
                           .arg(originLeafStrings.join(QLatin1Char(',')))
                           .arg(loglib::IsMatchAll(originModel->Configuration().expression))
                           .arg(window.LastDroppedFilterCountForTest())
                           .arg(originModel->rowCount()))
        );
    }

    static void TestBackgroundExportCompletionQueuesStatusOnOrigin()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString pathA = WriteJsonlFixture(temp.filePath(QStringLiteral("a.jsonl")), 4000);
        const QString pathB = WriteJsonlFixture(temp.filePath(QStringLiteral("b.jsonl")), 8);
        QVERIFY(!pathA.isEmpty() && !pathB.isEmpty());

        MainWindow window;
        window.SetSuppressDialogsForTest(true);
        LoadFileIntoActiveTab(window, pathA);
        window.AddNewTabForTest(/*makeActive=*/true);
        LoadFileIntoActiveTab(window, pathB);
        AddContainsFilter(window, QStringLiteral("active-tab-only"));

        LogSession *sessionA = window.SessionAtTab(0);
        LogSession *sessionB = window.SessionAtTab(1);
        QVERIFY(sessionA != nullptr && sessionB != nullptr);
        QCOMPARE(window.activeSession(), sessionB);

        const QStringList activeFilterTitles = FilterMenuTitles(window);
        const QString statusText = window.statusBar()->currentMessage();

        window.ActivateTabForTest(0);
        window.ExportSessionBundleToPathForTest(temp.filePath(QStringLiteral("a.slvbundle")));
        QVERIFY(sessionA->IsExportInFlight());
        window.ActivateTabForTest(1);
        QCOMPARE(window.activeSession(), sessionB);
        QTRY_VERIFY_WITH_TIMEOUT(!sessionA->IsExportInFlight(), 15000);

        QCOMPARE(window.activeSession(), sessionB);
        QCOMPARE(FilterMenuTitles(window), activeFilterTitles);
        QCOMPARE(window.statusBar()->currentMessage(), statusText);
        QVERIFY2(
            !sessionA->PendingPresentation().isEmpty(),
            "A background export completion must queue presentation work on the originating session."
        );
        QVERIFY(!sessionA->PendingPresentation().statusMessage.isEmpty());
        QVERIFY(sessionB->PendingPresentation().isEmpty());
    }

    static void TestBackgroundFailureIsRecordedOnOriginatingTab()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString gzipPath = WriteGzipJsonl(temp.filePath(QStringLiteral("corrupt.jsonl.gz")), 8000);
        QVERIFY(!gzipPath.isEmpty());
        QFile gzipFile(gzipPath);
        QVERIFY(gzipFile.open(QIODevice::ReadWrite));
        QVERIFY(gzipFile.resize(24));
        gzipFile.close();

        const QString pathB = WriteJsonlFixture(temp.filePath(QStringLiteral("b.jsonl")), 8);
        QVERIFY(!pathB.isEmpty());

        MainWindow window;
        window.SetSuppressDialogsForTest(true);
        window.OpenFilesForTest({gzipPath}, MainWindow::OpenMode::Replace);
        LogSession *sessionA = window.SessionAtTab(0);
        QVERIFY(sessionA != nullptr);
        window.AddNewTabForTest(/*makeActive=*/true);
        LoadFileIntoActiveTab(window, pathB);
        LogSession *sessionB = window.SessionAtTab(1);
        QVERIFY(sessionB != nullptr);
        QCOMPARE(window.activeSession(), sessionB);

        QTRY_VERIFY_WITH_TIMEOUT(!sessionA->IsDecompressionInFlight(), 15000);
        QVERIFY2(
            !sessionA->ParseErrorLog().batches.empty() || !sessionA->PendingPresentation().isEmpty() ||
                !sessionA->PendingDecompressionErrors().empty() || !sessionA->PendingOpenErrors().empty(),
            "A background decompression failure must be recorded on the originating session."
        );
        QVERIFY(sessionB->ParseErrorLog().batches.empty());
        QVERIFY(sessionB->PendingPresentation().isEmpty());
    }

    static void TestHighlightEditorDraftSurvivesTabSwitchWithoutCommitting()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString pathA = WriteJsonlFixture(temp.filePath(QStringLiteral("a.jsonl")), 8);
        const QString pathB = WriteJsonlFixture(temp.filePath(QStringLiteral("b.jsonl")), 8);
        QVERIFY(!pathA.isEmpty());
        QVERIFY(!pathB.isEmpty());

        MainWindow window;
        window.SetSuppressDialogsForTest(true);
        LoadFileIntoActiveTab(window, pathA);
        LogSession *sessionA = window.SessionAtTab(0);
        QVERIFY(sessionA != nullptr);
        window.AddNewTabForTest(/*makeActive=*/true);
        LoadFileIntoActiveTab(window, pathB);
        LogSession *sessionB = window.SessionAtTab(1);
        QVERIFY(sessionB != nullptr);
        window.ActivateTabForTest(0);
        QCOMPARE(window.activeSession(), sessionA);
        sessionA->ClearFiltersDirty();
        sessionB->ClearFiltersDirty();

        window.OpenHighlightRulesEditorForTest();
        HighlightRulesEditor *editor = window.HighlightRulesEditorForTest();
        QVERIFY(editor != nullptr);

        const auto committedA = sessionA->Highlights()->Rules();
        const auto committedB = sessionB->Highlights()->Rules();
        const HighlightRulesEditorDraft dirty = MakeDirtyHighlightDraft(committedA);
        editor->RestoreDraft(dirty);
        QVERIFY(editor->IsDirty());
        QVERIFY(!sessionA->IsFiltersDirty());
        QVERIFY(!sessionA->HasDirtyHighlightEditorDraft());

        window.ActivateTabForTest(1);
        QCOMPARE(window.activeSession(), sessionB);
        QCOMPARE(window.TabCount(), 2);
        QCOMPARE(window.HighlightRulesEditorForTest(), editor);
        QVERIFY(sessionA->HasDirtyHighlightEditorDraft());
        QVERIFY(!sessionA->IsFiltersDirty());
        QCOMPARE(sessionA->CloseDecision(), SessionCloseDecision::Prompt);
        QCOMPARE(sessionA->Highlights()->Rules(), committedA);
        QCOMPARE(sessionA->Model()->Configuration().highlightRules, committedA);
        QCOMPARE(sessionB->Highlights()->Rules(), committedB);
        QCOMPARE(sessionB->Model()->Configuration().highlightRules, committedB);
        QVERIFY(!editor->IsDirty());
        QCOMPARE(editor->CaptureDraft().localRules, committedB);

        window.ActivateTabForTest(0);
        QCOMPARE(window.activeSession(), sessionA);
        QCOMPARE(window.HighlightRulesEditorForTest(), editor);
        QVERIFY(editor->IsDirty());
        const HighlightRulesEditorDraft restored = editor->CaptureDraft();
        QCOMPARE(restored.localRules, dirty.localRules);
        QCOMPARE(restored.baseline, dirty.baseline);
        QCOMPARE(sessionA->Highlights()->Rules(), committedA);
        QCOMPARE(sessionB->Highlights()->Rules(), committedB);
    }

    static void TestDirtyHighlightEditorPromptsOnCloseWithoutTabSwitch()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString pathA = WriteJsonlFixture(temp.filePath(QStringLiteral("a.jsonl")), 8);
        QVERIFY(!pathA.isEmpty());

        MainWindow window;
        window.SetSuppressDialogsForTest(true);
        LoadFileIntoActiveTab(window, pathA);
        window.AddNewTabForTest(/*makeActive=*/false);
        LogSession *sessionA = window.SessionAtTab(0);
        QVERIFY(sessionA != nullptr);
        window.ActivateTabForTest(0);
        sessionA->ClearFiltersDirty();

        window.OpenHighlightRulesEditorForTest();
        HighlightRulesEditor *editor = window.HighlightRulesEditorForTest();
        QVERIFY(editor != nullptr);
        editor->RestoreDraft(MakeDirtyHighlightDraft(sessionA->Highlights()->Rules()));
        QVERIFY(editor->IsDirty());
        QVERIFY(!sessionA->HasDirtyHighlightEditorDraft());

        window.QueueClosePromptChoiceForTest(MainWindow::ClosePromptChoiceForTest::Cancel);
        window.CloseTabForTest(0);
        QCOMPARE(window.TabCount(), 2);
        QCOMPARE(window.SessionAtTab(0), sessionA);
        QVERIFY(sessionA->HasDirtyHighlightEditorDraft());
        QCOMPARE(sessionA->CloseDecision(), SessionCloseDecision::Prompt);
    }

    static void TestColumnsManagerAndDiagnosticsCloseOnTabSwitch()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString pathA = WriteJsonlFixture(temp.filePath(QStringLiteral("a.jsonl")), 8);
        const QString pathB = WriteJsonlFixture(temp.filePath(QStringLiteral("b.jsonl")), 8);
        QVERIFY(!pathA.isEmpty());
        QVERIFY(!pathB.isEmpty());

        MainWindow window;
        window.SetSuppressDialogsForTest(true);
        LoadFileIntoActiveTab(window, pathA);
        LogSession *sessionA = window.SessionAtTab(0);
        QVERIFY(sessionA != nullptr);
        window.AddNewTabForTest(/*makeActive=*/true);
        LoadFileIntoActiveTab(window, pathB);
        LogSession *sessionB = window.SessionAtTab(1);
        QVERIFY(sessionB != nullptr);
        window.ActivateTabForTest(0);

        const std::vector<bool> visibilityB = ColumnVisibility(*sessionB);
        window.ShowColumnsManager();
        QPointer<ColumnsManagerDialog> columnsDialog = window.ColumnsManagerDialogForTest();
        QVERIFY(!columnsDialog.isNull());
        QVERIFY(columnsDialog->isVisible());

        window.ShowConfigurationDiagnosticsForTest();
        QPointer<ConfigurationDiagnosticsDialog> diagnosticsDialog = window.ConfigurationDiagnosticsDialogForTest();
        QVERIFY(!diagnosticsDialog.isNull());
        QVERIFY(diagnosticsDialog->isVisible());

        window.ActivateTabForTest(1);
        QCOMPARE(window.activeSession(), sessionB);
        QVERIFY(window.ColumnsManagerDialogForTest() == nullptr);
        QVERIFY(window.ConfigurationDiagnosticsDialogForTest() == nullptr);
        QCOMPARE(ColumnVisibility(*sessionB), visibilityB);
        QTRY_VERIFY(columnsDialog.isNull());
        QTRY_VERIFY(diagnosticsDialog.isNull());
    }

    static void TestColumnsManagerQueuedChangeDoesNotApplyToNewlySelectedTab()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString pathA = WriteJsonlFixture(temp.filePath(QStringLiteral("a.jsonl")), 8);
        const QString pathB = WriteJsonlFixture(temp.filePath(QStringLiteral("b.jsonl")), 8);
        QVERIFY(!pathA.isEmpty());
        QVERIFY(!pathB.isEmpty());

        MainWindow window;
        window.SetSuppressDialogsForTest(true);
        LoadFileIntoActiveTab(window, pathA);
        LogSession *sessionA = window.SessionAtTab(0);
        QVERIFY(sessionA != nullptr);
        window.AddNewTabForTest(/*makeActive=*/true);
        LoadFileIntoActiveTab(window, pathB);
        LogSession *sessionB = window.SessionAtTab(1);
        QVERIFY(sessionB != nullptr);
        window.ActivateTabForTest(0);

        window.ShowColumnsManager();
        QPointer<ColumnsManagerDialog> dialog = window.ColumnsManagerDialogForTest();
        QVERIFY(!dialog.isNull());
        const std::vector<bool> visibilityA = ColumnVisibility(*sessionA);
        const std::vector<bool> visibilityB = ColumnVisibility(*sessionB);
        QVERIFY(!visibilityA.empty());
        QVERIFY(!visibilityB.empty());

        window.ActivateTabForTest(1);
        QCOMPARE(window.activeSession(), sessionB);
        QVERIFY(!dialog.isNull());
        auto *table = dialog->findChild<QTableWidget *>();
        QVERIFY(table != nullptr);
        constexpr int visibleColumn = 4;
        QTableWidgetItem *item = table->item(0, visibleColumn);
        QVERIFY(item != nullptr);
        item->setCheckState(item->checkState() == Qt::Checked ? Qt::Unchecked : Qt::Checked);

        QCOMPARE(ColumnVisibility(*sessionB), visibilityB);
        QCOMPARE(ColumnVisibility(*sessionA), visibilityA);
        QTRY_VERIFY(dialog.isNull());
    }

    static void TestDialogCallbacksAreNoOpsAfterOriginTabCloses()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString pathA = WriteJsonlFixture(temp.filePath(QStringLiteral("a.jsonl")), 8);
        const QString pathB = WriteJsonlFixture(temp.filePath(QStringLiteral("b.jsonl")), 8);
        QVERIFY(!pathA.isEmpty());
        QVERIFY(!pathB.isEmpty());

        MainWindow window;
        window.SetSuppressDialogsForTest(true);
        LoadFileIntoActiveTab(window, pathA);
        window.AddNewTabForTest(/*makeActive=*/true);
        LoadFileIntoActiveTab(window, pathB);
        LogSession *sessionB = window.SessionAtTab(1);
        QVERIFY(sessionB != nullptr);
        window.ActivateTabForTest(0);

        window.ShowColumnsManager();
        QPointer<ColumnsManagerDialog> columnsDialog = window.ColumnsManagerDialogForTest();
        QVERIFY(!columnsDialog.isNull());
        window.ShowConfigurationDiagnosticsForTest();
        QPointer<ConfigurationDiagnosticsDialog> diagnosticsDialog = window.ConfigurationDiagnosticsDialogForTest();
        QVERIFY(!diagnosticsDialog.isNull());

        const std::vector<bool> visibilityB = ColumnVisibility(*sessionB);
        window.CloseTabForTest(0);
        QCOMPARE(window.TabCount(), 1);
        QCOMPARE(window.activeSession(), sessionB);

        if (!columnsDialog.isNull())
        {
            auto *table = columnsDialog->findChild<QTableWidget *>();
            if (table != nullptr && table->item(0, 4) != nullptr)
            {
                QTableWidgetItem *item = table->item(0, 4);
                item->setCheckState(item->checkState() == Qt::Checked ? Qt::Unchecked : Qt::Checked);
            }
        }
        if (!diagnosticsDialog.isNull())
        {
            diagnosticsDialog->RequestEditColumnForTest(0);
        }

        QCOMPARE(ColumnVisibility(*sessionB), visibilityB);
        QCOMPARE(window.activeSession(), sessionB);
    }

    static void TestTabChromeExposesOperationTextDirtyAndErrors()
    {
        MainWindow window;
        window.SetSuppressDialogsForTest(true);
        LogSession *session = window.activeSession();
        QVERIFY(session != nullptr);
        session->SetStreamingFileName(QStringLiteral("C:/very/long/path/to/application.log"));
        session->SetMode(LogSession::Mode::LiveTail);
        session->SetSourceWaiting(false);
        session->MarkFiltersDirty();
        session->SetStreamingErrorCount(3);

        const QString tabText = window.TabWidgetForTest()->tabText(0);
        QVERIFY(tabText.contains(QStringLiteral("application.log")));
        QVERIFY(tabText.contains(QStringLiteral("Ingesting")));
        QVERIFY(tabText.contains(QStringLiteral("\u25CF")));
        QVERIFY(tabText.contains(QLatin1Char('!')));

        const QString tooltip = window.TabWidgetForTest()->tabToolTip(0);
        QVERIFY(tooltip.contains(QStringLiteral("C:/very/long/path/to/application.log")));
        QVERIFY(tooltip.contains(QStringLiteral("Ingesting")));
        QVERIFY(tooltip.contains(QStringLiteral("Unsaved changes")));
        QVERIFY(tooltip.contains(QStringLiteral("3")));
    }

    static void TestTabDragReorderMovesHostedSessions()
    {
        MainWindow window;
        window.AddNewTabForTest(/*makeActive=*/false);
        LogSession *first = window.SessionAtTab(0);
        LogSession *second = window.SessionAtTab(1);
        QVERIFY(first != nullptr && second != nullptr);
        window.TabWidgetForTest()->tabBar()->moveTab(0, 1);
        QCOMPARE(window.SessionAtTab(0), second);
        QCOMPARE(window.SessionAtTab(1), first);
        QCOMPARE(window.TabCount(), 2);
    }

    static void TestFileDropAppendAndReplace()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString pathA = WriteJsonlFixture(temp.filePath(QStringLiteral("drop-a.jsonl")), 4);
        const QString pathB = WriteJsonlFixture(temp.filePath(QStringLiteral("drop-b.jsonl")), 6);
        QVERIFY(!pathA.isEmpty() && !pathB.isEmpty());

        MainWindow window;
        window.SetSuppressDialogsForTest(true);
        QVERIFY(window.acceptDrops());

        auto dropFiles = [&window](const QString &path, Qt::KeyboardModifiers modifiers) {
            window.DropFilesForTest({path}, modifiers);
        };

        LogModel *model = window.activeSession()->Model();
        QVERIFY(model != nullptr);
        QSignalSpy firstSpy(model, &LogModel::streamingFinished);
        dropFiles(pathA, Qt::NoModifier);
        QTRY_VERIFY_WITH_TIMEOUT(firstSpy.count() >= 1, 5000);
        const int rowsAfterA = model->rowCount();
        QVERIFY(rowsAfterA > 0);

        QSignalSpy appendSpy(model, &LogModel::streamingFinished);
        dropFiles(pathB, Qt::NoModifier);
        QTRY_VERIFY_WITH_TIMEOUT(appendSpy.count() >= 1, 5000);
        QVERIFY(model->rowCount() > rowsAfterA);

        QSignalSpy replaceSpy(model, &LogModel::streamingFinished);
        dropFiles(pathB, Qt::ShiftModifier);
        QTRY_VERIFY_WITH_TIMEOUT(replaceSpy.count() >= 1, 5000);
        QCOMPARE(model->rowCount(), 6);
    }

    static void TestOsFileOpenUsesEnsureFreshActiveTab()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString pathA = WriteJsonlFixture(temp.filePath(QStringLiteral("cli-a.jsonl")), 4);
        const QString pathB = WriteJsonlFixture(temp.filePath(QStringLiteral("cli-b.jsonl")), 7);
        QVERIFY(!pathA.isEmpty() && !pathB.isEmpty());

        MainWindow window;
        window.SetSuppressDialogsForTest(true);
        LoadFileIntoActiveTab(window, pathA);
        LogSession *busy = window.activeSession();
        QVERIFY(busy != nullptr);

        window.EnsureFreshActiveTab();
        QCOMPARE(window.TabCount(), 2);
        QVERIFY(window.activeSession() != busy);

        LogModel *model = window.activeSession()->Model();
        QVERIFY(model != nullptr);
        QSignalSpy spy(model, &LogModel::streamingFinished);
        window.OpenFilesForCli({pathB});
        QTRY_VERIFY_WITH_TIMEOUT(spy.count() >= 1, 5000);
        QCOMPARE(window.TabCount(), 2);
        QCOMPARE(window.SessionAtTab(0), busy);
        QCOMPARE(model->rowCount(), 7);
    }

    static void TestUnselectedTabProgressCancelStaysAuthoritative()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString pathA = WriteJsonlFixture(temp.filePath(QStringLiteral("export-a.jsonl")), 4000);
        const QString pathB = WriteJsonlFixture(temp.filePath(QStringLiteral("export-b.jsonl")), 8);
        QVERIFY(!pathA.isEmpty() && !pathB.isEmpty());

        MainWindow window;
        window.SetSuppressDialogsForTest(true);
        LoadFileIntoActiveTab(window, pathA);
        window.AddNewTabForTest(/*makeActive=*/true);
        LoadFileIntoActiveTab(window, pathB);

        LogSession *sessionA = window.SessionAtTab(0);
        LogSessionView *viewA = qobject_cast<LogSessionView *>(window.TabWidgetForTest()->widget(0));
        QVERIFY(sessionA != nullptr && viewA != nullptr);

        window.ActivateTabForTest(0);
        window.ExportSessionBundleToPathForTest(temp.filePath(QStringLiteral("cancel-a.slvbundle")));
        QVERIFY(sessionA->IsExportInFlight());
        QVERIFY(viewA->IsOperationProgressVisible());
        QVERIFY(viewA->ProgressCancelButton() != nullptr);
        QVERIFY(viewA->ProgressCancelButton()->isEnabled());

        window.ActivateTabForTest(1);
        QVERIFY(sessionA->IsExportInFlight());
        QVERIFY(viewA->IsOperationProgressVisible());
        QVERIFY(viewA->ProgressCancelButton()->isEnabled());
        viewA->ProgressCancelButton()->click();
        QTRY_VERIFY_WITH_TIMEOUT(!sessionA->IsExportInFlight(), 15000);
        QVERIFY(!viewA->IsOperationProgressVisible());
        QVERIFY(viewA->IsContentEnabled());
    }

    static void TestWorkspaceRestoreRoundTrip()
    {
        const ScopedWorkspaceTestPaths paths;
        SessionHistoryManager manager(
            slv::persistence::WorkspacePersistence::DefaultWorkspaceDir(),
            std::make_unique<InMemoryRecentsIndexStorage>()
        );
        QTemporaryDir logs;
        QVERIFY(logs.isValid());
        const QString filePath = WriteJsonlFixture(logs.filePath(QStringLiteral("static.jsonl")), 5);
        const QString tailPath = WriteJsonlFixture(logs.filePath(QStringLiteral("tail.jsonl")), 4);
        const QString peerPath = WriteJsonlFixture(logs.filePath(QStringLiteral("peer.jsonl")), 3);
        QVERIFY(!filePath.isEmpty() && !tailPath.isEmpty() && !peerPath.isEmpty());

        MainWindow source(nullptr, &manager, nullptr);
        source.SetSuppressDialogsForTest(true);
        LoadFileIntoActiveTab(source, filePath);

        source.AddNewTabForTest(/*makeActive=*/true);
        LogSession *configOnly = source.activeSession();
        QVERIFY(configOnly != nullptr);
        configOnly->SetAutoSaveUuid(QUuid::createUuid().toString(QUuid::WithoutBraces));

        source.AddNewTabForTest(/*makeActive=*/true);
        LoadFileIntoActiveTab(source, tailPath);
        source.SetSessionModeForTest(MainWindow::TestSessionMode::LiveTail);
        source.activeSession()->DetachAutoSaveUuid();

        source.AddNewTabForTest(/*makeActive=*/true);
        loglib::LogConfiguration::Source network;
        network.kind = loglib::LogConfiguration::Source::Kind::NetworkStream;
        network.locators = {std::string{"tcp://127.0.0.1:5514"}};
        source.activeSession()->MutableCurrentSource() = network;

        source.AddNewTabForTest(/*makeActive=*/true);
        loglib::LogConfiguration::Source stdinSource;
        stdinSource.kind = loglib::LogConfiguration::Source::Kind::Stdin;
        stdinSource.locators = {std::string{"<stdin>"}};
        source.activeSession()->MutableCurrentSource() = stdinSource;

        source.ActivateTabForTest(0);
        source.AutoSaveAllHostedSessions(/*publishOpenWindow=*/false);
        slv::persistence::WorkspaceWindow captured = source.CaptureWorkspaceWindow();
        QCOMPARE(captured.tabs.size(), std::size_t{5});
        QCOMPARE(captured.tabs[0].sourceMode, slv::persistence::SourceMode::File);
        QCOMPARE(captured.tabs[1].sourceMode, slv::persistence::SourceMode::ConfigOnly);
        QCOMPARE(captured.tabs[2].sourceMode, slv::persistence::SourceMode::LiveTailFile);
        QCOMPARE(captured.tabs[3].sourceMode, slv::persistence::SourceMode::Network);
        QCOMPARE(captured.tabs[4].sourceMode, slv::persistence::SourceMode::Stdin);

        MainWindow peer(nullptr, &manager, nullptr);
        peer.SetSuppressDialogsForTest(true);
        LoadFileIntoActiveTab(peer, peerPath);
        peer.AutoSaveAllHostedSessions(/*publishOpenWindow=*/false);
        slv::persistence::WorkspaceWindow capturedPeer = peer.CaptureWorkspaceWindow();
        QCOMPARE(capturedPeer.tabs.size(), std::size_t{1});
        QCOMPARE(capturedPeer.tabs[0].sourceMode, slv::persistence::SourceMode::File);

        slv::persistence::WorkspaceTab corrupt;
        corrupt.sessionUuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
        corrupt.sourceMode = slv::persistence::SourceMode::File;
        captured.tabs.push_back(corrupt);
        captured.activeTabIndex = 0;

        slv::persistence::Workspace workspace;
        workspace.schemaVersion = slv::persistence::WorkspacePersistence::SCHEMA_VERSION;
        workspace.windows.push_back(captured);
        workspace.windows.push_back(capturedPeer);
        QVERIFY(slv::persistence::WorkspacePersistence::Publish(workspace));

        const slv::persistence::Workspace published = slv::persistence::WorkspacePersistence::Read();
        QCOMPARE(published.windows.size(), std::size_t{2});
        const QString corruptUuid = published.windows[0].tabs.back().sessionUuid;
        if (!corruptUuid.isEmpty())
        {
            const QString corruptPath =
                slv::persistence::WorkspacePersistence::SessionSnapshotPath(published.generation, corruptUuid);
            QFile corruptFile(corruptPath);
            QVERIFY(corruptFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
            corruptFile.write("not-json");
            corruptFile.close();
        }

        MainWindow restored(nullptr, &manager, nullptr);
        restored.SetSuppressDialogsForTest(true);
        restored.ApplyWorkspaceWindow(published.windows[0], published.generation);
        QCOMPARE(restored.TabCount(), 6);
        QCOMPARE(restored.TabWidgetForTest()->currentIndex(), 0);

        LogSession *restoredFile = restored.SessionAtTab(0);
        QVERIFY(restoredFile != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(restoredFile->Model() != nullptr && restoredFile->Model()->rowCount() > 0, 5000);
        QVERIFY(!restoredFile->IsLiveTailSession());

        LogSession *restoredConfig = restored.SessionAtTab(1);
        QVERIFY(restoredConfig != nullptr);
        QVERIFY(!restoredConfig->CurrentSource().has_value());
        QVERIFY(!restoredConfig->AutoSaveUuid().isEmpty());

        LogSession *restoredTail = restored.SessionAtTab(2);
        QVERIFY(restoredTail != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(restoredTail->Model() != nullptr && restoredTail->Model()->rowCount() > 0, 5000);
        QVERIFY(!restoredTail->IsLiveTailSession());
        QVERIFY(restoredTail->CurrentSource().has_value());
        QCOMPARE(restoredTail->CurrentSource()->kind, loglib::LogConfiguration::Source::Kind::File);

        LogSession *restoredNetwork = restored.SessionAtTab(3);
        QVERIFY(restoredNetwork != nullptr);
        QVERIFY(!restoredNetwork->CurrentSource().has_value() || restoredNetwork->Model()->rowCount() == 0);

        LogSession *restoredStdin = restored.SessionAtTab(4);
        QVERIFY(restoredStdin != nullptr);
        QVERIFY(!restoredStdin->CurrentSource().has_value() || restoredStdin->Model()->rowCount() == 0);

        LogSession *restoredCorrupt = restored.SessionAtTab(5);
        QVERIFY(restoredCorrupt != nullptr);
        QVERIFY(!restoredCorrupt->CurrentSource().has_value());
        QVERIFY(restoredFile->Model()->rowCount() > 0);

        MainWindow restoredPeer(nullptr, &manager, nullptr);
        restoredPeer.SetSuppressDialogsForTest(true);
        restoredPeer.ApplyWorkspaceWindow(published.windows[1], published.generation);
        QCOMPARE(restoredPeer.TabCount(), 1);
        LogSession *peerSession = restoredPeer.SessionAtTab(0);
        QVERIFY(peerSession != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(peerSession->Model() != nullptr && peerSession->Model()->rowCount() > 0, 5000);
        QVERIFY(!peerSession->IsLiveTailSession());
    }

    static void TestCloseEventCachesWorkspaceSnapshotForQuit()
    {
        const ScopedWorkspaceTestPaths paths;
        SessionHistoryManager manager(
            slv::persistence::WorkspacePersistence::DefaultWorkspaceDir(),
            std::make_unique<InMemoryRecentsIndexStorage>()
        );
        QTemporaryDir logs;
        QVERIFY(logs.isValid());
        const QString firstPath = WriteJsonlFixture(logs.filePath(QStringLiteral("first.jsonl")), 4);
        const QString secondPath = WriteJsonlFixture(logs.filePath(QStringLiteral("second.jsonl")), 5);
        QVERIFY(!firstPath.isEmpty() && !secondPath.isEmpty());

        MainWindow window(nullptr, &manager, nullptr);
        window.SetSuppressDialogsForTest(true);
        LoadFileIntoActiveTab(window, firstPath);
        window.AddNewTabForTest(/*makeActive=*/true);
        LoadFileIntoActiveTab(window, secondPath);
        window.AutoSaveAllHostedSessions(/*publishOpenWindow=*/false);

        const slv::persistence::WorkspaceWindow beforeClose = window.CaptureWorkspaceWindow();
        QCOMPARE(beforeClose.tabs.size(), std::size_t{2});
        QVERIFY(!beforeClose.tabs[0].sessionUuid.isEmpty());
        QVERIFY(!beforeClose.tabs[1].sessionUuid.isEmpty());
        QCOMPARE(beforeClose.tabs[0].sourceMode, slv::persistence::SourceMode::File);
        QCOMPARE(beforeClose.tabs[1].sourceMode, slv::persistence::SourceMode::File);

        QVERIFY(window.close());
        QCoreApplication::processEvents();

        const slv::persistence::WorkspaceWindow forQuit = window.WorkspaceSnapshotForQuit();
        QCOMPARE(forQuit.tabs.size(), std::size_t{2});
        QCOMPARE(forQuit.tabs[0].sessionUuid, beforeClose.tabs[0].sessionUuid);
        QCOMPARE(forQuit.tabs[1].sessionUuid, beforeClose.tabs[1].sessionUuid);
        QCOMPARE(forQuit.tabs[0].sourceMode, slv::persistence::SourceMode::File);
        QCOMPARE(forQuit.tabs[1].sourceMode, slv::persistence::SourceMode::File);

        const slv::persistence::WorkspaceWindow afterWipe = window.CaptureWorkspaceWindow();
        QVERIFY2(
            afterWipe.tabs[0].sessionUuid.isEmpty() && afterWipe.tabs[1].sessionUuid.isEmpty(),
            "closeEvent still detaches restorable identity so aboutToQuit does not "
            "republish the closed window into openWindowsAtQuit"
        );
        QVERIFY(window.RestorableActiveSessionUuid().isEmpty());
    }

    static void TestWorkspaceRestoreRotationFamilyAndSiblingTab()
    {
        const ScopedWorkspaceTestPaths paths;
        SessionHistoryManager manager(
            slv::persistence::WorkspacePersistence::DefaultWorkspaceDir(),
            std::make_unique<InMemoryRecentsIndexStorage>()
        );
        QTemporaryDir logs;
        QVERIFY(logs.isValid());
        const QString primary = WriteJsonlFixture(logs.filePath(QStringLiteral("app.jsonl")), 3);
        QVERIFY(!WriteJsonlFixture(logs.filePath(QStringLiteral("app.jsonl.1")), 2).isEmpty());
        QVERIFY(!WriteJsonlFixture(logs.filePath(QStringLiteral("app.jsonl.2")), 2).isEmpty());
        const QString sibling = WriteJsonlFixture(logs.filePath(QStringLiteral("other.jsonl")), 4);
        QVERIFY(!primary.isEmpty() && !sibling.isEmpty());

        MainWindow source(nullptr, &manager, nullptr);
        source.SetSuppressDialogsForTest(true);
        LogModel *primaryModel = source.activeSession() != nullptr ? source.activeSession()->Model() : nullptr;
        QVERIFY(primaryModel != nullptr);
        const QSignalSpy primarySpy(primaryModel, &LogModel::streamingFinished);
        QCOMPARE(
            source.OpenMixedFilesForTest({primary}, MainWindow::OpenMode::Replace),
            MainWindow::MixedInputDispatch::QueuedLogsOnly
        );
        QTRY_VERIFY_WITH_TIMEOUT(primarySpy.count() >= 1, 8000);
        LogSession *rotated = source.activeSession();
        QVERIFY(rotated != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(
            rotated->CurrentSource().has_value() && rotated->CurrentSource()->locators.size() >= 2, 8000
        );
        QCOMPARE(rotated->CurrentSource()->kind, loglib::LogConfiguration::Source::Kind::File);

        source.AddNewTabForTest(/*makeActive=*/true);
        LoadFileIntoActiveTab(source, sibling);

        source.AutoSaveAllHostedSessions(/*publishOpenWindow=*/false);
        slv::persistence::WorkspaceWindow captured = source.CaptureWorkspaceWindow();
        QCOMPARE(captured.tabs.size(), std::size_t{2});
        QCOMPARE(captured.tabs[0].sourceMode, slv::persistence::SourceMode::MultiFile);
        QCOMPARE(captured.tabs[1].sourceMode, slv::persistence::SourceMode::File);
        QVERIFY(!captured.tabs[0].sessionUuid.isEmpty());
        QVERIFY(!captured.tabs[1].sessionUuid.isEmpty());

        slv::persistence::Workspace workspace;
        workspace.schemaVersion = slv::persistence::WorkspacePersistence::SCHEMA_VERSION;
        workspace.windows.push_back(captured);
        QVERIFY(slv::persistence::WorkspacePersistence::Publish(workspace));
        const slv::persistence::Workspace published = slv::persistence::WorkspacePersistence::Read();

        MainWindow restored(nullptr, &manager, nullptr);
        restored.SetSuppressDialogsForTest(true);
        restored.ApplyWorkspaceWindow(published.windows[0], published.generation);
        QCOMPARE(restored.TabCount(), 2);

        LogSession *restoredRotated = restored.SessionAtTab(0);
        QVERIFY(restoredRotated != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(
            restoredRotated->Model() != nullptr && restoredRotated->Model()->rowCount() >= 5, 8000
        );
        QVERIFY(restoredRotated->CurrentSource().has_value());
        QVERIFY2(
            restoredRotated->CurrentSource()->locators.size() >= 2,
            "restored rotation tab must reopen every companion locator"
        );

        LogSession *restoredSibling = restored.SessionAtTab(1);
        QVERIFY(restoredSibling != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(restoredSibling->Model() != nullptr && restoredSibling->Model()->rowCount() > 0, 8000);
    }

    static void TestWorkspaceRestoreEmptyTabsWithSavedChromeDoesNotCrash()
    {
        const ScopedWorkspaceTestPaths paths;
        slv::persistence::WorkspaceWindow window;
        window.windowUuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
        window.activeTabIndex = 2;
        window.tabs.resize(3);
        window.tabs[0].label = QStringLiteral("app.log");
        window.tabs[1].label = QStringLiteral("tcp://127.0.0.1:9000");
        window.tabs[2].label = QStringLiteral("<stdin>");
        window.tabs[1].sourceMode = slv::persistence::SourceMode::Network;
        window.tabs[2].sourceMode = slv::persistence::SourceMode::Stdin;

        MainWindow chromeSource;
        chromeSource.SetSuppressDialogsForTest(true);
        auto *histogram = chromeSource.findChild<QDockWidget *>(QStringLiteral("histogramDock"));
        QVERIFY2(histogram != nullptr, "MainWindow must own histogramDock");
        histogram->show();
        window.geometry = chromeSource.saveGeometry();
        window.dockState = chromeSource.saveState();
        QVERIFY(!window.dockState.isEmpty());

        MainWindow restored(nullptr, nullptr, nullptr, nullptr, MainWindow::ChromeRestore::Deferred);
        restored.SetSuppressDialogsForTest(true);
        restored.ApplyWorkspaceWindow(window, /*generation=*/0);
        restored.show();
        QCoreApplication::processEvents();
        QCOMPARE(restored.TabCount(), 3);
        QCOMPARE(restored.TabWidgetForTest()->currentIndex(), 2);
        QCOMPARE(restored.TabWidgetForTest()->tabText(0), QStringLiteral("app.log"));
        QCOMPARE(restored.TabWidgetForTest()->tabText(1), QStringLiteral("tcp://127.0.0.1:9000"));
        QCOMPARE(restored.TabWidgetForTest()->tabText(2), QStringLiteral("<stdin>"));

        MainWindow restoredVisible;
        restoredVisible.SetSuppressDialogsForTest(true);
        restoredVisible.show();
        QCoreApplication::processEvents();
        restoredVisible.ApplyWorkspaceWindow(window, /*generation=*/0);
        QCoreApplication::processEvents();
        QCOMPARE(restoredVisible.TabCount(), 3);
        QCOMPARE(restoredVisible.TabWidgetForTest()->currentIndex(), 2);
        QCOMPARE(restoredVisible.TabWidgetForTest()->tabText(0), QStringLiteral("app.log"));
        QCOMPARE(restoredVisible.TabWidgetForTest()->tabText(1), QStringLiteral("tcp://127.0.0.1:9000"));
        QCOMPARE(restoredVisible.TabWidgetForTest()->tabText(2), QStringLiteral("<stdin>"));
    }
};

QTEST_MAIN(SessionTabsTest)
#include "session_tabs_test.moc"
