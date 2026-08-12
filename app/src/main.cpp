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
            // Post-tabs review-round bug #4 fix: route the
            // incoming file to a fresh foreground tab so the
            // MRU window's current investigation is not
            // clobbered. Symmetric with the single-instance
            // forward path in `main()` (see the
            // `EnsureFreshActiveTab()` call site near the
            // `SingleInstanceGuard` route branch). Before the
            // fix, macOS "Open With..." forwarded files (via
            // `QFileOpenEvent`) landed as an Append into
            // whatever session was active, silently corrupting
            // a busy tab.
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

    // Install the macOS file-open handler before any event pump
    // so pre-`exec()` deliveries are queued instead of dropped.
    FileOpenEventFilter fileOpenFilter;
    qApp->installEventFilter(&fileOpenFilter);

    // `ThemeControl` outlives every `MainWindow`: it's declared
    // here so windows die first (their `themeChanged` connections
    // auto-disconnect), then the theme controller, then
    // `QApplication`.
    ThemeControl themeControl;

    // Same lifetime story for the regex-template registry.
    // Its constructor scans `<AppDataLocation>/regex_templates/`
    // and hands the user slice to `loglib::SetExtraRegexTemplates`
    // so the auto-detect probe sees user templates immediately.
    // One registry per process (not per window) so peer windows
    // share the same merged catalog and library injection.
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
    const bool readStdin = parsed.readStdin;

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
    //
    // `-` / `--stdin` cannot be forwarded across processes (each
    // process has its own stdin FD), so a stdin launch behaves like
    // an implicit `--new-instance`. Skipping the forward keeps this
    // process alive to actually consume its stdin; otherwise the
    // pipe reader on the far end would silently attach to a primary
    // that never opens stdin.
    const bool effectiveAllowNewInstance = allowNewInstance || readStdin;
    SingleInstanceGuard instanceGuard;
    // Forward per-launch options to an existing primary process.
    SingleInstanceGuard::LaunchFlagsBitmask launchFlags;
    if (parsed.disableRotationHistory)
    {
        launchFlags |= SingleInstanceGuard::LaunchFlags::DisableRotationHistory;
    }
    if (!instanceGuard.TryAcquire(cliFiles, effectiveAllowNewInstance, launchFlags))
    {
        return 0;
    }

    // Explicit or stdin-implied new instances must not publish the
    // canonical primary's `openWindowsAtQuit` state.
    SessionHistoryManager::SetPublishingEnabled(!effectiveAllowNewInstance);

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

    // Apply the launch override before opening files.
    w.SetRotationHistoryLaunchOverride(parsed.disableRotationHistory);

    if (!cliFiles.isEmpty())
    {
        w.OpenFilesForCli(cliFiles);
    }

    // Stdin opens last and replaces any queued or in-flight file
    // opens, making the pipe the current session.
    if (readStdin)
    {
        w.OpenStdinStream();
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

    // Restore-on-launch is skipped for CLI files and new-instance
    // launches. Stdin is also skipped because it already replaced
    // the current session.
    const bool restoreEnabled = SessionHistoryManager::RestoreLastSessionOnLaunch();
    if (cliFiles.isEmpty() && restoreEnabled && !allowNewInstance && !readStdin)
    {
        // Task 8.7 grouped restore: prefer the workspace file
        // (structured windows/tabs) when present; fall back to
        // the flat `openWindowsAtQuit` list for backward
        // compatibility with pre-8.x quit state.
        //
        // Post-tabs review-round bug #8 fix: previously we called
        // `Take()` (atomic read + wipe) unconditionally and then
        // truncated the in-memory copy to `MAX_RESTORE_PEERS`.
        // For a workspace with more than 25 windows, the surplus
        // was silently DROPPED from the on-disk store because
        // the wipe happened before truncation. Now we `Read()`
        // first, and if the count exceeds the cap we write the
        // surplus BACK to the on-disk store so a subsequent
        // launch can pick up the rest; only the count-under-cap
        // common case uses the atomic `Take()` for its crash-
        // loop protection. Split-write is done atomically via
        // `Write()` (which itself writes via `QSaveFile`), so a
        // crash between the split-write and the restore leaves
        // exactly the surplus on-disk (idempotent).
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
                // Common case: atomic read+wipe protects against
                // mid-restore crash loops.
                workspace = slv::persistence::WorkspacePersistence::Take();
            }
        }
        if (!workspace.windows.empty())
        {
            const std::size_t windowCount = workspace.windows.size();
            // First window rebinds the freshly-constructed
            // primary; subsequent windows are new peers.
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
            // Atomic read + wipe so a crash mid-restore cannot loop us.
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
        slv::persistence::Workspace workspace;
        workspace.schemaVersion = slv::persistence::WorkspacePersistence::SCHEMA_VERSION;
        // Snapshot the widget list because flush -> closeEvent
        // can mutate it under-foot.
        const QList<QWidget *> topLevels = QApplication::topLevelWidgets();

        // Phase 1: flush + gather; publish runs in phase 3.
        //
        // Multi-tab: walk EVERY hosted tab of every window so
        // `AutoSaveAllHostedSessions` routes each per-tab save
        // through the shell's alias-based helpers -- background
        // tabs' state is preserved, not just the last-active tab
        // of each window.
        //
        // Post-tabs review-round bug #H3 fix: the FLAT
        // `openWindowsAtQuit` list (the pre-8.x legacy fallback
        // consumed at line 337-349 below) treats one entry as
        // ONE window (that's the pre-tabs contract and the
        // reader still expects it). A prior revision published
        // every per-tab uuid via `RestorableHostedSessionUuids`,
        // so a 3-tab window would explode into 3 separate
        // windows when a subsequent launch fell back to the
        // legacy branch (i.e. a workspace-file write failure,
        // an abnormal exit before the workspace publish, or a
        // downgraded binary). Publish ONE uuid per window here
        // (the active session's restorable uuid, matching the
        // pre-tabs semantics); the full multi-tab layout lives
        // in the grouped workspace record captured just below.
        //
        // Task 8.6: also snapshot each window's tab layout into
        // the workspace record for grouped restore next launch.
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
            // MRU order: primary window is first in
            // `topLevelWidgets()` on most platforms, but do not
            // rely on that -- use focus/activation order via a
            // stable adjacent sweep. For today we accept
            // insertion order (matches `topLevelWidgets()`).
            if (!snapshot.windowUuid.isEmpty())
            {
                workspace.mruOrder.append(snapshot.windowUuid);
            }
            workspace.windows.push_back(std::move(snapshot));
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
        // Publish the workspace snapshot alongside the flat
        // uuid list. Safe to publish even under `--new-instance`
        // (the WorkspacePersistence::Write guard returns false
        // silently). The flat list is kept for the current
        // release so a downgraded binary can still restore.
        (void)slv::persistence::WorkspacePersistence::Write(std::move(workspace));
    });

    // Forwarded launches: route to the MRU (most-recently-focused)
    // MainWindow when files are supplied so the incoming files
    // land as new foreground tabs on the window the user was
    // last looking at (task 8.11). An EMPTY file list still
    // spawns a new peer to preserve the pre-8.x "second launch
    // -> new window" UX and to give the user a clean surface
    // when they explicitly launched a fresh instance.
    QObject::connect(
        &instanceGuard,
        &SingleInstanceGuard::openWindowRequested,
        &a,
        [&themeControl, &historyManager, &regexTemplateRegistry, &appendPeer](
            const QStringList &files, int truncatedCount, SingleInstanceGuard::LaunchFlagsBitmask forwardedFlags
        ) {
            // Locate the MRU MainWindow. `activeWindow()` reflects
            // the last window Qt saw focus on this process; a
            // null return (headless / minimized) falls back to
            // the first MainWindow in `topLevelWidgets()`, then
            // to a fresh peer.
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
                // Route to existing MRU: `EnsureFreshActiveTab()`
                // is called explicitly BELOW so a busy tab is
                // not clobbered; `OpenFilesForCli` alone would
                // Append into whatever tab was active. The
                // incoming files land on the new foreground
                // tab created by `EnsureFreshActiveTab`.
                routeTarget->raise();
                routeTarget->activateWindow();
                routeTarget->SetRotationHistoryLaunchOverride(
                    forwardedFlags.testFlag(SingleInstanceGuard::LaunchFlags::DisableRotationHistory)
                );
                // Give the incoming files their own tab so the
                // MRU window's current investigation stays
                // intact. `AddNewTab(true)` before the open
                // is the same shape as the FR-24 "Open in
                // New Tab" action.
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
            // Empty-file forward: spawn a fresh peer (pre-8.x
            // shape). Also covers the fallback branch where
            // no MainWindow was reachable for routing.
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
