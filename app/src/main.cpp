#include "main_window.hpp"

#include "appearance_control.hpp"
#include "cli_parser.hpp"
#include "log_warning.hpp"
#include "session_history_manager.hpp"
#include "single_instance_guard.hpp"
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
#include <QStatusBar>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <memory>

namespace
{

/// Hard cap on the number of peer windows we will fan-restore from a
/// previous-launch snapshot. 25 is well above the user-meaningful
/// ceiling (anyone with 25 log windows open is already past the
/// point where the UI is useful) and well below the value at which
/// the cumulative cross-process lock waits stop being bounded.
/// Pre-fix this loop was unbounded, so a pathological persisted
/// state could trap the launch in restore for tens of seconds.
constexpr int MAX_RESTORE_PEERS = 25;

/// macOS-specific forwarder for `QFileOpenEvent`. The Finder /
/// `open` command and "Open With..." menu deliver requests via
/// this event; without a handler, a double-click of a `.log` file
/// in Finder launches the app but the file never opens. We install
/// it as an event filter on `qApp` so we can route requests
/// through whichever subsystem is appropriate based on lifecycle:
///   - Before the primary window is constructed (i.e. the events
///     arrived during `QApplication` startup, very common on macOS
///     where launch services delivers them before `exec()`), we
///     append the file to a pending queue that `main()` drains
///     into the first window once it constructs.
///   - After the primary window is up, we forward the file
///     directly to `MainWindow::OpenFilesForCli` so it lands in the
///     current session (matching the CLI / forward semantics).
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
        // Safe downcast: Qt's `QEvent::FileOpen` type discriminator
        // guarantees the dynamic type is `QFileOpenEvent`. Qt does
        // not enable RTTI on `QEvent`, so `dynamic_cast` is not the
        // canonical idiom here.
        auto *fileOpen =
            static_cast<QFileOpenEvent *>(event); // NOLINT(cppcoreguidelines-pro-type-static-cast-downcast)
        const QString path = fileOpen->file();
        if (path.isEmpty())
        {
            return true;
        }
        // Display form (case-preserving) so a Finder "Open With..."
        // path appears in the status bar exactly as the user sees it.
        // The downstream `StreamNextPendingFile` computes the dedup
        // key at the point where the path actually lands on a
        // `Source` descriptor.
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

    // Install the macOS file-open handler immediately so events that
    // arrive between `QApplication` construction and `exec()` (the
    // double-click-to-launch flow) are queued instead of dropped.
    FileOpenEventFilter fileOpenFilter;
    qApp->installEventFilter(&fileOpenFilter);

    AppearanceControl::LoadConfiguration();

    const logapp::ParsedCli parsed =
        logapp::ParseCli(QCoreApplication::arguments(), QProcessEnvironment::systemEnvironment());
    QStringList cliFiles = parsed.files;
    const bool allowNewInstance = parsed.allowNewInstance;

    // Drain pre-`TryAcquire` `QFileOpenEvent`s into `cliFiles` so a
    // forwarding secondary actually forwards the file the user
    // double-clicked in Finder. On macOS, double-clicking a `.log`
    // while the app is already running launches a second process
    // and delivers the path as a `QFileOpenEvent`; that event lands
    // in `fileOpenFilter.mPending` during construction (or arrives
    // while we are parsing CLI args). Without this drain, a
    // secondary's `TryAcquire` would forward only the argv
    // positionals -- typically empty for a Finder double-click --
    // and the user's file would be silently dropped on the floor.
    // The single `processEvents` pump lets any in-flight launch-
    // services event reach the filter before we read it. The
    // primary path keeps the post-`TryAcquire` drain below to catch
    // events that arrived during `TryAcquire`'s server takeover.
    QCoreApplication::processEvents(QEventLoop::AllEvents, 0);
    {
        const QStringList preAcquirePending = fileOpenFilter.takePending();
        if (!preAcquirePending.isEmpty())
        {
            cliFiles.append(preAcquirePending);
        }
    }

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

    // `--new-instance` isolation: gate the persisted-set mutators so
    // this process can neither publish into nor remove from the
    // canonical primary's `openWindowsAtQuit` set. The user opted out
    // of single-instance coordination ("don't disturb the running
    // primary"); inheriting or polluting the canonical restore-set
    // would silently violate that intent across launches (a peer
    // session would either be fan-restored into the canonical
    // primary's next launch or, worse, would strip the canonical
    // primary's own uuid on the way out via DetachAutoSaveUuid).
    // The gate applies process-wide via an atomic in
    // session_history_manager.cpp; no per-window threading required.
    SessionHistoryManager::SetPublishingEnabled(!allowNewInstance);

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
    //
    // Capture the report so we can surface a status-bar hint on the
    // primary window when the per-launch cap was hit -- otherwise the
    // user has no in-app feedback that the sessions directory was
    // throttled and is being drained across multiple launches.
    const SessionHistoryManager::CleanupReport cleanupReport = historyManager.CleanupOrphanFiles();

    MainWindow w(&historyManager, nullptr);
    w.show();
    if (cleanupReport.capped)
    {
        // 8s mirrors the truncation hint in the forwarded-files slot
        // (Improvement #11) so the two transient notices feel uniform
        // to the user. After the message clears the status bar reverts
        // to whatever the normal load flow puts there.
        constexpr int CAPPED_MESSAGE_TIMEOUT_MS = 8000;
        w.statusBar()->showMessage(
            QObject::tr("Cleaned up %1 orphan session files; more will be removed on the next launch.")
                .arg(cleanupReport.deletedCount),
            CAPPED_MESSAGE_TIMEOUT_MS
        );
    }
    fileOpenFilter.setLiveWindow(&w);

    // Drain any `QFileOpenEvent`s that arrived during the
    // pre-window startup window (common on macOS when launching via
    // Finder double-click) into the primary's CLI queue. The events
    // were captured by `fileOpenFilter` while `mLiveWindow` was
    // null; we append them to the CLI files so a launch via
    // "Open With..." behaves identically to a launch with argv
    // file paths.
    const QStringList pendingFromOs = fileOpenFilter.takePending();
    if (!pendingFromOs.isEmpty())
    {
        cliFiles.append(pendingFromOs);
    }

    // Open any CLI-provided files in the primary's first window.
    // Append mode so a future `--config` switch can preload filters
    // before the open without them being clobbered.
    if (!cliFiles.isEmpty())
    {
        w.OpenFilesForCli(cliFiles);
    }

    // Peer windows tracked so we can deterministically close + reap
    // them before `historyManager` goes out of scope. Pre-fix the
    // restore-loop peers and forwarded peers relied on
    // `WA_DeleteOnClose` + Qt's deferred deletion to reach
    // `closeEvent` before the manager's destruction. That ordering
    // happened to work in practice but was a latent dependency: any
    // future refactor that pumps `aboutToQuit` after
    // `historyManager` is destroyed would surface as a use-after-free
    // in `MainWindow::closeEvent` (which deref's the manager pointer
    // to flush its final auto-save snapshot).
    QList<QPointer<MainWindow>> peers;

    // Append a peer to `peers` and compact any null `QPointer`s left
    // behind by previously-closed peers. Without the compaction the
    // list grew monotonically over the process lifetime -- one entry
    // per forwarded launch / restore-time peer, never reclaimed --
    // and the `aboutToQuit` iteration paid for an unbounded number
    // of null-check skips on long-running primaries. The explicit
    // erase-remove avoids depending on any specific Qt minor-version
    // helper (`QList::removeIf` / `std::erase_if<QList>` overloads
    // came in across several patch releases).
    auto appendPeer = [&peers](MainWindow *peer) {
        peers.erase(
            std::remove_if(peers.begin(), peers.end(), [](const QPointer<MainWindow> &p) { return p.isNull(); }),
            peers.end()
        );
        peers.append(QPointer<MainWindow>(peer));
    };

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

        // Cap the fan-restore to a bounded number of peers. A
        // pathological persisted state (lots of accumulated uuids
        // from prior crashes the wipe never reached) would otherwise
        // dominate the launch with cross-process lock waits.
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
            if (!primaryPath.isEmpty() && QFileInfo::exists(primaryPath))
            {
                w.RestoreLastSessionFromPath(primaryPath);
            }
            else
            {
                // Symmetric with the peer-loop cleanup below: the
                // primary uuid had no backing JSON either (evicted
                // by a sibling, removed by hand, never written due
                // to a crash mid-WriteSnapshot). Pre-fix this branch
                // silently skipped the entry, leaving the dangling
                // uuid in the Recent Sessions menu until the user
                // clicked it and got the "Recent Session Unavailable"
                // dialog. Drop it here so the next menu rebuild is
                // clean. `LooksLikeUuid` guard mirrors the peer
                // branch -- defensive in case a future caller hands
                // us a non-uuid-shaped string from QSettings.
                if (logapp::LooksLikeUuid(primaryUuid))
                {
                    historyManager.Remove(primaryUuid);
                }
            }
            for (const QString &uuid : previouslyOpen)
            {
                const QString peerPath = historyManager.PathForUuid(uuid);
                if (peerPath.isEmpty() || !QFileInfo::exists(peerPath))
                {
                    // Pre-fix this branch silently dropped the entry
                    // and left it lingering in the recents index --
                    // every subsequent launch would see the same
                    // dangling uuid in `OpenWindowsAtQuitUnlocked`. Removing
                    // it here both cleans the index and keeps the
                    // restore-loop cap meaningful.
                    if (logapp::LooksLikeUuid(uuid))
                    {
                        historyManager.Remove(uuid);
                    }
                    continue;
                }
                auto *peer = new MainWindow(&historyManager, nullptr);
                peer->setAttribute(Qt::WA_DeleteOnClose);
                peer->show();
                peer->RestoreLastSessionFromPath(peerPath);
                appendPeer(peer);
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
    //      cross-process lock acquisition. The N-window cost shrinks
    //      from `2N * WRITE_LOCK_TIMEOUT_RUNTIME_MS` worth of
    //      worst-case waits (the pre-fix loop did one
    //      `AddOpenWindowUuid` *and* one `WriteSnapshot` per window)
    //      to `N * WRITE_LOCK_TIMEOUT_RUNTIME_MS` (only the
    //      per-window saves) plus a single shutdown-budget
    //      `AddOpenWindowUuids` slot. The per-window saves are
    //      themselves bounded individually, so the wall-clock cost
    //      stays roughly linear in `N`; only the asymptote on the
    //      coordination side became O(1) -- not the whole handler.
    //
    // We *merge* rather than overwrite: with `--new-instance` two
    // processes can be alive simultaneously, and a destructive
    // `SetOpenWindowsAtQuit(restorable)` from the first quitter
    // would clobber the other process's published uuids.
    // `AddOpenWindowUuids` honours the same idempotent merge as the
    // per-uuid `AddOpenWindowUuid`.
    QObject::connect(&a, &QCoreApplication::aboutToQuit, &a, [&peers] {
        // Idempotency guard: `aboutToQuit` is documented to fire
        // exactly once per `QCoreApplication` lifetime, but a few
        // edge cases (test harnesses re-driving the event loop,
        // OS-level shutdown signals racing the GUI quit path) have
        // historically delivered it twice. A second pass would
        // double-flush every window's auto-save and re-publish the
        // open-windows set the post-`closeEvent` window has just
        // detached, leaving a phantom entry in the persisted
        // restore-on-launch list. `QAtomicInt::fetchAndOrAcquire`
        // races safely across threads (Qt may emit from either the
        // main or a signal-handler thread depending on the OS) and
        // costs one CAS in the no-op path.
        static QAtomicInt fired{0};
        if (fired.fetchAndOrAcquire(1) != 0)
        {
            return;
        }
        QStringList restorable;
        // Snapshot the top-level widget list before iteration: the
        // `AutoSaveSessionSnapshot` -> `closeEvent` chain can mutate
        // the list mid-iteration if a window decides to close
        // synchronously, and Qt's container iterators are not
        // tolerant of size changes under-foot. Using a `QList<QWidget*>`
        // captured upfront makes the iteration deterministic.
        const QList<QWidget *> topLevels = QApplication::topLevelWidgets();

        // Phase 1: gather restorable uuids from every live window.
        // The flush passes `publishOpenWindow=false` because we
        // batch the publish in phase 3 below; per-window publishes
        // here would pay N cross-process lock acquisitions for no
        // benefit (any uuid not yet in the persisted set will be
        // added in the single batched call).
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

        // Phase 2: tear peer windows down explicitly so their
        // `closeEvent` (which deref's `historyManager`) runs while
        // the manager is still alive. `WA_DeleteOnClose` schedules
        // a `deleteLater`, but `aboutToQuit` is the last event-loop
        // turn -- without this explicit close, peers would be
        // destroyed during static destruction *after* the manager
        // already unwound.
        //
        // Critical ordering: this MUST run *before* the batched
        // publish in phase 3, not after it. Each peer's `closeEvent`
        // calls `DetachAutoSaveUuid` -> `RemoveOpenWindowUuid` to
        // strip its pre-aboutToQuit publish (set by a prior per-save
        // `AutoSaveSessionSnapshot(publishOpenWindow=true)`) out of
        // the persisted set. Pre-fix this loop ran *after* a phase-3
        // publish, and every peer's `RemoveOpenWindowUuid` then
        // undid the batched publish for that peer's uuid. The
        // primary survived because it is stack-allocated and never
        // explicitly closed during `aboutToQuit`, so multi-window
        // restore-on-launch silently degraded to single-window
        // restore. Putting close first lets each peer self-detach
        // its stale publish, and the phase-3 batch then re-adds
        // every peer's uuid alongside the primary's in one
        // cross-process lock acquisition.
        for (const QPointer<MainWindow> &peer : peers)
        {
            if (peer.isNull())
            {
                continue;
            }
            peer->close();
        }

        // Phase 3: single batched publish for the primary + every
        // peer captured in phase 1. `AddOpenWindowUuids` is
        // idempotent so the primary's pre-aboutToQuit publish (if
        // any) is harmlessly re-asserted, and peers re-enter the
        // set fresh after their phase-2 self-detach. With the
        // `--new-instance` publishing gate disabled, this entire
        // call is a silent no-op (see
        // `SessionHistoryManager::SetPublishingEnabled`).
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
    // before `QApplication` is torn down. We additionally pin the
    // peer in `peers` so the shutdown `aboutToQuit` handler can
    // close it deterministically.
    QObject::connect(
        &instanceGuard,
        &SingleInstanceGuard::openWindowRequested,
        &a,
        [&historyManager, &appendPeer](const QStringList &files, int truncatedCount) {
            auto *child = new MainWindow(&historyManager, nullptr);
            child->setAttribute(Qt::WA_DeleteOnClose);
            child->show();
            child->raise();
            child->activateWindow();
            if (!files.isEmpty())
            {
                child->OpenFilesForCli(files);
            }
            // Surface the secondary's "I dropped N files at the wire
            // limit" hint on the spawned window. The status bar is
            // already on screen by the time we get here (the window
            // was `show()`-n above); 8 s is long enough for a
            // operator to notice and short enough to clear before
            // the next interaction.
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
