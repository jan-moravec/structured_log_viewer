#pragma once

#include <loglib/log_configuration.hpp>

#include <QDateTime>
#include <QDir>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QStringList>

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
class IRecentsIndexStorage
{
public:
    virtual ~IRecentsIndexStorage() = default;

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
///   QSettings key (`OpenWindowsAtQuit`, `SetOpenWindowsAtQuit`,
///   `AddOpenWindowUuid`, `RemoveOpenWindowUuid`).
/// - The two mutexes are independent because the QSettings keys
///   they guard never alias. A caller never needs to hold both.
/// - A cross-process `QLockFile` at `sessionsDir/recents.lock`
///   is layered on top of every mutator (both families). The lock
///   is a strict gate: on acquisition timeout the mutator returns
///   without writing rather than racing the QSettings store. See
///   the `WRITE_LOCK_TIMEOUT_*` docstrings in the .cpp for the
///   timeout split between GUI-thread mutators (1.5 s) and
///   shutdown / user-initiated mutators (5 s).
class SessionHistoryManager : public QObject
{
    Q_OBJECT
public:
    SessionHistoryManager(QDir sessionsDir, std::unique_ptr<IRecentsIndexStorage> indexStorage, QObject *parent = nullptr);
    ~SessionHistoryManager() override;

    /// Cap on the number of entries kept in the index. Older entries
    /// are evicted on `WriteSnapshot` / `Touch`. Hard-coded for now;
    /// future Preferences entry can flow through here without an API
    /// change.
    static constexpr int MAX_ENTRIES = 10;

    /// Newest-first list of recent sessions. Safe to call from the
    /// GUI thread between mutations; reads the index storage under
    /// the same in-process mutex used by writers, so the snapshot
    /// is consistent against same-process mutators.
    ///
    /// Cross-process consistency is best-effort: the read does
    /// *not* take the cross-process `QLockFile` (matches the
    /// rationale in `OpenWindowsAtQuit`'s comment -- the GUI-thread
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

    /// Move @p uuid to the top of the recents list and refresh its
    /// timestamp. No-op if @p uuid is not in the index. Returns
    /// `true` when the index actually contained @p uuid and was
    /// reordered (whether or not the cross-process write succeeded
    /// -- a contended lock counts as "found but not bumped" because
    /// the caller's intent was correct), `false` when @p uuid was
    /// not in the index. Callers use the return value to decide
    /// whether to publish @p uuid into `openWindowsAtQuit` (we only
    /// want to publish uuids the manager actually owns, not arbitrary
    /// stems that happen to parse as UUIDs).
    bool Touch(const QString &uuid);

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
    /// `CleanupOrphanFiles` helpers (so the cross-process lock file
    /// lands in the same well-known location regardless of which
    /// process touches `openWindowsAtQuit`).
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
    static QStringList OpenWindowsAtQuit();
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
    /// On lock-acquisition timeout this returns the read value but
    /// performs no wipe -- losing the wipe on contention is the
    /// strictly safer half (we'll just retry the restore on the
    /// next launch) compared to losing the read and seeing an
    /// empty restore set.
    static QStringList TakeOpenWindowsAtQuit();

    /// Incrementally add @p uuid to the persisted open-windows list.
    /// Idempotent: re-adding an existing uuid is a no-op. Used by
    /// `MainWindow::AutoSaveSessionSnapshot` so the list reflects the
    /// currently-live sessions even when `aboutToQuit` runs after
    /// `WA_DeleteOnClose` has destroyed every peer window.
    ///
    /// Concurrency: serialised through `OpenWindowsMutex` in the
    /// .cpp (multi-window same-process) *and* a `QLockFile` at
    /// `DefaultSessionsDir()/recents.lock` (cross-process, e.g. when
    /// the user opted out of single-instance via `--new-instance`).
    /// Fail-closed on lock acquisition timeout: a sibling writer
    /// gets to finish, and the next AutoSave (or the `aboutToQuit`
    /// safety-net) re-attempts to publish this window.
    static void AddOpenWindowUuid(const QString &uuid);

    /// Companion to `AddOpenWindowUuid`. Removes @p uuid if present;
    /// no-op when absent or empty. Called from `MainWindow::closeEvent`
    /// and the destructive open / `NewSession` paths so a user-closed
    /// or user-discarded session is dropped from the next-launch
    /// restore set. Same cross-process locking story as
    /// `AddOpenWindowUuid`.
    static void RemoveOpenWindowUuid(const QString &uuid);

    /// One-shot housekeeping: remove every `<uuid>.json` under
    /// `sessionsDir` whose stem is not in the recents index. Called
    /// from `main()` at startup so crashes between
    /// `WriteSnapshot`'s file-write and index-update phases do not
    /// accumulate orphan files over time. Caller holds no lock; the
    /// method takes `mMutex` for the read-modify-delete cycle.
    void CleanupOrphanFiles();

signals:
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
