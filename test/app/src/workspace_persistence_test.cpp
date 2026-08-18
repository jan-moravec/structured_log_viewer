// Grouped workspace persistence tests for the value-type schema
// and on-disk facade. Filesystem tests use
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
#include <QUuid>

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
        (void)WorkspacePersistence::TakeDeferredWindows();
    }
    ScopedTestPaths(const ScopedTestPaths &) = delete;
    ScopedTestPaths &operator=(const ScopedTestPaths &) = delete;
    ScopedTestPaths(ScopedTestPaths &&) = delete;
    ScopedTestPaths &operator=(ScopedTestPaths &&) = delete;
    ~ScopedTestPaths()
    {
        WorkspacePersistence::Clear();
        (void)WorkspacePersistence::TakeDeferredWindows();
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

QString WriteRecentsSnapshot(const QString &uuid)
{
    const QString path = WorkspacePersistence::DefaultWorkspaceDir().filePath(uuid + QStringLiteral(".json"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        return {};
    }
    file.write(R"({"columns":[],"filters":[]})");
    return path;
}

} // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage): `Q_OBJECT` QtTest fixture.
class WorkspacePersistenceTest : public QObject
{
    Q_OBJECT

private slots:
    static void TestSessionSourceModeEnumIsStable()
    {
        // These numeric values are persisted. Keep existing values
        // stable and append new source modes after the current range.
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
        // Numeric values persist to disk; renumbering requires a
        // `kSchemaVersion` bump.
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

    // Inputs above the documented read-side limits are treated as
    // corrupt, so reads fail closed instead of truncating data.

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

    // A restore cap smaller than the persisted window count leaves the
    // original file intact and remembers the remainder for a later quit merge.

    static void TestLoadForLaunchDefersSurplusWithoutRewritingFile()
    {
        const ScopedTestPaths paths;
        constexpr std::size_t kRestoreCap = 5;
        constexpr std::size_t kSurplusCount = 3;
        constexpr std::size_t kTotal = kRestoreCap + kSurplusCount;

        Workspace src;
        src.schemaVersion = WorkspacePersistence::SCHEMA_VERSION;
        for (std::size_t i = 0; i < kTotal; ++i)
        {
            src.windows.push_back(MakeWindow(QStringLiteral("win-%1").arg(i), 1, 0));
            src.mruOrder.append(QStringLiteral("win-%1").arg(i));
        }
        QVERIFY(WorkspacePersistence::Write(src));

        const auto plan = WorkspacePersistence::LoadForLaunch(kRestoreCap);
        QCOMPARE(plan.toRestore.windows.size(), kRestoreCap);
        QCOMPARE(plan.deferred.windows.size(), kSurplusCount);
        for (std::size_t i = 0; i < kRestoreCap; ++i)
        {
            QCOMPARE(plan.toRestore.windows[i].windowUuid, QStringLiteral("win-%1").arg(i));
        }
        for (std::size_t i = 0; i < kSurplusCount; ++i)
        {
            QCOMPARE(plan.deferred.windows[i].windowUuid, QStringLiteral("win-%1").arg(kRestoreCap + i));
        }

        const Workspace onDisk = WorkspacePersistence::Read();
        QCOMPARE(onDisk.windows.size(), kTotal);
        for (std::size_t i = 0; i < kTotal; ++i)
        {
            QCOMPARE(onDisk.windows[i].windowUuid, QStringLiteral("win-%1").arg(i));
        }
    }

    static void TestLoadForLaunchLeavesFileWhenNothingIsDeferred()
    {
        const ScopedTestPaths paths;
        Workspace src;
        src.schemaVersion = WorkspacePersistence::SCHEMA_VERSION;
        src.windows.push_back(MakeWindow(QStringLiteral("win-0"), 1, 0));
        src.windows.push_back(MakeWindow(QStringLiteral("win-1"), 1, 0));
        QVERIFY(WorkspacePersistence::Write(src));

        const auto plan = WorkspacePersistence::LoadForLaunch(5);
        QCOMPARE(plan.toRestore.windows.size(), std::size_t{2});
        QVERIFY(plan.deferred.windows.empty());
        QCOMPARE(WorkspacePersistence::Read().windows.size(), std::size_t{2});
    }

    static void TestMergeCapturedWithDeferredSkipsDuplicateWindowUuids()
    {
        Workspace captured;
        captured.schemaVersion = WorkspacePersistence::SCHEMA_VERSION;
        captured.windows.push_back(MakeWindow(QStringLiteral("win-0"), 1, 0));
        captured.mruOrder.append(QStringLiteral("win-0"));

        Workspace deferred;
        deferred.schemaVersion = WorkspacePersistence::SCHEMA_VERSION;
        deferred.windows.push_back(MakeWindow(QStringLiteral("win-0"), 2, 0));
        deferred.windows.push_back(MakeWindow(QStringLiteral("win-5"), 1, 0));
        deferred.mruOrder.append(QStringLiteral("win-0"));
        deferred.mruOrder.append(QStringLiteral("win-5"));

        const Workspace merged = WorkspacePersistence::MergeCapturedWithDeferred(std::move(captured), deferred);
        QCOMPARE(merged.windows.size(), std::size_t{2});
        QCOMPARE(merged.windows[0].windowUuid, QStringLiteral("win-0"));
        QCOMPARE(merged.windows[0].tabs.size(), std::size_t{1});
        QCOMPARE(merged.windows[1].windowUuid, QStringLiteral("win-5"));
        QCOMPARE(merged.mruOrder.size(), 2);
        QCOMPARE(merged.mruOrder.at(0), QStringLiteral("win-0"));
        QCOMPARE(merged.mruOrder.at(1), QStringLiteral("win-5"));
    }

    // Startup restores a bounded prefix, a normal quit publishes the live
    // windows plus still-deferred remainder, and a second launch still sees
    // those deferred windows.

    static void TestRestoreCapSurvivesQuitAndSecondLaunch()
    {
        const ScopedTestPaths paths;
        constexpr std::size_t kRestoreCap = 5;
        constexpr std::size_t kSurplusCount = 3;
        constexpr std::size_t kTotal = kRestoreCap + kSurplusCount;

        Workspace src;
        src.schemaVersion = WorkspacePersistence::SCHEMA_VERSION;
        for (std::size_t i = 0; i < kTotal; ++i)
        {
            src.windows.push_back(MakeWindow(QStringLiteral("win-%1").arg(i), 1, 0));
            src.mruOrder.append(QStringLiteral("win-%1").arg(i));
        }
        QVERIFY(WorkspacePersistence::Write(src));

        const auto firstLaunch = WorkspacePersistence::LoadForLaunch(kRestoreCap);
        QCOMPARE(firstLaunch.toRestore.windows.size(), kRestoreCap);
        QCOMPARE(firstLaunch.deferred.windows.size(), kSurplusCount);

        Workspace captured;
        captured.schemaVersion = WorkspacePersistence::SCHEMA_VERSION;
        captured.windows = firstLaunch.toRestore.windows;
        captured.windows.pop_back();
        captured.windows.front().geometry = QByteArray("geom-after-quit");
        captured.mruOrder.append(QStringLiteral("win-0"));
        captured.mruOrder.append(QStringLiteral("win-1"));

        const Workspace deferred = WorkspacePersistence::TakeDeferredWindows();
        QCOMPARE(deferred.windows.size(), kSurplusCount);
        Workspace merged = WorkspacePersistence::MergeCapturedWithDeferred(std::move(captured), deferred);
        QVERIFY(WorkspacePersistence::Write(std::move(merged)));

        const Workspace afterQuit = WorkspacePersistence::Read();
        QCOMPARE(afterQuit.windows.size(), (kRestoreCap - 1) + kSurplusCount);
        QCOMPARE(afterQuit.windows.front().windowUuid, QStringLiteral("win-0"));
        QCOMPARE(afterQuit.windows.front().geometry, QByteArray("geom-after-quit"));
        QCOMPARE(afterQuit.windows[kRestoreCap - 1].windowUuid, QStringLiteral("win-5"));
        for (const WorkspaceWindow &window : afterQuit.windows)
        {
            QVERIFY(window.windowUuid != QStringLiteral("win-4"));
        }

        const auto secondLaunch = WorkspacePersistence::LoadForLaunch(kRestoreCap);
        QCOMPARE(secondLaunch.toRestore.windows.size(), kRestoreCap);
        QCOMPARE(secondLaunch.toRestore.windows.front().geometry, QByteArray("geom-after-quit"));
        QCOMPARE(secondLaunch.deferred.windows.size(), kSurplusCount - 1);
        QCOMPARE(secondLaunch.deferred.windows.front().windowUuid, QStringLiteral("win-6"));
        QCOMPARE(WorkspacePersistence::Read().windows.size(), afterQuit.windows.size());
    }

    static void TestSchemaVersionMismatchReturnsEmptyWithoutDeletingFile()
    {
        const ScopedTestPaths paths;
        QFile file(WorkspacePersistence::WorkspaceFilePath());
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(
            R"({"schemaVersion":1,"windows":[{"windowUuid":"win-1","tabs":[],"activeTabIndex":0}],"mruOrder":[]})"
        );
        file.close();

        const Workspace loaded = WorkspacePersistence::Read();
        QVERIFY(loaded.windows.empty());
        QVERIFY(QFile::exists(WorkspacePersistence::WorkspaceFilePath()));
    }

    static void TestReadRejectsOversizedWorkspaceFile()
    {
        const ScopedTestPaths paths;
        QFile file(WorkspacePersistence::WorkspaceFilePath());
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QVERIFY(file.resize(static_cast<qint64>(WorkspacePersistence::MAX_WORKSPACE_FILE_BYTES) + 1));
        file.close();

        const Workspace loaded = WorkspacePersistence::Read();
        QVERIFY(loaded.windows.empty());
        QVERIFY(loaded.mruOrder.isEmpty());
        QVERIFY(QFile::exists(WorkspacePersistence::WorkspaceFilePath()));
    }

    static void TestPublishCopiesSnapshotsAndAssignsGeneration()
    {
        const ScopedTestPaths paths;
        const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QVERIFY(!WriteRecentsSnapshot(uuid).isEmpty());

        Workspace src;
        src.schemaVersion = WorkspacePersistence::SCHEMA_VERSION;
        WorkspaceWindow window;
        window.windowUuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
        WorkspaceTab tab;
        tab.sessionUuid = uuid;
        tab.sourceMode = SourceMode::File;
        window.tabs.push_back(tab);
        src.windows.push_back(window);

        QVERIFY(WorkspacePersistence::Publish(src));
        const Workspace loaded = WorkspacePersistence::Read();
        QCOMPARE(loaded.generation, std::uint64_t{1});
        QCOMPARE(loaded.windows.size(), std::size_t{1});
        QCOMPARE(loaded.windows[0].tabs[0].sessionUuid, uuid);
        QVERIFY(QFile::exists(WorkspacePersistence::SessionSnapshotPath(1, uuid)));
    }

    static void TestPublishClearsUuidWhenSnapshotCopyFails()
    {
        const ScopedTestPaths paths;
        const QString missing = QUuid::createUuid().toString(QUuid::WithoutBraces);

        Workspace src;
        src.schemaVersion = WorkspacePersistence::SCHEMA_VERSION;
        WorkspaceWindow window;
        window.windowUuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
        WorkspaceTab tab;
        tab.sessionUuid = missing;
        tab.sourceMode = SourceMode::File;
        window.tabs.push_back(tab);
        src.windows.push_back(window);

        QVERIFY(WorkspacePersistence::Publish(src));
        const Workspace loaded = WorkspacePersistence::Read();
        QCOMPARE(loaded.generation, std::uint64_t{1});
        QVERIFY(loaded.windows[0].tabs[0].sessionUuid.isEmpty());
    }

    static void TestCrashBeforeManifestKeepsPreviousGeneration()
    {
        const ScopedTestPaths paths;
        const QString uuid1 = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString uuid2 = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QVERIFY(!WriteRecentsSnapshot(uuid1).isEmpty());
        QVERIFY(!WriteRecentsSnapshot(uuid2).isEmpty());

        Workspace first;
        first.schemaVersion = WorkspacePersistence::SCHEMA_VERSION;
        WorkspaceWindow window;
        window.windowUuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
        WorkspaceTab tab;
        tab.sessionUuid = uuid1;
        tab.sourceMode = SourceMode::File;
        window.tabs.push_back(tab);
        first.windows.push_back(window);
        QVERIFY(WorkspacePersistence::Publish(first));
        QCOMPARE(WorkspacePersistence::Read().generation, std::uint64_t{1});

        Workspace second = first;
        second.windows[0].tabs[0].sessionUuid = uuid2;
        QVERIFY(WorkspacePersistence::WriteGenerationSnapshots(2, second));
        QVERIFY(QFile::exists(WorkspacePersistence::SessionSnapshotPath(2, uuid2)));

        const Workspace onDisk = WorkspacePersistence::Read();
        QCOMPARE(onDisk.generation, std::uint64_t{1});
        QCOMPARE(onDisk.windows[0].tabs[0].sessionUuid, uuid1);
        QVERIFY(QDir(WorkspacePersistence::GenerationDirPath(1)).exists());
        QVERIFY(QDir(WorkspacePersistence::GenerationDirPath(2)).exists());
    }

    static void TestCrashAfterManifestBeforeCollectionKeepsNewGeneration()
    {
        const ScopedTestPaths paths;
        const QString uuid1 = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString uuid2 = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QVERIFY(!WriteRecentsSnapshot(uuid1).isEmpty());
        QVERIFY(!WriteRecentsSnapshot(uuid2).isEmpty());

        Workspace first;
        first.schemaVersion = WorkspacePersistence::SCHEMA_VERSION;
        WorkspaceWindow window;
        window.windowUuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
        WorkspaceTab tab;
        tab.sessionUuid = uuid1;
        tab.sourceMode = SourceMode::File;
        window.tabs.push_back(tab);
        first.windows.push_back(window);
        QVERIFY(WorkspacePersistence::Publish(first));

        Workspace second = first;
        second.windows[0].tabs[0].sessionUuid = uuid2;
        QVERIFY(WorkspacePersistence::WriteGenerationSnapshots(2, second));
        second.generation = 2;
        second.schemaVersion = WorkspacePersistence::SCHEMA_VERSION;
        QVERIFY(WorkspacePersistence::Write(second));

        const Workspace onDisk = WorkspacePersistence::Read();
        QCOMPARE(onDisk.generation, std::uint64_t{2});
        QCOMPARE(onDisk.windows[0].tabs[0].sessionUuid, uuid2);
        QVERIFY(QDir(WorkspacePersistence::GenerationDirPath(1)).exists());
        QVERIFY(QDir(WorkspacePersistence::GenerationDirPath(2)).exists());

        WorkspacePersistence::CollectSupersededGenerations(2);
        QVERIFY(!QDir(WorkspacePersistence::GenerationDirPath(1)).exists());
        QVERIFY(QDir(WorkspacePersistence::GenerationDirPath(2)).exists());
    }
};

QTEST_MAIN(WorkspacePersistenceTest)
#include "workspace_persistence_test.moc"
