#pragma once

#include <loglib/log_configuration.hpp>

#include <QDateTime>
#include <QDir>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <memory>
#include <optional>

/// One entry in the Recent Sessions index. Lightweight metadata that
/// backs the File -> Recent Sessions submenu without forcing a JSON
/// load per entry. The full session JSON lives at
/// `sessionsDir/<uuid>.json` and is read on demand.
struct RecentSessionEntry
{
    QString uuid;            ///< Filename stem under `sessionsDir`.
    QString label;           ///< User-facing menu label.
    QString primaryLocator;  ///< First file path (tooltip + provenance).
    int fileCount = 0;       ///< Number of locators in the original session.
    qint64 timestampMsEpoch = 0;
};

/// Abstraction over the recents index storage. Pulled out so tests can
/// substitute an in-memory implementation without touching QSettings or
/// the user's profile (`QStandardPaths::setTestModeEnabled` alone is not
/// enough -- it changes paths but the singleton QSettings instance may
/// still collide across test cases).
///
/// Thread-safety contract: implementations MAY assume the caller has
/// taken `SessionHistoryManager::mMutex` for every `Read` / `Write` /
/// `ReadLastUuid` / `WriteLastUuid` call. The manager honours this on
/// every code path -- including the cheap pre-check inside `Touch` --
/// so implementations are free to keep their internal state non-thread-
/// safe (e.g. a bare `QList` member without its own lock).
class IRecentsIndexStorage
{
public:
    virtual ~IRecentsIndexStorage() = default;

    /// Implementations are strongly encouraged to be `noexcept` in
    /// effect (any internal failure should be logged + swallowed,
    /// never propagated). `WriteSnapshot` catches `std::exception`
    /// in a single envelope around every storage call so a throwing
    /// implementation does not corrupt the in-process state; even so,
    /// a `noexcept` implementation gives the manager the cleanest
    /// guarantee that "the lock is the only thing that can stop a
    /// write" -- not also "did the storage throw" -- which keeps
    /// the cross-process recovery story simple.
    virtual QList<RecentSessionEntry> Read() const = 0;
    virtual void Write(const QList<RecentSessionEntry> &entries) = 0;
    virtual std::optional<QString> ReadLastUuid() const = 0;
    virtual void WriteLastUuid(const std::optional<QString> &uuid) = 0;
};

/// Process-wide recent-sessions store. Owned by `main()` and passed by
/// reference into each `MainWindow`. Per-entry JSON files are written
/// under `sessionsDir/<uuid>.json` using the existing
/// `LogConfigurationManager::Save(scope=Full)` schema, so an
/// auto-saved session and a manually-saved one are interchangeable
/// and the manager does not need its own serialization layer.
///
/// Concurrency:
///
/// - The internal `mMutex` (one per instance) serialises every
///   read / write of the recents *index* and per-uuid JSON pool
///   (`List`, `LastSessionPath`, `WriteSnapshot`, `Touch`, `Remove`,
///   `Clear`, `CleanupOrphanFiles`).
/// - The static `OpenWindowsMutex` (file-local, one per process)
///   serialises every read / write of the `openWindowsAtQuit`
///   QSettings key (`OpenWindowsAtQuitUnlocked`,
///   `SetOpenWindowsAtQuit`, `AddOpenWindowUuid`,
///   `RemoveOpenWindowUuid`).
/// - The two mutexes are independent because the QSettings keys
///   they guard never alias. A caller never needs to hold both.
/// - A cross-process `QLockFile` at `sessionsDir/recents.lock`
///   is layered on top of every mutator (both families). The lock
///   is a strict gate: on acquisition timeout the mutator returns
///   without writing rather than racing the QSettings store. See
///   the `WRITE_LOCK_TIMEOUT_*` docstrings in the .cpp for the
///   timeout split between GUI-thread mutators (1.5 s) and
///   shutdown / user-initiated mutators (5 s).
///
/// Lock-ordering invariant (every mutator obeys this):
///
///    crossProc (QLockFile)  ->  mMutex / OpenWindowsMutex
///
/// `crossProc` is *always* the outermost lock. The per-process
/// mutexes are acquired strictly *after* a successful lock-file
/// `tryLock`. Pre-fix the `openWindowsAtQuit` mutators acquired
/// `OpenWindowsMutex` first and `crossProc` second, inverting the
/// order used by `WriteSnapshotAndPublish` (the only path that
/// holds both). The inversion was latent because production today
/// only invokes the manager from the GUI thread, but a worker-
/// thread caller would have stalled up to `WRITE_LOCK_TIMEOUT_*`
/// on every contended write. The unified ordering removes that
/// future footgun: no code path can produce an AB-BA cycle because
/// every site goes through `crossProc` before any in-process
/// mutex.
class SessionHistoryManager : public QObject
{
    Q_OBJECT
public:
    SessionHistoryManager(QDir sessionsDir, std::unique_ptr<IRecentsIndexStorage> indexStorage, QObject *parent = nullptr);
    ~SessionHistoryManager() override;

    // The manager owns shared cross-process state (the lock file
    // path, the QSettings index, the on-disk JSON pool); copying or
    // moving it would either share that state across the copies
    // (with no real synchronisation) or invalidate the pointer the
    // window holds. Both are footguns.
    Q_DISABLE_COPY_MOVE(SessionHistoryManager)

    /// Default cap on the number of entries kept in the index. The
    /// runtime cap is read from `QSettings` via `MaxEntries()` so a
    /// Preferences entry can override the default per profile;
    /// callers that need a compile-time bound (e.g. test fixtures
    /// generating a deterministic number of entries) use this
    /// constant directly. Older entries are evicted on `WriteSnapshot`
    /// / `Touch`.
    static constexpr int MAX_ENTRIES = 25;

    /// Hard lower / upper bounds for the user-configurable cap.
    /// `SetMaxEntries` clamps to this range so an out-of-range
    /// QSettings value (manually edited profile, future migration
    /// bug) cannot wedge the recents subsystem or trigger
    /// pathological eviction counts.
    static constexpr int MAX_ENTRIES_LOWER_BOUND = 1;
    static constexpr int MAX_ENTRIES_UPPER_BOUND = 200;

    /// Read the user-configurable recents-cap. Returns
    /// `MAX_ENTRIES` when the preference has never been set, and a
    /// value clamped into `[MAX_ENTRIES_LOWER_BOUND,
    /// MAX_ENTRIES_UPPER_BOUND]` otherwise. Cheap (one QSettings
    /// read); call sites can re-query on every mutation rather
    /// than caching.
    [[nodiscard]] static int MaxEntries();
    /// Persist the user-configurable recents-cap. Values outside
    /// `[MAX_ENTRIES_LOWER_BOUND, MAX_ENTRIES_UPPER_BOUND]` are
    /// clamped before writing.
    static void SetMaxEntries(int maxEntries);

    /// Newest-first list of recent sessions. Safe to call from the
    /// GUI thread between mutations; reads the index storage under
    /// the same in-process mutex used by writers, so the snapshot
    /// is consistent against same-process mutators.
    ///
    /// Cross-process consistency is best-effort: the read does
    /// *not* take the cross-process `QLockFile` (matches the
    /// rationale in `OpenWindowsAtQuitUnlocked`'s comment -- the GUI-thread
    /// menu rebuild on `aboutToShow` cannot afford the worst-case
    /// 1.5 s acquire stall a sibling writer would force, and a
    /// torn read just shows a slightly stale recents menu rather
    /// than corrupting state). The QSettings backend writes the
    /// `size` key after the per-entry sub-group, so a worst-case
    /// torn read is "size says N, only M < N entries actually
    /// readable"; the storage layer detects this and drops the
    /// torn slot rather than fabricating an empty entry.
    [[nodiscard]] QList<RecentSessionEntry> List() const;

    /// Persist @p configuration as a full session snapshot under
    /// `sessionsDir/<uuid>.json` and bump (or insert) the matching
    /// index entry. Returns the assigned uuid. If @p reuseUuid is
    /// non-empty and already maps to a current entry, the snapshot is
    /// rewritten in place (so a single MainWindow can amend its own
    /// session repeatedly without bloating the recents list).
    QString WriteSnapshot(const loglib::LogConfiguration &configuration, const QString &reuseUuid = QString());

    /// Persist @p configuration as `WriteSnapshot` would and -- if
    /// @p publishOpenWindow is `true` and the snapshot succeeded --
    /// publish the assigned uuid into the persisted
    /// `openWindowsAtQuit` set under the same cross-process lock
    /// acquisition. This is the recommended path for
    /// `AutoSaveSessionSnapshot`: the pre-fix code took two
    /// independent cross-process lock acquisitions back-to-back
    /// (one for the snapshot, one for the publish), doubling the
    /// worst-case GUI freeze under sibling contention and creating
    /// a small race window where a sibling could see the recents
    /// JSON updated but the openWindowsAtQuit set not yet
    /// reflecting it. Returns the assigned uuid (empty on failure)
    /// with the same semantics as `WriteSnapshot`.
    ///
    /// @p publishedOut (optional) receives whether the open-windows
    /// publish actually landed in the persisted set. `true` means
    /// the uuid is now in `openWindowsAtQuit` (either newly added or
    /// already present); `false` means either @p publishOpenWindow
    /// was false, the snapshot itself failed, the process-wide
    /// `IsPublishingEnabled()` gate is off (a `--new-instance`
    /// peer), or the snapshot succeeded but the publish was
    /// skipped. Callers that maintain a `mAutoSaveUuidPublished`
    /// latch should set it from @p publishedOut so the next save
    /// can retry on contention without leaving the in-process flag
    /// desynced from on-disk state. The default `nullptr` keeps
    /// existing callers (tests + the few legacy snapshot paths)
    /// allocation-free.
    QString WriteSnapshotAndPublish(
        const loglib::LogConfiguration &configuration,
        const QString &reuseUuid,
        bool publishOpenWindow,
        bool *publishedOut = nullptr
    );

    /// Move @p uuid to the top of the recents list and refresh its
    /// timestamp. No-op if @p uuid is not in the index. Returns
    /// `true` iff the index actually contained @p uuid **and** the
    /// cross-process lock was acquired so the bump landed on disk;
    /// returns `false` for either "not found" or "contended lock".
    ///
    /// Callers use the return value as the publish gate for
    /// `openWindowsAtQuit`: only ever publish a uuid we *both* own
    /// and have just successfully touched. Treating a contended
    /// lock as success (the pre-fix behaviour) lets two sibling
    /// processes publish the same uuid into the persisted set when
    /// neither has actually written the bump, so the next launch
    /// fan-restores a window the sibling may have never seen.
    [[nodiscard]] bool Touch(const QString &uuid);

    /// Remove the entry + its per-uuid JSON file. No-op if @p uuid is
    /// not in the index.
    void Remove(const QString &uuid);

    /// Drop every entry and delete every per-uuid JSON file under
    /// `sessionsDir`.
    void Clear();

    /// Path to the last-session JSON, if any. Used by the
    /// restore-on-launch flow in `main.cpp` as the *single-window*
    /// fallback when the multi-window `openWindowsAtQuit` set is
    /// empty. The backing `lastSessionUuid` is updated by every
    /// `WriteSnapshot`, so with multiple concurrent windows it drifts
    /// to whichever window auto-saved most recently; that is the
    /// intended semantics for "restore the most recent session" and
    /// is documented in the matching `WriteLastUuid` call site.
    [[nodiscard]] std::optional<QString> LastSessionPath() const;

    /// Per-uuid JSON path. Public so the Recent Sessions menu can
    /// reopen an entry through `MainWindow::DoLoadConfiguration`.
    ///
    /// Refuses non-uuid-shaped @p uuid (returns an empty string).
    /// Without the gate this is naive string concatenation and a
    /// malicious / corrupted QSettings value (e.g. `"../etc/passwd"`)
    /// would escape the sessions directory on every caller that
    /// composed the result into a `QFile::remove` / unlink.
    [[nodiscard]] QString PathForUuid(const QString &uuid) const;

    /// Sessions directory passed in at construction. Exposed for
    /// tests that want to inspect on-disk state and for callers
    /// that need to compose paths against the same root the
    /// manager uses internally.
    [[nodiscard]] QDir SessionsDir() const
    {
        return mSessionsDir;
    }

    /// Per-user default sessions directory under `AppDataLocation`.
    /// Shared by `main()` (constructs the production manager against
    /// this path) and by the static `AddOpenWindowUuid` /
    /// `RemoveOpenWindowUuid` / `SetOpenWindowsAtQuit` /
    /// `OpenWindowsAtQuitUnlocked` / `TakeOpenWindowsAtQuit` helpers
    /// (so the cross-process lock file lands in the same well-known
    /// location regardless of which process touches
    /// `openWindowsAtQuit`). The `CleanupOrphanFiles` instance method
    /// also reads the directory for its sweep, but it does so through
    /// the per-instance `mSessionsDir` rather than this static helper.
    [[nodiscard]] static QDir DefaultSessionsDir();

    /// Read the `restoreLastSessionOnLaunch` user preference. Default
    /// is `true` (opt-in to a smooth restart). Backed by `QSettings`;
    /// kept here so the preference lives in the same module as the
    /// other recents-related keys.
    static bool RestoreLastSessionOnLaunch();
    static void SetRestoreLastSessionOnLaunch(bool enabled);

    /// Read / write the `openWindowsAtQuit` list -- the uuids of
    /// the sessions that were active in *any* window the last time
    /// the application shut down. The primary uses this on launch
    /// to fan-restore every window that was open at quit, so a
    /// power loss or app crash does not lose the multi-window
    /// layout. Each uuid corresponds to a session JSON on disk in
    /// `SessionsDir()`.
    ///
    /// `OpenWindowsAtQuitUnlocked` deliberately skips the
    /// cross-process `QLockFile` (it still takes `OpenWindowsMutex`
    /// for the in-process read). The trade-off is documented in the
    /// implementation -- the read side never blocks on a sibling
    /// writer's queue, and the worst observable outcome is a torn
    /// list that gets corrected on the next mutation. The `Unlocked`
    /// suffix is the API's flag that callers must accept that
    /// trade-off; production code uses `TakeOpenWindowsAtQuit`
    /// (which takes the cross-process lock and atomically wipes
    /// after the read) instead.
    ///
    /// `SetOpenWindowsAtQuit` honours `SetPublishingEnabled(false)`
    /// (the `--new-instance` peer isolation gate) -- a peer that
    /// calls this with the gate disabled gets a silent no-op so it
    /// cannot wipe / overwrite the canonical primary's persisted
    /// restore set. The Add/Remove counterparts already honour the
    /// gate; this keeps the Set side symmetric.
    static QStringList OpenWindowsAtQuitUnlocked();
    static void SetOpenWindowsAtQuit(const QStringList &uuids);

    /// Atomic read-and-wipe of the `openWindowsAtQuit` list. Used by
    /// `main()` at launch so we never observe a torn list between
    /// "read what to restore" and "wipe so a mid-restore crash does
    /// not loop on the same uuids" -- a sibling `--new-instance`
    /// peer running concurrently could otherwise either (a) see the
    /// wipe and lose its own uuids, or (b) write between our read
    /// and wipe and have its addition silently dropped. Folding the
    /// two operations under a single lock acquisition closes the
    /// window without changing the existing crash-resilience
    /// semantics (the caller still re-adds restored uuids via
    /// `AddOpenWindowUuid` as windows come up).
    ///
    /// On lock-acquisition timeout this returns an empty list and
    /// performs no wipe -- "fail-closed empty" rather than "return
    /// what we managed to read". An earlier draft returned the read
    /// value (and only suppressed the wipe), but that let two
    /// sibling processes both see and act on the same persisted
    /// uuid set, double-restoring every window. Returning empty
    /// instead means the contended launch silently skips its
    /// fan-restore -- the user re-opens files via the recents menu
    /// -- which is strictly safer than two windows of the same
    /// session fighting over the same `<uuid>.json` and racing each
    /// other's `AddOpenWindowUuid` calls on quit. The non-contended
    /// (overwhelmingly common) path is unchanged: read + wipe under
    /// a single lock acquisition. Pinned by
    /// `TestRecentsTakeOpenWindowsAtQuitReturnsEmptyOnContention`.
    static QStringList TakeOpenWindowsAtQuit();

    /// Incrementally add @p uuid to the persisted open-windows list.
    /// Idempotent: re-adding an existing uuid is a no-op. Used by
    /// `MainWindow::AutoSaveSessionSnapshot` so the list reflects the
    /// currently-live sessions even when `aboutToQuit` runs after
    /// `WA_DeleteOnClose` has destroyed every peer window.
    ///
    /// Returns true when, post-call, @p uuid is guaranteed to be in
    /// the persisted set (either newly added by us or already present
    /// from a prior call / sibling process). Returns false when the
    /// publish did not land: cross-process lock contention timed out,
    /// publishing is disabled process-wide via
    /// `SetPublishingEnabled(false)` (the `--new-instance` isolation
    /// gate), or the uuid string is empty. Callers gate their
    /// "this window has been published" latch on the return value so
    /// the next AutoSave retries on contention without leaving the
    /// in-process flag desynced from the on-disk state.
    ///
    /// Concurrency: serialised through `OpenWindowsMutex` in the
    /// .cpp (multi-window same-process) *and* a `QLockFile` at
    /// `DefaultSessionsDir()/recents.lock` (cross-process, e.g. when
    /// the user opted out of single-instance via `--new-instance`).
    /// Fail-closed on lock acquisition timeout: a sibling writer
    /// gets to finish, and the next AutoSave (or the `aboutToQuit`
    /// safety-net) re-attempts to publish this window.
    [[nodiscard]] static bool AddOpenWindowUuid(const QString &uuid);

    /// Batched variant of `AddOpenWindowUuid`: append every entry in
    /// @p uuids that is not already in the persisted list, under a
    /// single cross-process lock acquisition. Used by `main()`'s
    /// `aboutToQuit` fan, which otherwise paid N cross-process lock
    /// round-trips for N restorable windows during OS-driven
    /// shutdown (Cmd+Q, login session teardown). The merge preserves
    /// the same "don't clobber `--new-instance` peers" invariant as
    /// the single-uuid path: any uuid already in the list -- whether
    /// published by us in a previous call or by a sibling process --
    /// is left in place. Empty strings inside @p uuids are skipped.
    /// Fail-closed on lock acquisition timeout, same rationale as
    /// `AddOpenWindowUuid`. Honours `SetPublishingEnabled(false)`
    /// the same way -- a `--new-instance` peer's `aboutToQuit` fan
    /// is a silent no-op.
    static void AddOpenWindowUuids(const QStringList &uuids);

    /// Companion to `AddOpenWindowUuid`. Removes @p uuid if present;
    /// no-op when absent or empty. Called from `MainWindow::closeEvent`
    /// and the destructive open / `NewSession` paths so a user-closed
    /// or user-discarded session is dropped from the next-launch
    /// restore set. Same cross-process locking story as
    /// `AddOpenWindowUuid`. Honours `SetPublishingEnabled(false)`
    /// for symmetry with the Add side: a `--new-instance` peer that
    /// briefly published into (or reopened an existing entry from)
    /// the canonical primary's persisted set must not be able to
    /// mutate that set on its way out either, because the canonical
    /// primary might still be alive and expecting its own uuid to
    /// survive the peer's lifecycle.
    static void RemoveOpenWindowUuid(const QString &uuid);

    /// Process-wide gate for the `openWindowsAtQuit` mutators. The
    /// canonical primary keeps the default `true`; a `--new-instance`
    /// peer calls `SetPublishingEnabled(false)` immediately after
    /// `SingleInstanceGuard::TryAcquire` so its sessions cannot
    /// alter the persisted restore-on-launch set the canonical
    /// primary owns. Affects `AddOpenWindowUuid`,
    /// `AddOpenWindowUuids`, and `RemoveOpenWindowUuid`; the
    /// startup-only `TakeOpenWindowsAtQuit` is unaffected because
    /// `--new-instance` peers do not call it (the restore-on-launch
    /// flow is gated separately in `main.cpp`).
    ///
    /// Thread-safety: backed by an atomic; safe to query / set from
    /// any thread. Production sets it exactly once in `main.cpp`.
    static void SetPublishingEnabled(bool enabled) noexcept;
    [[nodiscard]] static bool IsPublishingEnabled() noexcept;

    /// POD report from a single `CleanupOrphanFiles()` pass. The
    /// caller (`main.cpp`) inspects @ref capped to decide whether to
    /// surface a status-bar hint -- a capped sweep means the next
    /// launch will pick up the remainder, and the user benefits from
    /// knowing why the sessions directory hasn't fully drained yet.
    struct CleanupReport
    {
        /// Number of files actually deleted on this pass. Already
        /// excludes skipped files (non-uuid stems, ones that vanished
        /// between listing and unlink).
        int deletedCount = 0;
        /// `true` when the deletion loop terminated because it hit
        /// `CLEANUP_DELETIONS_PER_LAUNCH`; the remainder will be
        /// swept on the next startup. `false` when the loop ran to
        /// completion (the common case).
        bool capped = false;
    };

    /// One-shot housekeeping: remove every `<uuid>.json` under
    /// `sessionsDir` whose stem is not in the recents index. Called
    /// from `main()` at startup so crashes between
    /// `WriteSnapshot`'s file-write and index-update phases do not
    /// accumulate orphan files over time. Caller holds no lock; the
    /// method takes `mMutex` for the read-modify-delete cycle.
    ///
    /// Returns a `CleanupReport` so callers can surface a status-bar
    /// hint when the per-launch cap was hit. Lock-contention or
    /// missing-dir bail-outs return `{0, false}` (no work attempted,
    /// no cap hit). Pre-fix this returned `void` and the cap was
    /// only visible in the warning log; a user staring at a still-
    /// large sessions directory had no in-app feedback that the
    /// sweeper had been throttled.
    [[nodiscard]] CleanupReport CleanupOrphanFiles();

signals:
    /// Fired after a successful mutation (`WriteSnapshot`,
    /// `WriteSnapshotAndPublish`, `Touch`, `Remove`, `Clear`).
    ///
    /// Threading contract: always emitted from the thread that
    /// invoked the mutator -- the manager has no internal worker
    /// thread. In production every call site is on the GUI thread
    /// (auto-save, menu actions, restore-on-launch), so direct
    /// connections from a `MainWindow` slot run synchronously
    /// before the mutator returns. The mutator releases both the
    /// in-process mutex and the cross-process `QLockFile` *before*
    /// emitting, so a slot that recursively calls another mutator
    /// (e.g. `Clear` from inside a `changed()` handler) is safe
    /// against re-entrant deadlock.
    ///
    /// Tests that drive the manager from a worker thread are
    /// expected to use a `Qt::QueuedConnection` if the slot must
    /// run on the GUI thread; direct connections will run on the
    /// emitting thread.
    void changed();

private:
    /// Build the display label for @p configuration.
    [[nodiscard]] static QString BuildLabel(const loglib::LogConfiguration &configuration);

    /// Build the entry metadata for @p configuration (excluding uuid /
    /// timestamp which the caller fills in).
    [[nodiscard]] static RecentSessionEntry MakeEntryMetadata(const loglib::LogConfiguration &configuration);

    /// Tear down the per-uuid JSON file. Errors are swallowed -- a
    /// stale file does not block index mutation. Caller holds
    /// `mMutex`.
    void RemoveUuidFileLocked(const QString &uuid);

    /// Capacity-evict oldest entries until size <= MAX_ENTRIES. Returns
    /// the uuids of the entries that were dropped so the caller can
    /// unlink their backing JSON *after* the index `Write` lands. The
    /// reverse order (unlink first, then write) was unsafe: a crash
    /// between the unlink and the write left a dangling index entry
    /// pointing at a missing JSON, which `CleanupOrphanFiles` cannot
    /// repair (it sweeps unreferenced files, not unreferenced index
    /// entries). Caller holds `mMutex`.
    [[nodiscard]] QStringList EvictLocked(QList<RecentSessionEntry> &entries);

    QDir mSessionsDir;
    std::unique_ptr<IRecentsIndexStorage> mIndexStorage;
    mutable QMutex mMutex;
};

/// Default production storage backed by `QSettings`. Reads / writes
/// `recentSessions/*` keys under the existing organization /
/// application pair configured in `main.cpp`. Calls
/// `QSettings::sync()` at the tail of every write.
class QSettingsRecentsIndexStorage final : public IRecentsIndexStorage
{
public:
    QSettingsRecentsIndexStorage() = default;

    QList<RecentSessionEntry> Read() const override;
    void Write(const QList<RecentSessionEntry> &entries) override;
    std::optional<QString> ReadLastUuid() const override;
    void WriteLastUuid(const std::optional<QString> &uuid) override;
};
