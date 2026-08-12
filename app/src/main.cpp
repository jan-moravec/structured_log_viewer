#include "main_window.hpp"

#include "cli_parser.hpp"
#include "log_warning.hpp"
#include "regex_template_registry.hpp"
#include "session_history_manager.hpp"
#include "single_instance_guard.hpp"
#include "theme_control.hpp"
#include "uuid_utils.hpp"
#include "workspace_persistence.hpp"

#include <QApplication>
#include <QAtomicInt>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QFileOpenEvent>
#include <QObject>
#include <QPointer>
#include <QProcessEnvironment>
#include <QSettings>
#include <QStatusBar>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <memory>

namespace
{

// Bound startup work for unusually large persisted workspaces.
constexpr int MAX_RESTORE_PEERS = 25;

// Queue macOS file-open events until a live window can receive them.
class FileOpenEventFilter : public QObject
{
public:
    explicit FileOpenEventFilter(QObject *parent = nullptr)
        : QObject(parent)
    {
    }

    [[nodiscard]] QStringList takePending()
    {
        QStringList out;
        out.swap(mPending);
        return out;
    }

    void setLiveWindow(MainWindow *window)
    {
        mLiveWindow = window;
    }

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() != QEvent::FileOpen)
        {
            return QObject::eventFilter(watched, event);
        }
        // The event type guarantees this Qt-defined dynamic type.
        auto *fileOpen =
            static_cast<QFileOpenEvent *>(event); // NOLINT(cppcoreguidelines-pro-type-static-cast-downcast)
        const QString path = fileOpen->file();
        if (path.isEmpty())
        {
            return true;
        }
        // Preserve display casing; the open queue performs deduplication.
        const QString displayPath = logapp::CanonicalDisplayPath(path);
        if (mLiveWindow != nullptr)
        {
            // OS-open requests must not replace the active investigation.
            mLiveWindow->raise();
            mLiveWindow->activateWindow();
            mLiveWindow->EnsureFreshActiveTab();
            mLiveWindow->OpenFilesForCli({displayPath});
        }
        else
        {
            mPending.append(displayPath);
        }
        return true;
    }

private:
    QStringList mPending;
    QPointer<MainWindow> mLiveWindow;
};

} // namespace

int main(int argc, char *argv[])
{
    const QApplication a(argc, argv);

    QCoreApplication::setOrganizationName("jan-moravec");
    QCoreApplication::setApplicationName("StructuredLogViewer");

    // Install before event processing so early macOS opens are retained.
    FileOpenEventFilter fileOpenFilter;
    qApp->installEventFilter(&fileOpenFilter);

    // Windows hold non-owning pointers, so shared controllers outlive them.
    ThemeControl themeControl;

    // One process-wide registry keeps every window on the same template catalog.
    RegexTemplateRegistry regexTemplateRegistry;
    // Remove obsolete appearance keys only when they are present.
    {
        QSettings settings;
        if (settings.contains(QStringLiteral("appearance/style")))
        {
            settings.remove(QStringLiteral("appearance/style"));
        }
        if (settings.contains(QStringLiteral("appearance/font")))
        {
            settings.remove(QStringLiteral("appearance/font"));
        }
    }

    const logapp::ParsedCli parsed =
        logapp::ParseCli(QCoreApplication::arguments(), QProcessEnvironment::systemEnvironment());
    QStringList cliFiles = parsed.files;
    const bool allowNewInstance = parsed.allowNewInstance;
    const bool readStdin = parsed.readStdin;

    // Include early OS-open events in any single-instance forwarding.
    QCoreApplication::processEvents(QEventLoop::AllEvents, 0);
    {
        const QStringList preAcquirePending = fileOpenFilter.takePending();
        if (!preAcquirePending.isEmpty())
        {
            cliFiles.append(preAcquirePending);
        }
    }

    // Stdin is process-local, so stdin launches cannot forward to the primary.
    const bool effectiveAllowNewInstance = allowNewInstance || readStdin;
    SingleInstanceGuard instanceGuard;
    // Forward options that affect how the primary opens this launch.
    SingleInstanceGuard::LaunchFlagsBitmask launchFlags;
    if (parsed.disableRotationHistory)
    {
        launchFlags |= SingleInstanceGuard::LaunchFlags::DisableRotationHistory;
    }
    if (!instanceGuard.TryAcquire(cliFiles, effectiveAllowNewInstance, launchFlags))
    {
        return 0;
    }

    // Only the canonical primary publishes shared quit state.
    SessionHistoryManager::SetPublishingEnabled(!effectiveAllowNewInstance);

    // Restored filters may format timestamps during window construction.
    if (!MainWindow::InitializeTimezoneDatabase())
    {
        return 1;
    }

    // Windows use this manager during close and quit callbacks.
    SessionHistoryManager historyManager(
        SessionHistoryManager::DefaultSessionsDir(), std::make_unique<QSettingsRecentsIndexStorage>()
    );

    // Reconcile snapshots that were written without an index update.
    const SessionHistoryManager::CleanupReport cleanupReport = historyManager.CleanupOrphanFiles();

    MainWindow w(&themeControl, &historyManager, &regexTemplateRegistry, nullptr);
    w.show();
    if (cleanupReport.capped)
    {
        constexpr int CAPPED_MESSAGE_TIMEOUT_MS = 8000;
        w.statusBar()->showMessage(
            QObject::tr("Cleaned up %1 orphan session files; more will be removed on the next launch.")
                .arg(cleanupReport.deletedCount),
            CAPPED_MESSAGE_TIMEOUT_MS
        );
    }
    fileOpenFilter.setLiveWindow(&w);

    // Capture OS-open events delivered during startup.
    const QStringList pendingFromOs = fileOpenFilter.takePending();
    if (!pendingFromOs.isEmpty())
    {
        cliFiles.append(pendingFromOs);
    }

    // The launch override must govern every source opened below.
    w.SetRotationHistoryLaunchOverride(parsed.disableRotationHistory);

    if (!cliFiles.isEmpty())
    {
        w.OpenFilesForCli(cliFiles);
    }

    // Stdin is authoritative and replaces file opens from the same launch.
    if (readStdin)
    {
        w.OpenStdinStream();
    }

    // Peers must close before their non-owning history manager expires.
    QList<QPointer<MainWindow>> peers;

    // Compact stale pointers whenever a peer is registered.
    auto appendPeer = [&peers](MainWindow *peer) {
        peers.erase(
            std::remove_if(peers.begin(), peers.end(), [](const QPointer<MainWindow> &p) { return p.isNull(); }),
            peers.end()
        );
        peers.append(QPointer<MainWindow>(peer));
    };

    // Explicit launch inputs take precedence over workspace restoration.
    const bool restoreEnabled = SessionHistoryManager::RestoreLastSessionOnLaunch();
    if (cliFiles.isEmpty() && restoreEnabled && !allowNewInstance && !readStdin)
    {
        // Prefer grouped workspace state; retain surplus windows for another launch.
        slv::persistence::Workspace workspace;
        {
            slv::persistence::Workspace peek = slv::persistence::WorkspacePersistence::Read();
            if (peek.windows.size() > static_cast<std::size_t>(MAX_RESTORE_PEERS))
            {
                logapp::LogWarning() << "Workspace has" << peek.windows.size() << "windows; restoring first"
                                     << MAX_RESTORE_PEERS << "and writing the surplus back for a future launch.";
                slv::persistence::Workspace surplus;
                surplus.schemaVersion = peek.schemaVersion;
                surplus.windows.assign(peek.windows.begin() + MAX_RESTORE_PEERS, peek.windows.end());
                surplus.mruOrder = peek.mruOrder;
                (void)slv::persistence::WorkspacePersistence::Write(std::move(surplus));
                peek.windows.resize(MAX_RESTORE_PEERS);
                workspace = std::move(peek);
            }
            else
            {
                // Atomic consumption prevents repeated restore crashes.
                workspace = slv::persistence::WorkspacePersistence::Take();
            }
        }
        if (!workspace.windows.empty())
        {
            const std::size_t windowCount = workspace.windows.size();
            // Reuse the primary for the first record and create peers for the rest.
            for (std::size_t i = 0; i < windowCount; ++i)
            {
                MainWindow *target = nullptr;
                if (i == 0)
                {
                    target = &w;
                }
                else
                {
                    auto *peer = new MainWindow(&themeControl, &historyManager, &regexTemplateRegistry, nullptr);
                    peer->setAttribute(Qt::WA_DeleteOnClose);
                    peer->show();
                    appendPeer(peer);
                    target = peer;
                }
                target->ApplyWorkspaceWindow(workspace.windows[i]);
            }
        }
        else
        {
            // Consume the compatibility index atomically to avoid restore loops.
            QStringList previouslyOpen = SessionHistoryManager::TakeOpenWindowsAtQuit();
            if (previouslyOpen.size() > MAX_RESTORE_PEERS)
            {
                logapp::LogWarning()
                    << "Truncating restore from" << previouslyOpen.size() << "to" << MAX_RESTORE_PEERS
                    << "peer windows; the surplus stays in the recents index and can be reopened manually.";
                previouslyOpen = previouslyOpen.mid(0, MAX_RESTORE_PEERS);
            }
            if (!previouslyOpen.isEmpty())
            {
                // Reuse the primary and discard missing compatibility entries.
                const QString primaryUuid = previouslyOpen.takeFirst();
                const QString primaryPath = historyManager.PathForUuid(primaryUuid);
                if (!primaryPath.isEmpty() && QFileInfo::exists(primaryPath))
                {
                    w.RestoreLastSessionFromPath(primaryPath);
                }
                else if (logapp::LooksLikeUuid(primaryUuid))
                {
                    historyManager.Remove(primaryUuid);
                }
                for (const QString &uuid : previouslyOpen)
                {
                    const QString peerPath = historyManager.PathForUuid(uuid);
                    if (peerPath.isEmpty() || !QFileInfo::exists(peerPath))
                    {
                        if (logapp::LooksLikeUuid(uuid))
                        {
                            historyManager.Remove(uuid);
                        }
                        continue;
                    }
                    auto *peer = new MainWindow(&themeControl, &historyManager, &regexTemplateRegistry, nullptr);
                    peer->setAttribute(Qt::WA_DeleteOnClose);
                    peer->show();
                    peer->RestoreLastSessionFromPath(peerPath);
                    appendPeer(peer);
                }
            }
            else
            {
                // With no quit state, restore the most recent session.
                const auto lastPath = historyManager.LastSessionPath();
                if (lastPath.has_value())
                {
                    w.RestoreLastSessionFromPath(*lastPath);
                }
            }
        }
    }

    // OS-driven quit must flush all tabs before shared services are destroyed.
    QObject::connect(&a, &QCoreApplication::aboutToQuit, &a, [&peers] {
        // Guard duplicate quit delivery.
        static QAtomicInt fired{0};
        if (fired.fetchAndOrAcquire(1) != 0)
        {
            return;
        }
        QStringList restorable;
        slv::persistence::Workspace workspace;
        workspace.schemaVersion = slv::persistence::WorkspacePersistence::SCHEMA_VERSION;
        // Flushing and closing windows mutates the top-level widget list.
        const QList<QWidget *> topLevels = QApplication::topLevelWidgets();

        // Save every tab, but publish one UUID per window in the compatibility index.
        for (QWidget *widget : topLevels)
        {
            auto *mw = qobject_cast<MainWindow *>(widget);
            if (mw == nullptr)
            {
                continue;
            }
            mw->AutoSaveAllHostedSessions(/*publishOpenWindow=*/false);
            const QString windowUuid = mw->RestorableActiveSessionUuid();
            if (!windowUuid.isEmpty())
            {
                restorable.append(windowUuid);
            }
            slv::persistence::WorkspaceWindow snapshot = mw->CaptureWorkspaceWindow();
            // Persist Qt's stable top-level order when explicit MRU data is unavailable.
            if (!snapshot.windowUuid.isEmpty())
            {
                workspace.mruOrder.append(snapshot.windowUuid);
            }
            workspace.windows.push_back(std::move(snapshot));
        }

        // Close peers before the final batch publish can be altered by close handlers.
        for (const QPointer<MainWindow> &peer : peers)
        {
            if (peer.isNull())
            {
                continue;
            }
            peer->close();
        }

        // Publish only after every close handler has finished.
        SessionHistoryManager::AddOpenWindowUuids(restorable);
        // Keep the flat index for backward-compatible restore readers.
        (void)slv::persistence::WorkspacePersistence::Write(std::move(workspace));
    });

    // Route forwarded files to the active window; an empty request creates a peer.
    QObject::connect(
        &instanceGuard,
        &SingleInstanceGuard::openWindowRequested,
        &a,
        [&themeControl, &historyManager, &regexTemplateRegistry, &appendPeer](
            const QStringList &files, int truncatedCount, SingleInstanceGuard::LaunchFlagsBitmask forwardedFlags
        ) {
            // Fall back from Qt's active window to any existing main window.
            MainWindow *routeTarget = nullptr;
            if (!files.isEmpty())
            {
                QWidget *active = QApplication::activeWindow();
                routeTarget = qobject_cast<MainWindow *>(active);
                if (routeTarget == nullptr)
                {
                    for (QWidget *widget : QApplication::topLevelWidgets())
                    {
                        if (auto *mw = qobject_cast<MainWindow *>(widget); mw != nullptr)
                        {
                            routeTarget = mw;
                            break;
                        }
                    }
                }
            }
            if (routeTarget != nullptr)
            {
                // Preserve a busy active tab before opening forwarded files.
                routeTarget->raise();
                routeTarget->activateWindow();
                routeTarget->SetRotationHistoryLaunchOverride(
                    forwardedFlags.testFlag(SingleInstanceGuard::LaunchFlags::DisableRotationHistory)
                );
                // Forwarded files use the same non-destructive tab routing as OS opens.
                routeTarget->EnsureFreshActiveTab();
                routeTarget->OpenFilesForCli(files);
                if (truncatedCount > 0)
                {
                    constexpr int TRUNCATION_MESSAGE_TIMEOUT_MS = 8000;
                    routeTarget->statusBar()->showMessage(
                        QObject::tr(
                            "Opened forwarded files; %n additional file(s) were dropped (single-launch limit).",
                            nullptr,
                            truncatedCount
                        ),
                        TRUNCATION_MESSAGE_TIMEOUT_MS
                    );
                }
                return;
            }
            // Empty requests and missing route targets need a fresh window.
            auto *child = new MainWindow(&themeControl, &historyManager, &regexTemplateRegistry, nullptr);
            child->setAttribute(Qt::WA_DeleteOnClose);
            child->show();
            child->raise();
            child->activateWindow();
            child->SetRotationHistoryLaunchOverride(
                forwardedFlags.testFlag(SingleInstanceGuard::LaunchFlags::DisableRotationHistory)
            );
            if (!files.isEmpty())
            {
                child->OpenFilesForCli(files);
            }
            if (truncatedCount > 0)
            {
                constexpr int TRUNCATION_MESSAGE_TIMEOUT_MS = 8000;
                child->statusBar()->showMessage(
                    QObject::tr(
                        "Opened forwarded files; %n additional file(s) were dropped (single-launch limit).",
                        nullptr,
                        truncatedCount
                    ),
                    TRUNCATION_MESSAGE_TIMEOUT_MS
                );
            }
            appendPeer(child);
        }
    );

    return QApplication::exec();
}
