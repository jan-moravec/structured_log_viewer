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
///
/// Limitations of this hand-rolled parser (kept deliberately small;
/// migrate to `QCommandLineParser` once the flag set grows past a
/// second entry):
///   - No `--flag=value` syntax. Every flag we own today
///     (`--new-instance`) is boolean / valueless. Adding the first
///     value-bearing flag is the trigger for migration.
///   - Unrecognised long-form `--foo` and short-form `-x` are
///     silently dropped (no `--help`, no error). A typo like
///     `--new-instnace` therefore behaves identically to no flag,
///     which is annoying but safer than mis-routing the typo to a
///     file open. The migration to `QCommandLineParser` would
///     surface unknown flags via its built-in error path.
///   - The flag list is duplicated between `CollectCliFiles` (which
///     filters them out) and `ShouldAllowNewInstance` (which
///     interprets them). New flags must be reflected in both -- a
///     `QCommandLineParser` migration would consolidate both into a
///     single declarative table.
///   - `--` end-of-options is honoured so `app -- -weird-name.log`
///     opens a dash-prefixed file. Same semantics as
///     `QCommandLineParser`, kept for forward-compat with the
///     migration.
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

    // Initialise the IANA timezone database *before* any code path
    // that formats timestamps. The restore-on-launch flow below calls
    // `RestoreLastSessionFromPath` synchronously (i.e. before
    // `exec()`), and that path rehydrates filters whose titles run
    // through `loglib::UtcMicrosecondsToDateTimeString`. The first
    // call to that helper materialises the date library's process-
    // wide zone cache; without a prior `loglib::Initialize` the date
    // library would probe its platform-default install path (on
    // Windows: `<user-profile>/Downloads/tzdata`, which is not the
    // location the build / installer ships tzdata to) and throw,
    // surfacing as a misleading "Error Parsing Configuration"
    // dialog. Initialising here -- before the manager + windows are
    // built -- collapses that footgun: every subsequent timestamp
    // helper sees an initialised zone cache regardless of which
    // entry point reaches it first.
    if (!MainWindow::InitializeTimezoneDatabase())
    {
        return 1;
    }

    // Owned by main; lifetime spans every window. MainWindow keeps a
    // non-owning pointer and writes through it on streamingFinished /
    // closeEvent.
    //
    // Declaration order matters: this must be constructed *before*
    // both the primary `w` below and every heap-allocated peer
    // window in the restore loop, because stack unwinding tears
    // those down first (and `WA_DeleteOnClose` peers also die
    // before `exec()` returns). If a future refactor moves
    // `historyManager` after any window construction, the windows
    // would outlive the manager they point at.
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
    // does not race the restore) and only on the canonical primary
    // (`--new-instance` peers start blank by design -- they are an
    // escape hatch for "I want a fresh process without disturbing
    // the running primary", and inheriting the running primary's
    // open-windows set would defeat that intent and silently
    // duplicate sessions across both processes).
    const bool restoreEnabled = SessionHistoryManager::RestoreLastSessionOnLaunch();
    if (cliFiles.isEmpty() && restoreEnabled && !allowNewInstance)
    {
        // Multi-window restore: if the previous shutdown captured
        // an `openWindowsAtQuit` list, restore every session in it.
        // The first uuid populates the primary window (already
        // shown); the rest fan out into peer windows. Falls back to
        // single-session restore via `LastSessionPath` when the list
        // is empty (clean install / first launch after this commit).
        //
        // `TakeOpenWindowsAtQuit` reads and wipes the persisted list
        // atomically under the cross-process lock. The wipe prevents
        // a crash mid-restore from looping us forever on the same
        // uuids; the atomicity prevents a sibling writer from
        // disappearing between the read and the wipe.
        QStringList previouslyOpen = SessionHistoryManager::TakeOpenWindowsAtQuit();

        if (!previouslyOpen.isEmpty())
        {
            // First uuid into the already-shown primary; remainder
            // get fresh windows.
            //
            // Every path we pass to `RestoreLastSessionFromPath` here
            // is produced by `historyManager.PathForUuid(uuid)`, so
            // the stem is always a UUID and the uuid-pin branch of
            // that function applies. The fallback below (single-window
            // restore via `LastSessionPath`) preserves the same
            // invariant: `lastSessionUuid` is uuid-shaped by
            // construction. The non-uuid-stem branch documented on
            // `RestoreLastSessionFromPath` is reserved for tests and
            // for hypothetical future callers that pass ad-hoc paths.
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
    // Two-phase batched fan:
    //
    //   1. Loop windows once, calling
    //      `AutoSaveSessionSnapshot(publishOpenWindow=false)` to
    //      flush any post-`streamingFinished` edits (filter tweaks,
    //      sort changes, column moves) into the recents JSON
    //      *without* touching the persisted open-windows key. Each
    //      window's `RestorableActiveSessionUuid()` is collected into
    //      a local `QStringList` instead. The flush itself is a no-op
    //      on live-tail / network-stream / no-source windows because
    //      `ShouldAutoSaveSession` rejects them.
    //
    //   2. Issue a single `AddOpenWindowUuids(restorable)` call so
    //      all collected uuids enter the persisted set under one
    //      cross-process lock acquisition. Pre-fix this loop did
    //      N * `AddOpenWindowUuid` (and N * `WriteSnapshot` -- the
    //      `publishOpenWindow=true` argument made the per-window
    //      snapshot itself a second publisher), so an OS-driven
    //      shutdown with M restorable windows paid up to
    //      2M * `WRITE_LOCK_TIMEOUT_RUNTIME_MS` worth of lock waits.
    //      The batched primitive bounds the contribution from this
    //      handler to a single
    //      `WRITE_LOCK_TIMEOUT_SHUTDOWN_MS` slot regardless of M.
    //
    // The collected list covers two cases that previously needed
    // separate code paths: (a) a finished static session whose uuid
    // the per-window `publishOpenWindow=true` AutoSave used to
    // republish, and (b) a live-tail-on-a-file window whose uuid was
    // pinned by a previous static load -- `RestorableActiveSessionUuid`
    // returns it (the JSON on disk still reflects the static view,
    // which is what the user would expect to come back after the OS
    // quit) but `ShouldAutoSaveSession` rejects it for re-flush.
    //
    // We *merge* rather than overwrite: with `--new-instance` two
    // processes can be alive simultaneously, and a destructive
    // `SetOpenWindowsAtQuit(restorable)` from the first quitter
    // would clobber the other process's published uuids.
    // `AddOpenWindowUuids` honours the same idempotent merge as the
    // per-uuid `AddOpenWindowUuid`.
    QObject::connect(&a, &QCoreApplication::aboutToQuit, &a, [] {
        QStringList restorable;
        for (QWidget *widget : QApplication::topLevelWidgets())
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
        SessionHistoryManager::AddOpenWindowUuids(restorable);
    });

    // Forwarded launches from secondary processes spawn a new
    // `MainWindow` peer that shares the recents store. The new
    // window opens the forwarded files (if any) into its own
    // session; an empty list still produces a fresh empty window
    // (mirrors VS Code's "open a new window on second launch" UX).
    //
    // Lifetime invariant: this lambda captures `&historyManager` by
    // reference. `historyManager` is declared above in the same
    // scope, so it outlives both `instanceGuard` (which is destroyed
    // on stack unwind after `historyManager`) and every peer window
    // produced here. The peer is heap-allocated with
    // `WA_DeleteOnClose`, so it is reaped through its closeEvent's
    // `deleteLater` while `exec()` is still pumping events --
    // guaranteeing the peer's `mHistoryManager` deref in
    // `closeEvent` (auto-save flush) sees a live manager. The
    // `&a` context object further ensures the lambda is disconnected
    // before `QApplication` is torn down.
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
