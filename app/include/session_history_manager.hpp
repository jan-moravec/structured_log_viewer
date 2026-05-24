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

/// One entry in the Recent Sessions index. The full session JSON
/// lives at `sessionsDir/<uuid>.json` and is read on demand; this
/// metadata backs the submenu without forcing a JSON load per entry.
struct RecentSessionEntry
{
    QString uuid;           ///< Filename stem under `sessionsDir`.
    QString label;          ///< User-facing menu label.
    QString primaryLocator; ///< First file path (tooltip + provenance).
    int fileCount = 0;      ///< Number of locators in the original session.
    qint64 timestampMsEpoch = 0;
};

/// Abstraction over the recents index storage. Tests substitute an
/// in-memory implementation; production uses `QSettings`.
///
/// Thread-safety: implementations may assume the caller has taken
/// `SessionHistoryManager::mMutex` for every call. The manager
/// honours this on every code path.
class IRecentsIndexStorage
{
public:
    virtual ~IRecentsIndexStorage() = default;

    virtual QList<RecentSessionEntry> Read() const = 0;
    virtual void Write(const QList<RecentSessionEntry> &entries) = 0;
    virtual std::optional<QString> ReadLastUuid() const = 0;
    virtual void WriteLastUuid(const std::optional<QString> &uuid) = 0;
};

/// Process-wide recent-sessions store. Owned by `main()` and passed
/// by reference into each `MainWindow`. Per-session JSON files are
/// written via `LogConfigurationManager::Save(scope=Full)` so an
/// auto-saved session and a manually-saved one are interchangeable.
///
/// Concurrency:
///
/// - `mMutex` serialises every read / write of the recents index
///   and per-uuid JSON pool.
/// - A file-local `OpenWindowsMutex` serialises every read / write
///   of the `openWindowsAtQuit` QSettings key. Independent of
///   `mMutex` (the keys never alias); a caller never needs both.
/// - A cross-process `QLockFile` at `sessionsDir/recents.lock` is
///   layered on top of every mutator. On acquisition timeout the
///   mutator fails closed (returns without writing) rather than
///   racing the QSettings store.
///
/// Lock ordering: cross-process `QLockFile` is always outermost,
/// acquired before any per-process mutex. Every call site follows
/// this rule so no AB-BA cycle is possible.
class SessionHistoryManager : public QObject
{
    Q_OBJECT
public:
    SessionHistoryManager(
        const QDir &sessionsDir, std::unique_ptr<IRecentsIndexStorage> indexStorage, QObject *parent = nullptr
    );
    ~SessionHistoryManager() override;

    Q_DISABLE_COPY_MOVE(SessionHistoryManager)

    /// Default cap on the number of entries kept in the index.
    /// `MaxEntries()` reads the runtime cap from `QSettings` so a
    /// Preferences entry can override per profile.
    static constexpr int MAX_ENTRIES = 25;

    /// Hard bounds for the user-configurable cap.
    static constexpr int MAX_ENTRIES_LOWER_BOUND = 1;
    static constexpr int MAX_ENTRIES_UPPER_BOUND = 200;

    /// Read the user-configurable recents cap. Returns `MAX_ENTRIES`
    /// when unset; otherwise the persisted value clamped into
    /// `[MAX_ENTRIES_LOWER_BOUND, MAX_ENTRIES_UPPER_BOUND]`.
    [[nodiscard]] static int MaxEntries();
    /// Persist the recents cap. Clamped to the supported range.
    static void SetMaxEntries(int maxEntries);

    /// Newest-first list of recent sessions. Safe to call between
    /// mutations. Skips the cross-process lock so the read never
    /// blocks on a sibling writer; a torn read just shows a slightly
    /// stale menu rather than corrupting state.
    [[nodiscard]] QList<RecentSessionEntry> List() const;

    /// Persist @p configuration under `sessionsDir/<uuid>.json` and
    /// bump (or insert) the matching index entry. Returns the
    /// assigned uuid (empty on failure). If @p reuseUuid maps to an
    /// existing entry, the snapshot is rewritten in place so a
    /// MainWindow can amend its session without bloating the list.
    QString WriteSnapshot(const loglib::LogConfiguration &configuration, const QString &reuseUuid = QString());

    /// Persist @p configuration and -- when @p publishOpenWindow is
    /// true -- publish the assigned uuid into `openWindowsAtQuit`
    /// under the same cross-process lock acquisition. Halves the
    /// worst-case GUI freeze under contention vs. two back-to-back
    /// acquisitions and closes the race where a sibling could see
    /// the recents JSON updated but not the open-windows set.
    /// Returns the uuid (empty on failure).
    ///
    /// @p publishedOut (optional) reports whether the publish
    /// actually landed (true == uuid is in `openWindowsAtQuit`).
    /// Callers gate their `mAutoSaveUuidPublished` latch on this so
    /// contention does not desync the in-process flag.
    QString WriteSnapshotAndPublish(
        const loglib::LogConfiguration &configuration,
        const QString &reuseUuid,
        bool publishOpenWindow,
        bool *publishedOut = nullptr
    );

    /// Move @p uuid to the top of the list and refresh its
    /// timestamp. Returns `true` iff the index contained @p uuid
    /// *and* the cross-process lock was acquired (i.e. the bump
    /// landed on disk). A contended lock returns `false`.
    ///
    /// Callers use the return value as the publish gate for
    /// `openWindowsAtQuit`: only publish a uuid we own and have
    /// successfully touched.
    [[nodiscard]] bool Touch(const QString &uuid);

    /// Remove the entry + its per-uuid JSON file. No-op when absent.
    void Remove(const QString &uuid);

    /// Drop every entry and unlink every per-uuid JSON.
    void Clear();

    /// Path to the last-session JSON, if any. Used by the
    /// restore-on-launch flow as the single-window fallback when
    /// `openWindowsAtQuit` is empty. Tracks whichever window
    /// auto-saved most recently.
    [[nodiscard]] std::optional<QString> LastSessionPath() const;

    /// Per-uuid JSON path. Public so the Recent Sessions menu can
    /// reopen an entry via `MainWindow::DoLoadConfiguration`.
    /// Returns empty for a non-uuid-shaped @p uuid -- guards naive
    /// concatenation against a hand-edited / corrupt profile value
    /// that would otherwise escape `sessionsDir`.
    [[nodiscard]] QString PathForUuid(const QString &uuid) const;

    /// Sessions directory passed in at construction.
    [[nodiscard]] QDir SessionsDir() const
    {
        return mSessionsDir;
    }

    /// Per-user default sessions directory under `AppDataLocation`.
    /// Used by `main()` and by the static `openWindowsAtQuit`
    /// helpers so the cross-process lock lives in a well-known
    /// location regardless of which process touches the key.
    [[nodiscard]] static QDir DefaultSessionsDir();

    /// Read / write the `restoreLastSessionOnLaunch` preference.
    /// Defaults to `true`.
    static bool RestoreLastSessionOnLaunch();
    static void SetRestoreLastSessionOnLaunch(bool enabled);

    /// Read / write the `openWindowsAtQuit` list -- the uuids of
    /// sessions active in any window the last time the application
    /// shut down. The primary uses this on launch to fan-restore
    /// every window that was open at quit.
    ///
    /// `OpenWindowsAtQuitUnlocked` skips the cross-process lock
    /// (the read still takes `OpenWindowsMutex`); the `Unlocked`
    /// suffix flags that callers must accept a possibly torn read
    /// in exchange for never blocking on a sibling writer. The
    /// torn slot self-corrects on the next mutation.
    /// `SetOpenWindowsAtQuit` honours `SetPublishingEnabled(false)`
    /// so a `--new-instance` peer cannot clobber the canonical
    /// primary's persisted set.
    static QStringList OpenWindowsAtQuitUnlocked();
    static void SetOpenWindowsAtQuit(const QStringList &uuids);

    /// Atomic read-and-wipe of the `openWindowsAtQuit` list. Used
    /// by `main()` at launch so we never observe a torn list
    /// between "read what to restore" and "wipe so a mid-restore
    /// crash does not loop". Fails closed (returns empty, no
    /// wipe) on lock-acquisition timeout -- two contended siblings
    /// must not both restore the same uuid set.
    static QStringList TakeOpenWindowsAtQuit();

    /// Idempotently add @p uuid to the persisted open-windows list.
    /// Used by `AutoSaveSessionSnapshot` so the list reflects
    /// currently-live sessions even when `aboutToQuit` runs after
    /// `WA_DeleteOnClose` has destroyed every peer window.
    ///
    /// Returns true when @p uuid is in the persisted set after the
    /// call (either newly added or already present). Returns false
    /// on lock contention, when publishing is disabled
    /// (`--new-instance` peer), or on empty input. Callers latch
    /// `mAutoSaveUuidPublished` on the return value so contention
    /// does not desync the in-process flag.
    [[nodiscard]] static bool AddOpenWindowUuid(const QString &uuid);

    /// Batched variant. Appends every @p uuids entry not already
    /// present under one lock acquisition. Used by `main()`'s
    /// `aboutToQuit` fan to avoid paying N round-trips during OS-
    /// driven shutdown. Empty strings are skipped. Fails closed on
    /// contention and honours `SetPublishingEnabled(false)`.
    static void AddOpenWindowUuids(const QStringList &uuids);

    /// Companion to `AddOpenWindowUuid`. Called from `closeEvent`
    /// and the destructive open / `NewSession` paths so a closed
    /// or discarded session is dropped from the next-launch restore
    /// set. Honours `SetPublishingEnabled(false)` symmetrically so
    /// a `--new-instance` peer cannot mutate the canonical
    /// primary's persisted set on its way out.
    static void RemoveOpenWindowUuid(const QString &uuid);

    /// Process-wide gate for the `openWindowsAtQuit` mutators. The
    /// canonical primary leaves it `true`; a `--new-instance` peer
    /// calls `SetPublishingEnabled(false)` so its sessions cannot
    /// alter the persisted restore set the canonical primary owns.
    /// `TakeOpenWindowsAtQuit` is unaffected (peers do not call it).
    /// Thread-safe; production sets it exactly once in `main.cpp`.
    static void SetPublishingEnabled(bool enabled) noexcept;
    [[nodiscard]] static bool IsPublishingEnabled() noexcept;

    /// Report from `CleanupOrphanFiles`. `capped` is true when the
    /// per-launch cap was hit -- `main()` surfaces a status-bar
    /// hint so the user knows the rest will be swept next launch.
    struct CleanupReport
    {
        int deletedCount = 0;
        bool capped = false;
    };

    /// Remove every `<uuid>.json` under `sessionsDir` whose stem is
    /// not in the recents index. Called from `main()` at startup to
    /// reap files written by `WriteSnapshot` before its index update
    /// (i.e. a crash between the two phases). Lock-contention or a
    /// missing dir return `{0, false}` (no work attempted).
    [[nodiscard]] CleanupReport CleanupOrphanFiles();

signals:
    /// Fired after a successful mutation. Emitted on the thread
    /// that invoked the mutator (the manager has no worker thread)
    /// *after* both the in-process mutex and the cross-process
    /// `QLockFile` have been released, so a slot may safely call
    /// another mutator without re-entrant deadlock.
    void changed();

private:
    [[nodiscard]] static QString BuildLabel(const loglib::LogConfiguration &configuration);

    /// Build the entry metadata for @p configuration (excluding uuid
    /// and timestamp, which the caller fills in).
    [[nodiscard]] static RecentSessionEntry MakeEntryMetadata(const loglib::LogConfiguration &configuration);

    /// Tear down the per-uuid JSON file. Errors are swallowed. Caller
    /// holds `mMutex`.
    void RemoveUuidFileLocked(const QString &uuid) const;

    /// Capacity-evict oldest entries until size <= MaxEntries().
    /// Returns the evicted uuids so the caller can unlink their
    /// backing JSON *after* the index `Write` lands -- the reverse
    /// order would leave a dangling index entry on a crash between
    /// the two. Caller holds `mMutex`.
    [[nodiscard]] QStringList EvictLocked(QList<RecentSessionEntry> &entries);

    QDir mSessionsDir;
    std::unique_ptr<IRecentsIndexStorage> mIndexStorage;
    mutable QMutex mMutex;
};

/// Default production storage backed by `QSettings`.
class QSettingsRecentsIndexStorage final : public IRecentsIndexStorage
{
public:
    QSettingsRecentsIndexStorage() = default;

    QList<RecentSessionEntry> Read() const override;
    void Write(const QList<RecentSessionEntry> &entries) override;
    std::optional<QString> ReadLastUuid() const override;
    void WriteLastUuid(const std::optional<QString> &uuid) override;
};
