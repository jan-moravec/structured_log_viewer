#include "main_window.hpp"

#include "appearance_control.hpp"
#include "session_history_manager.hpp"

#include <QApplication>
#include <QDir>
#include <QStandardPaths>

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

} // namespace

int main(int argc, char *argv[])
{
    const QApplication a(argc, argv);

    QCoreApplication::setOrganizationName("jan-moravec");
    QCoreApplication::setApplicationName("StructuredLogViewer");

    AppearanceControl::LoadConfiguration();

    // Owned by main; lifetime spans every window. MainWindow keeps a
    // non-owning pointer and writes through it on streamingFinished /
    // closeEvent.
    SessionHistoryManager historyManager(RecentSessionsDir(), std::make_unique<QSettingsRecentsIndexStorage>());

    MainWindow w(&historyManager, nullptr);
    w.show();

    return QApplication::exec();
}
