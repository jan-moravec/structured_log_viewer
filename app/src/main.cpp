#include "main_window.hpp"

#include "appearance_control.hpp"
#include "session_history_manager.hpp"
#include "single_instance_guard.hpp"

#include <QApplication>
#include <QDir>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QStringList>

#include <memory>

namespace
{

/// Per-user directory under `AppDataLocation` for the recents-index
/// per-uuid JSON files. Created lazily on first `WriteSnapshot`. The
/// path is shared across all windows so multi-window history stays
/// consistent.
QDir RecentSessionsDir()
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (base.isEmpty())
    {
        // Fallback used when the platform refuses to compute the
        // path (rare; mostly portable-mode setups). Sub-folder
        // tagged so the directory is easy to wipe by hand.
        base = QDir::tempPath();
    }
    return QDir(base).filePath(QStringLiteral("sessions"));
}

/// CLI parse: collect every positional argument as a candidate file
/// path; ignore the binary name and any flag we own. Anything not
/// understood is forwarded to the primary, which is then free to
/// reject it with a parse error (matches how the existing OpenFiles
/// flow handles bad paths).
QStringList CollectCliFiles(const QStringList &args)
{
    QStringList files;
    files.reserve(args.size());
    bool sawProgramName = false;
    for (const QString &arg : args)
    {
        if (!sawProgramName)
        {
            // First entry is the program path; skip exactly one.
            sawProgramName = true;
            continue;
        }
        if (arg.startsWith(QStringLiteral("--")))
        {
            continue;
        }
        files.append(arg);
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
    SessionHistoryManager historyManager(RecentSessionsDir(), std::make_unique<QSettingsRecentsIndexStorage>());

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
    if (cliFiles.isEmpty() && SessionHistoryManager::RestoreLastSessionOnLaunch())
    {
        const auto lastPath = historyManager.LastSessionPath();
        if (lastPath.has_value())
        {
            // Failures inside the restore flow surface a dialog from
            // `DoLoadConfiguration`; on success the window is now
            // mirroring the restored session.
            w.RestoreLastSessionFromPath(*lastPath);
        }
    }

    // Forwarded launches from secondary processes spawn a new
    // `MainWindow` peer that shares the recents store. The new
    // window opens the forwarded files (if any) into its own
    // session; an empty list still produces a fresh empty window
    // (mirrors VS Code's "open a new window on second launch" UX).
    QObject::connect(&instanceGuard, &SingleInstanceGuard::openWindowRequested, &a, [&](const QStringList &files) {
        auto *child = new MainWindow(&historyManager, nullptr);
        child->setAttribute(Qt::WA_DeleteOnClose);
        child->show();
        child->raise();
        child->activateWindow();
        if (!files.isEmpty())
        {
            child->OpenFilesForCli(files);
        }
    });

    return QApplication::exec();
}
