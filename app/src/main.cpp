#include "main_window.hpp"

#include "appearance_control.hpp"
#include "session_history_manager.hpp"
#include "single_instance_guard.hpp"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QStringList>

#include <memory>

namespace
{

/// CLI parse: collect every positional argument as a candidate file
/// path; ignore the binary name and any flag we own. Anything not
/// understood is forwarded to the primary, which is then free to
/// reject it with a parse error (matches how the existing OpenFiles
/// flow handles bad paths).
///
/// Paths are converted to absolute against the caller's current
/// working directory *before* forwarding: with single-instance
/// coordination on, the primary process probably runs from a
/// different CWD than the secondary, so a relative path that means
/// "the file next to me" on the secondary would resolve against the
/// primary's CWD and fail to open.
QStringList CollectCliFiles(const QStringList &args)
{
    QStringList files;
    files.reserve(args.size());
    bool sawProgramName = false;
    bool afterDoubleDash = false;
    for (const QString &arg : args)
    {
        if (!sawProgramName)
        {
            // First entry is the program path; skip exactly one.
            sawProgramName = true;
            continue;
        }
        if (!afterDoubleDash && arg == QStringLiteral("--"))
        {
            // POSIX end-of-options separator: everything that
            // follows is treated as a positional argument, even when
            // it starts with `-`. Lets the user open files whose
            // names begin with a dash (rare but possible on Linux:
            // `app -- -weird-filename.log`) without our flag filter
            // dropping them. The `--` token itself is not a file.
            afterDoubleDash = true;
            continue;
        }
        if (!afterDoubleDash && arg.startsWith(QStringLiteral("-")))
        {
            // Drop everything that looks like a flag (long `--foo`
            // *or* short `-x`) so a future option doesn't get
            // misclassified as a file path. The flags we recognise
            // are handled below in `ShouldAllowNewInstance`.
            continue;
        }
        // `absoluteFilePath` resolves against the *current* CWD
        // without requiring the file to exist (unlike
        // `canonicalFilePath`, which returns empty for missing
        // files and would silently drop a user typo here).
        files.append(QFileInfo(arg).absoluteFilePath());
    }
    return files;
}

bool ShouldAllowNewInstance(const QStringList &args)
{
    if (args.contains(QStringLiteral("--new-instance")))
    {
        return true;
    }
    const QString envOverride = QProcessEnvironment::systemEnvironment().value(QStringLiteral("LOGAPP_NEW_INSTANCE"));
    return envOverride == QStringLiteral("1") || envOverride.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
}

} // namespace

int main(int argc, char *argv[])
{
    const QApplication a(argc, argv);

    QCoreApplication::setOrganizationName("jan-moravec");
    QCoreApplication::setApplicationName("StructuredLogViewer");

    AppearanceControl::LoadConfiguration();

    const QStringList cliArgs = QCoreApplication::arguments();
    const QStringList cliFiles = CollectCliFiles(cliArgs);
    const bool allowNewInstance = ShouldAllowNewInstance(cliArgs);

    // Single-instance coordinator: try to take the primary role. A
    // secondary launch forwards its parsed files to the primary and
    // returns immediately so the OS-launched process is gone before
    // the user notices.
    SingleInstanceGuard instanceGuard;
    if (!instanceGuard.TryAcquire(cliFiles, allowNewInstance))
    {
        // Forwarded; the primary will spawn a window for us. Exit
        // cleanly so the user sees one application even though they
        // double-clicked the binary twice.
        return 0;
    }

    // Owned by main; lifetime spans every window. MainWindow keeps a
    // non-owning pointer and writes through it on streamingFinished /
    // closeEvent.
    SessionHistoryManager historyManager(
        SessionHistoryManager::DefaultSessionsDir(), std::make_unique<QSettingsRecentsIndexStorage>()
    );

    // One-shot orphan sweep: a crash between `WriteSnapshot`'s
    // per-uuid JSON write and the index update leaves `<uuid>.json`
    // on disk with no recents entry pointing at it. Without this
    // cleanup the sessions dir grows monotonically across crashes.
    // Cheap (one directory listing + set membership check) and safe
    // to run while concurrent processes hold the lock file -- a
    // sibling primary in the middle of `WriteSnapshot` has not yet
    // updated the index, so its in-flight JSON could in principle
    // be misclassified as an orphan; the `removeServer`-bounded
    // single-instance guarantee above means there is no such sibling
    // primary at this point.
    historyManager.CleanupOrphanFiles();

    MainWindow w(&historyManager, nullptr);
    w.show();

    // Open any CLI-provided files in the primary's first window.
    // Append mode so a future `--config` switch can preload filters
    // before the open without them being clobbered.
    if (!cliFiles.isEmpty())
    {
        w.OpenFilesForCli(cliFiles);
    }

    // Restore-on-launch: opt-in via Preferences. Only fires when no
    // CLI files were passed (so the user explicitly opening a file
    // does not race the restore) and only when the manager has a
    // last-session pointer. The single-instance coordinator above
    // guarantees we only run this in the primary process.
    const bool restoreEnabled = SessionHistoryManager::RestoreLastSessionOnLaunch();
    if (cliFiles.isEmpty() && restoreEnabled)
    {
        // Multi-window restore: if the previous shutdown captured
        // an `openWindowsAtQuit` list, restore every session in it.
        // The first uuid populates the primary window (already
        // shown); the rest fan out into peer windows. Falls back to
        // single-session restore via `LastSessionPath` when the list
        // is empty (clean install / first launch after this commit).
        QStringList previouslyOpen = SessionHistoryManager::OpenWindowsAtQuit();
        // Wipe the persisted list so a crash mid-restore does not
        // loop us forever on the same uuids.
        SessionHistoryManager::SetOpenWindowsAtQuit({});

        if (!previouslyOpen.isEmpty())
        {
            // First uuid into the already-shown primary; remainder
            // get fresh windows.
            const QString primaryUuid = previouslyOpen.takeFirst();
            const QString primaryPath = historyManager.PathForUuid(primaryUuid);
            if (QFileInfo::exists(primaryPath))
            {
                w.RestoreLastSessionFromPath(primaryPath);
            }
            for (const QString &uuid : previouslyOpen)
            {
                const QString peerPath = historyManager.PathForUuid(uuid);
                if (!QFileInfo::exists(peerPath))
                {
                    continue;
                }
                auto *peer = new MainWindow(&historyManager, nullptr);
                peer->setAttribute(Qt::WA_DeleteOnClose);
                peer->show();
                peer->RestoreLastSessionFromPath(peerPath);
            }
        }
        else
        {
            const auto lastPath = historyManager.LastSessionPath();
            if (lastPath.has_value())
            {
                w.RestoreLastSessionFromPath(*lastPath);
            }
        }
    }

    // Safety-net at shutdown. The persisted `openWindowsAtQuit` list
    // is maintained eagerly by `AutoSaveSessionSnapshot` (Add) and
    // `closeEvent` (Remove); by the time `aboutToQuit` fires the list
    // already reflects reality for the user-initiated-close case.
    //
    // The case this handler still matters for is OS-driven quit
    // (Cmd+Q on macOS, login session teardown on X11, ...) where the
    // event loop exits *without* `closeEvent` firing on every window.
    // In that path `topLevelWidgets()` still holds live `MainWindow`s
    // when `aboutToQuit` is emitted from `exit()`, so we capture them
    // here.
    //
    // We *merge* rather than overwrite: with `--new-instance` two
    // processes can be alive simultaneously, and a destructive
    // `SetOpenWindowsAtQuit(openUuids)` from the first quitter would
    // clobber the other process's published uuids. `AddOpenWindowUuid`
    // is idempotent and serialised through the cross-process lock,
    // so each process's contribution survives independently.
    //
    // We deliberately use `RestorableActiveSessionUuid` rather than
    // `ActiveSessionUuid` so windows whose session cannot be
    // fan-restored (e.g. a legacy NetworkStream entry the user
    // opened from Recent Sessions) do not get re-published on every
    // OS-quit. Without this filter the user would see the "must
    // re-bind manually" info popup on every subsequent launch until
    // they manually cleared the entry.
    QObject::connect(&a, &QCoreApplication::aboutToQuit, &a, [] {
        for (QWidget *widget : QApplication::topLevelWidgets())
        {
            auto *mw = qobject_cast<MainWindow *>(widget);
            if (mw == nullptr)
            {
                continue;
            }
            const QString uuid = mw->RestorableActiveSessionUuid();
            if (!uuid.isEmpty())
            {
                SessionHistoryManager::AddOpenWindowUuid(uuid);
            }
        }
    });

    // Forwarded launches from secondary processes spawn a new
    // `MainWindow` peer that shares the recents store. The new
    // window opens the forwarded files (if any) into its own
    // session; an empty list still produces a fresh empty window
    // (mirrors VS Code's "open a new window on second launch" UX).
    QObject::connect(
        &instanceGuard, &SingleInstanceGuard::openWindowRequested, &a, [&historyManager](const QStringList &files) {
            auto *child = new MainWindow(&historyManager, nullptr);
            child->setAttribute(Qt::WA_DeleteOnClose);
            child->show();
            child->raise();
            child->activateWindow();
            if (!files.isEmpty())
            {
                child->OpenFilesForCli(files);
            }
        }
    );

    return QApplication::exec();
}
