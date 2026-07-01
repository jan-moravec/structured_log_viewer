#include "main_window.hpp"

#include "cli_parser.hpp"
#include "log_warning.hpp"
#include "regex_template_registry.hpp"
#include "session_history_manager.hpp"
#include "single_instance_guard.hpp"
#include "theme_control.hpp"
#include "uuid_utils.hpp"

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

/// Cap on fan-restored peer windows; a pathological persisted set
/// must not trap the launch in lock waits.
constexpr int MAX_RESTORE_PEERS = 25;

/// macOS `QFileOpenEvent` handler. Finder / `open` / "Open With..."
/// deliver paths via this event. Events before the primary window
/// is constructed queue; later ones forward to `OpenFilesForCli`.
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
        // `QEvent::FileOpen` guarantees the dynamic type; Qt does
        // not enable RTTI on `QEvent`.
        auto *fileOpen =
            static_cast<QFileOpenEvent *>(event); // NOLINT(cppcoreguidelines-pro-type-static-cast-downcast)
        const QString path = fileOpen->file();
        if (path.isEmpty())
        {
            return true;
        }
        // Case-preserving display form; dedup keys are computed
        // later in `StreamNextPendingFile`.
        const QString displayPath = logapp::CanonicalDisplayPath(path);
        if (mLiveWindow != nullptr)
        {
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

    // Install the macOS file-open handler before any event pump
    // so pre-`exec()` deliveries are queued instead of dropped.
    FileOpenEventFilter fileOpenFilter;
    qApp->installEventFilter(&fileOpenFilter);

    // `ThemeControl` outlives every `MainWindow`: it's declared
    // here so windows die first (their `themeChanged` connections
    // auto-disconnect), then the theme controller, then
    // `QApplication`.
    ThemeControl themeControl;

    // Same lifetime story for the regex-template registry: its
    // constructor scans `<AppDataLocation>/regex_templates/` and
    // pushes the user slice into `loglib::SetExtraRegexTemplates`,
    // so the auto-detect probe surface sees user templates from
    // the first file the user drops on the window. Constructed
    // here (not in `MainWindow`) so peer windows opened later
    // share one merged registry / one library injection.
    RegexTemplateRegistry regexTemplateRegistry;
    // Best-effort cleanup of the legacy `appearance/*` keys from
    // the pre-theme build. The `contains()` gate keeps the
    // post-migration steady state free of `QSettings::sync` cost.
    // Safe to remove after one release ships.
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

    // Drain pre-`TryAcquire` `QFileOpenEvent`s into `cliFiles` so
    // a forwarding secondary actually forwards what the user
    // double-clicked. A second drain runs after `TryAcquire`.
    QCoreApplication::processEvents(QEventLoop::AllEvents, 0);
    {
        const QStringList preAcquirePending = fileOpenFilter.takePending();
        if (!preAcquirePending.isEmpty())
        {
            cliFiles.append(preAcquirePending);
        }
    }

    // Single-instance coordinator. A secondary forwards its files
    // to the primary and exits.
    SingleInstanceGuard instanceGuard;
    if (!instanceGuard.TryAcquire(cliFiles, allowNewInstance))
    {
        return 0;
    }

    // `--new-instance` peers must not mutate the canonical primary's
    // `openWindowsAtQuit`.
    SessionHistoryManager::SetPublishingEnabled(!allowNewInstance);

    // Init the IANA timezone database before any timestamp work
    // (restore-on-launch rehydrates filters that format timestamps).
    if (!MainWindow::InitializeTimezoneDatabase())
    {
        return 1;
    }

    // Owned by main; outlives every window (closeEvent /
    // aboutToQuit both deref the manager).
    SessionHistoryManager historyManager(
        SessionHistoryManager::DefaultSessionsDir(), std::make_unique<QSettingsRecentsIndexStorage>()
    );

    // Reap `<uuid>.json` files left behind by a crash between
    // `WriteSnapshot`'s file write and its index update. Capped
    // result feeds a status-bar hint.
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

    // Second drain for events that landed during startup
    // (Finder double-click on macOS).
    const QStringList pendingFromOs = fileOpenFilter.takePending();
    if (!pendingFromOs.isEmpty())
    {
        cliFiles.append(pendingFromOs);
    }

    if (!cliFiles.isEmpty())
    {
        w.OpenFilesForCli(cliFiles);
    }

    // Track peer windows so we can close + reap them before
    // `historyManager` goes out of scope (avoids a latent UAF if
    // `aboutToQuit` runs after the manager).
    QList<QPointer<MainWindow>> peers;

    // Append a peer and compact null `QPointer`s. Hand-rolled
    // `remove_if` so Qt version differences in `removeIf` /
    // `std::erase_if` don't matter.
    auto appendPeer = [&peers](MainWindow *peer) {
        peers.erase(
            std::remove_if(peers.begin(), peers.end(), [](const QPointer<MainWindow> &p) { return p.isNull(); }),
            peers.end()
        );
        peers.append(QPointer<MainWindow>(peer));
    };

    // Restore-on-launch (Preferences toggle). Skipped when CLI
    // files are present or `--new-instance` is set.
    const bool restoreEnabled = SessionHistoryManager::RestoreLastSessionOnLaunch();
    if (cliFiles.isEmpty() && restoreEnabled && !allowNewInstance)
    {
        // Atomic read + wipe so a crash mid-restore cannot loop us.
        QStringList previouslyOpen = SessionHistoryManager::TakeOpenWindowsAtQuit();

        if (previouslyOpen.size() > MAX_RESTORE_PEERS)
        {
            logapp::LogWarning(
            ) << "Truncating restore from"
              << previouslyOpen.size() << "to" << MAX_RESTORE_PEERS
              << "peer windows; the surplus stays in the recents index and can be reopened manually.";
            previouslyOpen = previouslyOpen.mid(0, MAX_RESTORE_PEERS);
        }

        if (!previouslyOpen.isEmpty())
        {
            // First uuid -> existing primary; remainder -> peers.
            // Dangling uuids are evicted so the menu stays clean.
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
            // Fallback for clean installs: restore the single most
            // recent session.
            const auto lastPath = historyManager.LastSessionPath();
            if (lastPath.has_value())
            {
                w.RestoreLastSessionFromPath(*lastPath);
            }
        }
    }

    // Safety-net for OS-driven quit (Cmd+Q, login teardown), where
    // the event loop exits without `closeEvent` firing on every
    // window.
    //
    // (1) flush + gather restorable uuids, (2) explicitly close
    // peers so their `closeEvent` runs while the manager is alive,
    // (3) one batched `AddOpenWindowUuids` after (2) so peers
    // cannot strip their own uuids out of the set on close.
    QObject::connect(&a, &QCoreApplication::aboutToQuit, &a, [&peers] {
        // `aboutToQuit` has been observed to fire twice on rare
        // shutdown paths.
        static QAtomicInt fired{0};
        if (fired.fetchAndOrAcquire(1) != 0)
        {
            return;
        }
        QStringList restorable;
        // Snapshot the widget list because flush -> closeEvent
        // can mutate it under-foot.
        const QList<QWidget *> topLevels = QApplication::topLevelWidgets();

        // Phase 1: flush + gather; publish runs in phase 3.
        for (QWidget *widget : topLevels)
        {
            auto *mw = qobject_cast<MainWindow *>(widget);
            if (mw == nullptr)
            {
                continue;
            }
            mw->AutoSaveSessionSnapshot(/*publishOpenWindow=*/false);
            const QString uuid = mw->RestorableActiveSessionUuid();
            if (!uuid.isEmpty())
            {
                restorable.append(uuid);
            }
        }

        // Phase 2: close peers; `closeEvent` -> `DetachAutoSaveUuid`
        // strips any stale prior publish before phase 3 re-adds.
        for (const QPointer<MainWindow> &peer : peers)
        {
            if (peer.isNull())
            {
                continue;
            }
            peer->close();
        }

        // Phase 3: batched publish. No-op under `--new-instance`.
        SessionHistoryManager::AddOpenWindowUuids(restorable);
    });

    // Forwarded launches spawn a new peer sharing the recents
    // store. An empty file list still opens an empty window
    // (mirrors VS Code's "second launch -> new window" UX).
    QObject::connect(
        &instanceGuard,
        &SingleInstanceGuard::openWindowRequested,
        &a,
        [&themeControl, &historyManager, &regexTemplateRegistry, &appendPeer](
            const QStringList &files, int truncatedCount
        ) {
            auto *child = new MainWindow(&themeControl, &historyManager, &regexTemplateRegistry, nullptr);
            child->setAttribute(Qt::WA_DeleteOnClose);
            child->show();
            child->raise();
            child->activateWindow();
            if (!files.isEmpty())
            {
                child->OpenFilesForCli(files);
            }
            // Forward the secondary's wire-cap truncation hint.
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
