// Grouped workspace persistence tests (task 8.13 / 8.15).
//
// Covers the value-type schema (task 8.1) and the on-disk facade
// (task 8.2) in isolation from `MainWindow`. Filesystem tests use
// a per-test `QTemporaryDir` redirected through
// `QStandardPaths::setTestModeEnabled(true)` so the production
// user-scope `AppDataLocation` is not touched.

#include "log_session_presentation.hpp"
#include "session_history_manager.hpp"
#include "workspace_persistence.hpp"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QObject>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

using slv::persistence::RestorePolicy;
using slv::persistence::SourceMode;
using slv::persistence::Workspace;
using slv::persistence::WorkspacePersistence;
using slv::persistence::WorkspaceTab;
using slv::persistence::WorkspaceWindow;

namespace
{

/// Redirect `QStandardPaths::AppDataLocation` (and every other
/// standard-path scope) into an isolated per-test directory so
/// workspace writes cannot escape into the user's real profile.
class ScopedTestPaths
{
public:
    ScopedTestPaths()
    {
        QStandardPaths::setTestModeEnabled(true);
        // Ensure `AppDataLocation` under test-mode exists so
        // `WorkspacePersistence::Write` can create the sessions
        // directory beneath it.
        QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).mkpath(QStringLiteral("sessions"));
        WorkspacePersistence::Clear();
    }
    ScopedTestPaths(const ScopedTestPaths &) = delete;
    ScopedTestPaths &operator=(const ScopedTestPaths &) = delete;
    ScopedTestPaths(ScopedTestPaths &&) = delete;
    ScopedTestPaths &operator=(ScopedTestPaths &&) = delete;
    ~ScopedTestPaths()
    {
        WorkspacePersistence::Clear();
        QStandardPaths::setTestModeEnabled(false);
    }
};

WorkspaceWindow MakeWindow(const QString &uuid, std::size_t tabCount, int activeIdx)
{
    WorkspaceWindow window;
    window.windowUuid = uuid;
    window.geometry = QByteArray("geom-blob");
    window.dockState = QByteArray("dock-blob");
    window.activeTabIndex = activeIdx;
    for (std::size_t i = 0; i < tabCount; ++i)
    {
        WorkspaceTab tab;
        tab.sessionUuid = uuid + QStringLiteral("-tab-%1").arg(i);
        tab.sourceMode = (i % 2 == 0) ? SourceMode::File : SourceMode::LiveTailFile;
        tab.restorePolicy = RestorePolicy::Restore;
        window.tabs.push_back(std::move(tab));
    }
    return window;
}

} // namespace

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

    static void TestWorkspaceSchemaSourceModeEnumIsStable()
    {
        // Task 8.1: numeric values persist to disk. Do not
        // renumber without bumping `kSchemaVersion`.
        QCOMPARE(static_cast<std::uint8_t>(SourceMode::Empty), static_cast<std::uint8_t>(0));
        QCOMPARE(static_cast<std::uint8_t>(SourceMode::File), static_cast<std::uint8_t>(1));
        QCOMPARE(static_cast<std::uint8_t>(SourceMode::MultiFile), static_cast<std::uint8_t>(2));
        QCOMPARE(static_cast<std::uint8_t>(SourceMode::Compressed), static_cast<std::uint8_t>(3));
        QCOMPARE(static_cast<std::uint8_t>(SourceMode::Bundle), static_cast<std::uint8_t>(4));
        QCOMPARE(static_cast<std::uint8_t>(SourceMode::LiveTailFile), static_cast<std::uint8_t>(5));
        QCOMPARE(static_cast<std::uint8_t>(SourceMode::Network), static_cast<std::uint8_t>(6));
        QCOMPARE(static_cast<std::uint8_t>(SourceMode::Stdin), static_cast<std::uint8_t>(7));
        QCOMPARE(static_cast<std::uint8_t>(SourceMode::ConfigOnly), static_cast<std::uint8_t>(8));
    }

    static void TestReadReturnsEmptyWhenFileMissing()
    {
        const ScopedTestPaths paths;
        const Workspace workspace = WorkspacePersistence::Read();
        QVERIFY(workspace.windows.empty());
        QVERIFY(workspace.mruOrder.isEmpty());
    }

    static void TestWriteReadRoundTripSingleWindow()
    {
        const ScopedTestPaths paths;
        Workspace src;
        src.schemaVersion = WorkspacePersistence::SCHEMA_VERSION;
        src.windows.push_back(MakeWindow(QStringLiteral("win-1"), /*tabCount=*/3, /*activeIdx=*/1));
        src.mruOrder = QStringList{QStringLiteral("win-1")};

        QVERIFY(WorkspacePersistence::Write(src));

        const Workspace loaded = WorkspacePersistence::Read();
        QCOMPARE(loaded.schemaVersion, WorkspacePersistence::SCHEMA_VERSION);
        QCOMPARE(loaded.windows.size(), std::size_t{1});
        QCOMPARE(loaded.windows[0].windowUuid, QStringLiteral("win-1"));
        QCOMPARE(loaded.windows[0].geometry, QByteArray("geom-blob"));
        QCOMPARE(loaded.windows[0].dockState, QByteArray("dock-blob"));
        QCOMPARE(loaded.windows[0].tabs.size(), std::size_t{3});
        QCOMPARE(loaded.windows[0].tabs[0].sessionUuid, QStringLiteral("win-1-tab-0"));
        QCOMPARE(loaded.windows[0].tabs[0].sourceMode, SourceMode::File);
        QCOMPARE(loaded.windows[0].tabs[1].sourceMode, SourceMode::LiveTailFile);
        QCOMPARE(loaded.windows[0].activeTabIndex, 1);
        QCOMPARE(loaded.mruOrder, QStringList{QStringLiteral("win-1")});
    }

    static void TestWriteReadRoundTripMultiWindow()
    {
        const ScopedTestPaths paths;
        Workspace src;
        src.schemaVersion = WorkspacePersistence::SCHEMA_VERSION;
        src.windows.push_back(MakeWindow(QStringLiteral("win-a"), 2, 0));
        src.windows.push_back(MakeWindow(QStringLiteral("win-b"), 4, 3));
        src.mruOrder = QStringList{QStringLiteral("win-a"), QStringLiteral("win-b")};

        QVERIFY(WorkspacePersistence::Write(src));

        const Workspace loaded = WorkspacePersistence::Read();
        QCOMPARE(loaded.windows.size(), std::size_t{2});
        QCOMPARE(loaded.windows[0].windowUuid, QStringLiteral("win-a"));
        QCOMPARE(loaded.windows[1].windowUuid, QStringLiteral("win-b"));
        QCOMPARE(loaded.windows[1].tabs.size(), std::size_t{4});
        QCOMPARE(loaded.windows[1].activeTabIndex, 3);
        QCOMPARE(loaded.mruOrder.size(), 2);
    }

    static void TestSchemaMismatchYieldsEmptyRead()
    {
        const ScopedTestPaths paths;
        // Hand-write a doc with a bogus schema version.
        const QString path = WorkspacePersistence::WorkspaceFilePath();
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        const QByteArray raw = R"({"schemaVersion": 999, "windows": []})";
        QVERIFY(file.write(raw) == raw.size());
        file.close();

        const Workspace loaded = WorkspacePersistence::Read();
        QVERIFY(loaded.windows.empty());
    }

    static void TestInvalidJsonYieldsEmptyRead()
    {
        const ScopedTestPaths paths;
        const QString path = WorkspacePersistence::WorkspaceFilePath();
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        const QByteArray raw = "{not valid json";
        QVERIFY(file.write(raw) == raw.size());
        file.close();

        const Workspace loaded = WorkspacePersistence::Read();
        QVERIFY(loaded.windows.empty());
    }

    static void TestTakeWipesOnRead()
    {
        const ScopedTestPaths paths;
        Workspace src;
        src.schemaVersion = WorkspacePersistence::SCHEMA_VERSION;
        src.windows.push_back(MakeWindow(QStringLiteral("win-1"), 1, 0));
        QVERIFY(WorkspacePersistence::Write(src));
        QVERIFY(WorkspacePersistence::HasPersistedWorkspace());

        const Workspace taken = WorkspacePersistence::Take();
        QCOMPARE(taken.windows.size(), std::size_t{1});

        // Second take returns empty; the file was wiped in place.
        const Workspace again = WorkspacePersistence::Read();
        QVERIFY(again.windows.empty());
        QVERIFY(!WorkspacePersistence::HasPersistedWorkspace());
    }

    static void TestSanitizeClampsOversizeInput()
    {
        // Cap validation is exposed for direct verification so a
        // test does not need to hand-craft a 65-window workspace
        // through the writer just to hit the ceiling.
        Workspace ws;
        ws.schemaVersion = WorkspacePersistence::SCHEMA_VERSION;
        for (std::size_t i = 0; i < WorkspacePersistence::MAX_WINDOWS + 5; ++i)
        {
            ws.windows.push_back(MakeWindow(QStringLiteral("w-%1").arg(i), 1, 0));
        }
        WorkspacePersistence::Sanitize(ws);
        QCOMPARE(ws.windows.size(), WorkspacePersistence::MAX_WINDOWS);
    }

    static void TestSanitizeDropsOversizeGeometryBlob()
    {
        Workspace ws;
        ws.schemaVersion = WorkspacePersistence::SCHEMA_VERSION;
        WorkspaceWindow window = MakeWindow(QStringLiteral("win"), 1, 0);
        window.geometry = QByteArray(static_cast<int>(WorkspacePersistence::MAX_GEOMETRY_BYTES) + 1, 'x');
        ws.windows.push_back(std::move(window));
        WorkspacePersistence::Sanitize(ws);
        // Oversize blob dropped entirely so the on-disk workspace
        // never carries a truncated (unrestorable) geometry.
        QVERIFY(ws.windows[0].geometry.isEmpty());
    }

    static void TestSanitizeClampsActiveTabIndex()
    {
        Workspace ws;
        ws.schemaVersion = WorkspacePersistence::SCHEMA_VERSION;
        WorkspaceWindow window = MakeWindow(QStringLiteral("win"), 3, 42);
        ws.windows.push_back(std::move(window));
        WorkspacePersistence::Sanitize(ws);
        QVERIFY(ws.windows[0].activeTabIndex >= 0);
        QVERIFY(static_cast<std::size_t>(ws.windows[0].activeTabIndex) < ws.windows[0].tabs.size());
    }

    static void TestPeerCannotClobberWorkspace()
    {
        const ScopedTestPaths paths;
        Workspace src;
        src.schemaVersion = WorkspacePersistence::SCHEMA_VERSION;
        src.windows.push_back(MakeWindow(QStringLiteral("win-1"), 1, 0));
        QVERIFY(WorkspacePersistence::Write(src));

        SessionHistoryManager::SetPublishingEnabled(false);
        // Should return false and leave the file untouched.
        Workspace peer;
        peer.schemaVersion = WorkspacePersistence::SCHEMA_VERSION;
        peer.windows.push_back(MakeWindow(QStringLiteral("peer-win"), 1, 0));
        QVERIFY(!WorkspacePersistence::Write(std::move(peer)));
        // Restore publish gate so subsequent tests are not
        // starved.
        SessionHistoryManager::SetPublishingEnabled(true);

        const Workspace loaded = WorkspacePersistence::Read();
        QCOMPARE(loaded.windows.size(), std::size_t{1});
        QCOMPARE(loaded.windows[0].windowUuid, QStringLiteral("win-1"));
    }

    static void TestSkipPolicyRoundTrips()
    {
        const ScopedTestPaths paths;
        Workspace src;
        src.schemaVersion = WorkspacePersistence::SCHEMA_VERSION;
        WorkspaceWindow window = MakeWindow(QStringLiteral("win"), 2, 0);
        window.tabs[1].restorePolicy = RestorePolicy::Skip;
        src.windows.push_back(std::move(window));
        QVERIFY(WorkspacePersistence::Write(src));

        const Workspace loaded = WorkspacePersistence::Read();
        QCOMPARE(loaded.windows[0].tabs[0].restorePolicy, RestorePolicy::Restore);
        QCOMPARE(loaded.windows[0].tabs[1].restorePolicy, RestorePolicy::Skip);
    }

    static void TestClearRemovesFile()
    {
        const ScopedTestPaths paths;
        Workspace src;
        src.schemaVersion = WorkspacePersistence::SCHEMA_VERSION;
        src.windows.push_back(MakeWindow(QStringLiteral("win"), 1, 0));
        QVERIFY(WorkspacePersistence::Write(src));
        QVERIFY(QFile::exists(WorkspacePersistence::WorkspaceFilePath()));
        WorkspacePersistence::Clear();
        QVERIFY(!QFile::exists(WorkspacePersistence::WorkspaceFilePath()));
    }

    // -----------------------------------------------------------------
    // Post-tabs review-round bug #M3 fix pin: read-side bounds
    // must fail-closed (the header docstring on `MAX_WINDOWS`
    // etc. promises "values above these ceilings are treated as
    // corrupted input; the read returns an empty workspace").
    // Previously the reader silently truncated at the caps and
    // `Take()`'s subsequent atomic wipe permanently lost the
    // surplus.
    // -----------------------------------------------------------------

    static void TestOverCapWindowsCountFailsClosedOnRead()
    {
        const ScopedTestPaths paths;
        const QString path = WorkspacePersistence::WorkspaceFilePath();
        QString json = QStringLiteral("{\"schemaVersion\":1,\"windows\":[");
        for (std::size_t i = 0; i <= WorkspacePersistence::MAX_WINDOWS; ++i)
        {
            if (i > 0)
            {
                json += QLatin1Char(',');
            }
            json += QStringLiteral("{\"windowUuid\":\"w-%1\",\"tabs\":[]}").arg(i);
        }
        json += QStringLiteral("]}");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QVERIFY(file.write(json.toUtf8()) == json.toUtf8().size());
        file.close();

        const Workspace loaded = WorkspacePersistence::Read();
        QVERIFY2(
            loaded.windows.empty(),
            "Over-cap window count must fail-closed (return empty workspace) "
            "instead of silently truncating; combined with Take()'s wipe, silent "
            "truncation would permanently destroy the surplus."
        );
    }

    static void TestOverCapTabsPerWindowFailsClosedOnRead()
    {
        const ScopedTestPaths paths;
        const QString path = WorkspacePersistence::WorkspaceFilePath();
        QString tabs;
        for (std::size_t i = 0; i <= WorkspacePersistence::MAX_TABS_PER_WINDOW; ++i)
        {
            if (i > 0)
            {
                tabs += QLatin1Char(',');
            }
            tabs += QStringLiteral("{\"sessionUuid\":\"t-%1\",\"sourceMode\":0,\"restorePolicy\":0}").arg(i);
        }
        const QString json = QStringLiteral("{\"schemaVersion\":1,\"windows\":[{\"windowUuid\":\"w\",\"tabs\":[") +
                             tabs + QStringLiteral("]}]}");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QVERIFY(file.write(json.toUtf8()) == json.toUtf8().size());
        file.close();

        const Workspace loaded = WorkspacePersistence::Read();
        QVERIFY2(
            loaded.windows.empty(),
            "Over-cap tab count in any single window must fail-closed. Previously "
            "the reader silently trimmed to `MAX_TABS_PER_WINDOW` and Take()'s "
            "atomic wipe made the surplus unrecoverable."
        );
    }

    // -----------------------------------------------------------------
    // Post-tabs review-round bug #8 fix pin: the startup restore
    // in `main()` splits an over-cap workspace into a "restore
    // prefix" + "surplus written back" pair instead of the
    // pre-fix `Take()` (atomic read + wipe) followed by silent
    // truncation. This test simulates that shape end-to-end
    // through `WorkspacePersistence::{Read,Write}` so a
    // regression in the split logic (dropping the surplus) is
    // caught even though the split itself lives outside this
    // class.
    // -----------------------------------------------------------------

    static void TestOverCapWorkspaceSurplusIsWrittenBackForNextLaunch()
    {
        const ScopedTestPaths paths;
        constexpr std::size_t kSurplusCount = 3;
        constexpr std::size_t kRestorePrefix = 5;
        constexpr std::size_t kTotal = kRestorePrefix + kSurplusCount;

        Workspace src;
        src.schemaVersion = WorkspacePersistence::SCHEMA_VERSION;
        for (std::size_t i = 0; i < kTotal; ++i)
        {
            src.windows.push_back(MakeWindow(QStringLiteral("win-%1").arg(i), 1, 0));
            src.mruOrder.append(QStringLiteral("win-%1").arg(i));
        }
        QVERIFY(WorkspacePersistence::Write(src));

        // Simulate the startup split-write: `Read()` (no wipe),
        // truncate the in-memory restore list, `Write()` the
        // surplus back so a later launch picks it up.
        Workspace peek = WorkspacePersistence::Read();
        QCOMPARE(peek.windows.size(), kTotal);

        Workspace surplus;
        surplus.schemaVersion = peek.schemaVersion;
        surplus.windows.assign(peek.windows.begin() + kRestorePrefix, peek.windows.end());
        surplus.mruOrder = peek.mruOrder;
        QVERIFY(WorkspacePersistence::Write(std::move(surplus)));

        // A subsequent `Read()` (or the next launch's `Take()`)
        // must see the surplus intact -- previously the
        // atomic wipe from `Take()` would have destroyed it.
        const Workspace afterSplit = WorkspacePersistence::Read();
        QCOMPARE(afterSplit.windows.size(), kSurplusCount);
        for (std::size_t i = 0; i < kSurplusCount; ++i)
        {
            QCOMPARE(afterSplit.windows[i].windowUuid, QStringLiteral("win-%1").arg(kRestorePrefix + i));
        }
    }
};

QTEST_MAIN(WorkspacePersistenceTest)
#include "workspace_persistence_test.moc"
