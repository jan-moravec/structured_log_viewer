#include "session_history_manager.hpp"

#include "log_warning.hpp"
#include "uuid_utils.hpp"

#include <QFileInfo>
#include <QLatin1String>
#include <QLockFile>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QUuid>
#include <QVariant>

#include <algorithm>
#include <atomic>
#include <exception>
#include <memory>
#include <utility>

namespace
{
constexpr char SETTINGS_SIZE_KEY[] = "recentSessions/size";
constexpr char SETTINGS_ENTRIES_GROUP[] = "recentSessions/entries";
constexpr char SETTINGS_LAST_UUID_KEY[] = "recentSessions/lastSessionUuid";
constexpr char SETTINGS_RESTORE_LAST_KEY[] = "recentSessions/restoreLastSessionOnLaunch";
constexpr char SETTINGS_OPEN_WINDOWS_KEY[] = "recentSessions/openWindowsAtQuit";
constexpr char SETTINGS_VERSION_KEY[] = "recentSessions/version";
constexpr char SETTINGS_MAX_ENTRIES_KEY[] = "recentSessions/maxEntries";

/// On-disk layout version. Bump when the recents index schema
/// changes in a way that needs migration code. Today we have one
/// version; the key exists so a future build can inspect the value
/// at startup and decide whether to migrate.
constexpr int CURRENT_RECENTS_VERSION = 1;

/// Belt-and-braces upper bound on the entry count we will trust from
/// QSettings. A corrupted profile (or an attacker who can scribble
/// `recentSessions/size`) should not be able to convince us to
/// allocate gigabytes of `RecentSessionEntry`. `MAX_ENTRIES * 4`
/// is generous (we still drop anything past `MAX_ENTRIES` via
/// `EvictLocked`) while keeping the allocation O(KB).
constexpr int CORRUPT_PROFILE_SIZE_CAP_MULTIPLIER = 4;

/// Hard cap on the number of orphan deletions per `CleanupOrphanFiles`
/// call. A pathological sessions directory (hundreds of orphans from
/// a long-running misconfigured peer) should not be allowed to wedge
/// startup on a slow filesystem; we delete what we can and the rest
/// gets swept up over subsequent launches.
constexpr int CLEANUP_DELETIONS_PER_LAUNCH = 200;

/// Once-per-process warning suppression for the "could not acquire
/// the cross-process recents lock" diagnostic. The lock is queried
/// from many call sites (every AutoSave, every menu open, every
/// `aboutToQuit`); reporting each timeout would drown a real fault
/// in noise. We log on the first failure of the process lifetime
/// and stay silent afterwards -- enough for support triage to see
/// "this user is hitting lock contention" without flooding the
/// log.
std::atomic<bool> g_recents_lock_warned{false}; // NOLINT(readability-identifier-naming)

/// Log a once-per-process diagnostic when a `QSettings::sync()` call
/// reports an error. `AccessError` covers "could not write to the
/// profile file at all" (read-only volume, ENOSPC, ...) and
/// `FormatError` indicates "the on-disk format is malformed and
/// QSettings could not reconcile". Both are conditions the caller
/// cannot recover from but support triage needs to know about; the
/// flag-and-skip pattern matches the lock-contention warning above.
std::atomic<bool> g_settings_status_warned{false}; // NOLINT(readability-identifier-naming)
void SyncWarnIfNeeded(QSettings &settings, const char *context)
{
    const auto status = settings.status();
    if (status == QSettings::NoError)
    {
        return;
    }
    if (g_settings_status_warned.exchange(true))
    {
        return;
    }
    LOGAPP_WARN() << "QSettings::sync() returned" << static_cast<int>(status) << "after" << context
                  << "; subsequent failures will be silent this process.";
}

/// Cross-process lock timeouts.
///
/// `WRITE_LOCK_TIMEOUT_RUNTIME_MS` covers the in-session mutators
/// (`WriteSnapshot`, `Touch`, `AddOpenWindowUuid`,
/// `RemoveOpenWindowUuid`, `CleanupOrphanFiles`). These run on the
/// GUI thread (via `streamingFinished` AutoSave / menu actions), so
/// the timeout doubles as the worst-case GUI freeze under
/// cross-process contention. 1.5 s comfortably covers a sibling's
/// `Read` + `Write` cycle on a typical filesystem while keeping the
/// freeze well under the 2 s "user notices the UI is wedged"
/// threshold.
///
/// `WRITE_LOCK_TIMEOUT_SHUTDOWN_MS` is reserved for the user-
/// initiated / shutdown mutators (`Remove`, `Clear`,
/// `SetOpenWindowsAtQuit`). Those are not on a hot interactive path
/// and the alternative (a lost write across a launch / quit
/// boundary) is more disruptive than a longer wait. 5 s is enough
/// to outlast any reasonable sibling save without making the user
/// wait visibly.
///
/// Policy on timeout: callers now *fail closed*. If `AcquireRecentsLock`
/// returns an unlocked guard, mutators return without writing rather
/// than racing the QSettings store (where `QSettingsRecentsIndexStorage::Write`
/// clears and rewrites the entries sub-tree, which interleaved with a
/// sibling writer would silently corrupt the index). Worst case is a
/// dropped recents entry; the previously-persisted state is left
/// intact.
constexpr int WRITE_LOCK_TIMEOUT_RUNTIME_MS = 1500;
constexpr int WRITE_LOCK_TIMEOUT_SHUTDOWN_MS = 5000;

QString EntryKey(int index, const QString &field)
{
    return QStringLiteral("%1/%2/%3").arg(QLatin1String(SETTINGS_ENTRIES_GROUP)).arg(index).arg(field);
}

/// Tries to acquire the cross-process recents lock. Returns a guard
/// whose `locked` field tells the caller whether the lock was
/// actually obtained; the destructor unlocks on scope exit when
/// `locked` is true.
///
/// Callers must check `guard.locked` and bail (no write) when it is
/// false: the cross-process lock is a strict gate, not a best-effort
/// hint -- silently proceeding on timeout would race
/// `QSettingsRecentsIndexStorage::Write`'s clear+rewrite of the
/// entries sub-group with a sibling writer and corrupt the index.
///
/// Side effect: this helper materialises @p sessionsDir via
/// `mkpath` so the lock file can be created in place. The directory
/// is otherwise created lazily on first `WriteSnapshot`.
///
/// Callers pass either `WRITE_LOCK_TIMEOUT_RUNTIME_MS` (GUI-thread
/// mutators) or `WRITE_LOCK_TIMEOUT_SHUTDOWN_MS` (user-initiated /
/// shutdown). See the constant docstrings for the rationale.
struct LockFileGuard
{
    // Invariant: `locked == true` implies `lock != nullptr`. The only
    // way to flip `locked` to `true` goes through `AcquireRecentsLock`
    // below, which constructs the `unique_ptr` before calling
    // `tryLock`. Move-from clears the source's `locked` so the
    // invariant survives transfers. Destructor + move-assign therefore
    // only need to test `locked`.
    std::unique_ptr<QLockFile> lock;
    bool locked = false;

    LockFileGuard() = default;
    LockFileGuard(const LockFileGuard &) = delete;
    LockFileGuard &operator=(const LockFileGuard &) = delete;
    LockFileGuard(LockFileGuard &&other) noexcept : lock(std::move(other.lock)), locked(other.locked)
    {
        other.locked = false;
    }
    LockFileGuard &operator=(LockFileGuard &&other) noexcept
    {
        if (this != &other)
        {
            // Order matters: unlock our previously-held lock
            // BEFORE moving the new `unique_ptr` into `lock`.
            // The `unique_ptr` reset that happens implicitly on
            // assignment would otherwise destroy our old `QLockFile`
            // *without* unlocking it -- the destructor would still
            // run, but the cross-process `tryLock` recovery path
            // relies on an explicit `unlock()` so a sibling process
            // sees the lock release immediately rather than waiting
            // for the OS to reap the underlying file handle. The
            // self-assignment guard above is required because
            // `lock = std::move(other.lock)` would clear both
            // pointers if `this == &other`, leaving the
            // never-unlocked file in place.
            if (locked)
            {
                lock->unlock();
            }
            lock = std::move(other.lock);
            locked = other.locked;
            other.locked = false;
        }
        return *this;
    }
    ~LockFileGuard()
    {
        if (locked)
        {
            lock->unlock();
        }
    }
};

/// Read the persisted `openWindowsAtQuit` list. Pulled out so the
/// public static accessor and the read-modify-write helpers below
/// share one implementation and one set of QSettings semantics.
QStringList ReadOpenWindowsAtQuit()
{
    QSettings settings;
    return settings.value(QLatin1String(SETTINGS_OPEN_WINDOWS_KEY)).toStringList();
}

void WriteOpenWindowsAtQuit(const QStringList &uuids)
{
    QSettings settings;
    if (uuids.isEmpty())
    {
        // Remove the key entirely so the next launch's
        // `OpenWindowsAtQuit()` returns an empty list without us
        // having to special-case it.
        settings.remove(QLatin1String(SETTINGS_OPEN_WINDOWS_KEY));
    }
    else
    {
        settings.setValue(QLatin1String(SETTINGS_OPEN_WINDOWS_KEY), uuids);
    }
    settings.sync();
    SyncWarnIfNeeded(settings, "WriteOpenWindowsAtQuit");
}

/// Serialises read-modify-write of the `openWindowsAtQuit` list across
/// every window in this process. QSettings itself is reentrant but not
/// thread-safe; this guard prevents two windows from racing to set the
/// same key and clobbering each other's update. The cross-process
/// `QLockFile` layered on top in `Add` / `Remove` / `Set` handles
/// inter-process races (e.g. `--new-instance` launches).
QMutex &OpenWindowsMutex()
{
    static QMutex mutex;
    return mutex;
}

LockFileGuard AcquireRecentsLock(const QDir &sessionsDir, int timeoutMs)
{
    LockFileGuard guard;
    // The lock file lives next to the per-uuid JSONs; the directory
    // must exist before `tryLock` will succeed. `QDir::mkpath` is
    // non-const, so we make a one-off mutable copy here -- the caller
    // is unaffected because we only need the path string from it.
    // mkpath is idempotent + safe to call concurrently.
    //
    // The mkpath side-effect is acceptable because every caller is a
    // write-class operation (`WriteSnapshot`, `Touch`, `Remove`,
    // `Clear`, the `*OpenWindowUuid` writers, `CleanupOrphanFiles`)
    // that genuinely needs the sessions directory to exist before
    // any subsequent file I/O. The pure-read path
    // (`OpenWindowsAtQuit -> ReadOpenWindowsAtQuit`) deliberately
    // skips `AcquireRecentsLock` entirely so it never pays the
    // mkpath cost on a process that has not yet auto-saved a
    // session -- see `OpenWindowsAtQuit`'s comment for the
    // torn-read trade-off that justifies the unlocked read.
    if (!sessionsDir.exists() && !QDir(sessionsDir).mkpath(QStringLiteral(".")))
    {
        // Filesystem refusal (read-only volume, ENOSPC, ...).
        // Returning the empty guard signals "no lock acquired" --
        // the caller will detect `locked == false` and bail.
        return guard;
    }
    guard.lock = std::make_unique<QLockFile>(sessionsDir.filePath(QStringLiteral("recents.lock")));
    // `setStaleLockTime(0)` disables Qt's "this PID died, take the
    // lock anyway" recovery; a crashed primary then leaves the
    // recents subsystem wedged for every subsequent launch. 30 s
    // beats both `WRITE_LOCK_TIMEOUT_*` and any plausible legitimate
    // hold-time, so contention with a live holder still fails
    // closed via the timeout, but a dead-holder lock is recovered
    // within a minute instead of never. `tryLock` itself still
    // honours the explicit timeout argument below; this only
    // affects the staleness threshold.
    constexpr int STALE_LOCK_TIMEOUT_MS = 30 * 1000;
    guard.lock->setStaleLockTime(STALE_LOCK_TIMEOUT_MS);
    guard.locked = guard.lock->tryLock(timeoutMs);
    if (!guard.locked && !g_recents_lock_warned.exchange(true))
    {
        // First-of-launch diagnostic; subsequent failures are silent
        // to keep the log signal-rich. The QLockFile error code
        // distinguishes "another process holds it" from "filesystem
        // refusal" -- include it so support triage can tell the
        // difference.
        LOGAPP_WARN() << "SessionHistoryManager: failed to acquire recents lock after" << timeoutMs
                      << "ms (error code:" << static_cast<int>(guard.lock->error())
                      << "); subsequent timeouts will be silent this process.";
    }
    return guard;
}
} // namespace

QDir SessionHistoryManager::DefaultSessionsDir()
{
    // Single source of truth for the recents directory. `main()`
    // and the static `openWindowsAtQuit` helpers below all call
    // this so the cross-process lock file lands in the same path
    // regardless of who computes it.
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (base.isEmpty())
    {
        // `AppDataLocation` is empty on a few exotic platforms /
        // portable-mode setups; fall back to the user temp dir so we
        // still have a writeable scratch space rather than crashing
        // out of the recents subsystem.
        base = QDir::tempPath();
    }
    return QDir(base).filePath(QStringLiteral("sessions"));
}

bool SessionHistoryManager::RestoreLastSessionOnLaunch()
{
    QSettings settings;
    return settings.value(QLatin1String(SETTINGS_RESTORE_LAST_KEY), true).toBool();
}

void SessionHistoryManager::SetRestoreLastSessionOnLaunch(bool enabled)
{
    QSettings settings;
    settings.setValue(QLatin1String(SETTINGS_RESTORE_LAST_KEY), enabled);
    settings.sync();
    SyncWarnIfNeeded(settings, "SetRestoreLastSessionOnLaunch");
}

int SessionHistoryManager::MaxEntries()
{
    QSettings settings;
    const QVariant raw = settings.value(QLatin1String(SETTINGS_MAX_ENTRIES_KEY));
    if (!raw.isValid())
    {
        return MAX_ENTRIES;
    }
    bool ok = false;
    int value = raw.toInt(&ok);
    if (!ok)
    {
        return MAX_ENTRIES;
    }
    return std::clamp(value, MAX_ENTRIES_LOWER_BOUND, MAX_ENTRIES_UPPER_BOUND);
}

void SessionHistoryManager::SetMaxEntries(int maxEntries)
{
    QSettings settings;
    const int clamped = std::clamp(maxEntries, MAX_ENTRIES_LOWER_BOUND, MAX_ENTRIES_UPPER_BOUND);
    settings.setValue(QLatin1String(SETTINGS_MAX_ENTRIES_KEY), clamped);
    settings.sync();
    SyncWarnIfNeeded(settings, "SetMaxEntries");
}

SessionHistoryManager::SessionHistoryManager(
    QDir sessionsDir, std::unique_ptr<IRecentsIndexStorage> indexStorage, QObject *parent
)
    : QObject(parent), mSessionsDir(std::move(sessionsDir)), mIndexStorage(std::move(indexStorage))
{
    Q_ASSERT(mIndexStorage != nullptr);
    // Create the directory lazily on first write; nothing to do here.
}

SessionHistoryManager::~SessionHistoryManager() = default;

QList<RecentSessionEntry> SessionHistoryManager::List() const
{
    QMutexLocker lock(&mMutex);
    return mIndexStorage->Read();
}

QString SessionHistoryManager::WriteSnapshot(const loglib::LogConfiguration &configuration, const QString &reuseUuid)
{
    return WriteSnapshotAndPublish(configuration, reuseUuid, /*publishOpenWindow=*/false);
}

QString SessionHistoryManager::WriteSnapshotAndPublish(
    const loglib::LogConfiguration &configuration, const QString &reuseUuid, bool publishOpenWindow
)
{
    QString uuid = reuseUuid;
    if (uuid.isEmpty())
    {
        // Strip the curly braces so the uuid is filesystem-friendly
        // (Windows handles them fine but a clean stem makes ad-hoc
        // inspection easier).
        uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    // Belt-and-braces: reject a caller-supplied non-uuid `reuseUuid`
    // before it lands in `PathForUuid`. The pre-fix path would
    // happily build `sessionsDir/<garbage>.json` and write the
    // session there, leaking outside the managed pool.
    else if (!logapp::LooksLikeUuid(uuid))
    {
        LOGAPP_WARN() << "WriteSnapshot rejecting non-uuid reuseUuid:" << uuid;
        return QString();
    }

    // Scope the locks so both the cross-process `QLockFile` and
    // `mMutex` are released before we `emit changed()`. A future
    // direct connection on `changed` that triggers another mutator
    // on the same thread would otherwise deadlock on `QLockFile`
    // (it is not reentrant within a process).
    bool changedFired = false;
    {
        // Lock-ordering rationale: acquire the cross-process
        // `QLockFile` *before* `mMutex`. The lock-file acquisition
        // can block for up to `WRITE_LOCK_TIMEOUT_RUNTIME_MS`
        // (1.5 s); holding `mMutex` across that wait would freeze
        // every same-process `List()` reader (and every other
        // mutator) for the same window. `mMutex`-after-`QLockFile`
        // means readers stay fast even when a sibling process is
        // mid-write.
        LockFileGuard crossProc = AcquireRecentsLock(mSessionsDir, WRITE_LOCK_TIMEOUT_RUNTIME_MS);
        if (!crossProc.locked)
        {
            // Fail-closed: a sibling writer is mid-`Write`, and
            // racing their clear+rewrite of the entries sub-group
            // would corrupt the index. Returning an empty uuid lets
            // the caller treat this exactly like a serialization
            // failure.
            return QString();
        }

        QMutexLocker lock(&mMutex);

        const QString jsonPath = PathForUuid(uuid);
        if (jsonPath.isEmpty())
        {
            // `PathForUuid` only returns empty for a uuid that
            // failed `LooksLikeUuid`. We validated above so this is
            // an internal-consistency assertion -- if it fires the
            // uuid generation logic has drifted.
            return QString();
        }

        // The library serializer plus every `IRecentsIndexStorage`
        // call site runs through a single try / catch envelope:
        // any failure (disk full, QSettings corruption, ...) leaves
        // the in-process state untouched and returns an empty
        // uuid. Without the wider envelope, an exception thrown by
        // `mIndexStorage->Read()` would bypass the `LockFileGuard`
        // unlock (the destructor still fires) but skip the rest
        // of the write sequence, leaving the entries list in an
        // unknown state on disk.
        try
        {
            loglib::LogConfigurationManager::Save(configuration, jsonPath.toStdString(), loglib::SaveScope::Full);

            QList<RecentSessionEntry> entries = mIndexStorage->Read();

            RecentSessionEntry entry = MakeEntryMetadata(configuration);
            entry.uuid = uuid;
            entry.timestampMsEpoch = QDateTime::currentMSecsSinceEpoch();

            // Reuse-uuid fast path: a window auto-saving its own
            // session repeatedly is the steady-state case (every
            // streamingFinished, every Save Session). If the entry
            // is already at the head of the list and the metadata
            // is unchanged, the full-list QSettings rewrite is
            // pure overhead -- the on-disk JSON has already been
            // refreshed above and `EvictLocked` cannot shrink the
            // list further. Skipping the index write keeps the
            // common case allocation-free on the QSettings side
            // (no `clear() + setValue()` storm under the entries
            // sub-group). The timestamp-only drift is acceptable
            // because the menu rebuild re-reads on every
            // `aboutToShow` and uses the timestamp only for the
            // relative-time tooltip; a few-second skew is not
            // user-visible.
            const auto it = std::find_if(
                entries.begin(), entries.end(), [&](const RecentSessionEntry &e) { return e.uuid == uuid; }
            );
            const bool inPlaceFastPath =
                it != entries.end() && it == entries.begin() && it->label == entry.label
                && it->primaryLocator == entry.primaryLocator && it->fileCount == entry.fileCount;
            if (inPlaceFastPath)
            {
                // Skip the entries-group rewrite. Only refresh
                // `lastSessionUuid` if it drifted -- a cheap single
                // QSettings key, no `clear() + setValue()` storm
                // under `recentSessions/entries`. Timestamp drift
                // (the existing entry's `ts` is not refreshed in
                // QSettings) is acceptable because the menu
                // rebuild re-reads on every `aboutToShow` and the
                // timestamp only drives a relative-time tooltip
                // where a few-minute skew is invisible.
                const std::optional<QString> currentLast = mIndexStorage->ReadLastUuid();
                if (!currentLast.has_value() || *currentLast != uuid)
                {
                    mIndexStorage->WriteLastUuid(uuid);
                }
            }
            else
            {
                // Replace existing entry by uuid, otherwise push front.
                if (it != entries.end())
                {
                    entries.erase(it);
                }
                entries.prepend(entry);

                const QStringList evictedUuids = EvictLocked(entries);

                // Index write *first*, then unlink. A crash between the two
                // leaves an orphan JSON whose stem is no longer in the index
                // -- the next launch's `CleanupOrphanFiles` mops those up
                // automatically. The reverse order would leave a dangling
                // index entry pointing at a missing file, which surfaces as
                // a "Recent Session Unavailable" warning the next time the
                // user clicks the entry.
                mIndexStorage->Write(entries);
                mIndexStorage->WriteLastUuid(uuid);

                for (const QString &evictedUuid : evictedUuids)
                {
                    RemoveUuidFileLocked(evictedUuid);
                }
            }

            // Single-lock publish: when the caller asked us to
            // also publish the uuid into `openWindowsAtQuit`,
            // fold that write under the same cross-process lock
            // acquisition we already hold for the snapshot. This
            // halves the worst-case GUI freeze under sibling
            // contention (one acquisition instead of two) and
            // closes the small race window where a sibling could
            // observe the recents JSON updated but the
            // open-windows set still missing the uuid.
            //
            // The `OpenWindowsMutex` is taken too -- it is
            // independent of `mMutex` (guards a different
            // QSettings key) and a same-process sibling could
            // otherwise race the same set from a parallel
            // `RemoveOpenWindowUuid` call.
            if (publishOpenWindow)
            {
                QMutexLocker openWindowsLock(&OpenWindowsMutex());
                QStringList openUuids = ReadOpenWindowsAtQuit();
                if (!openUuids.contains(uuid))
                {
                    openUuids.append(uuid);
                    WriteOpenWindowsAtQuit(openUuids);
                }
            }
        }
        catch (const std::exception &e)
        {
            LOGAPP_WARN() << "WriteSnapshot failed:" << e.what();
            return QString();
        }

        changedFired = true;
    }

    if (changedFired)
    {
        emit changed();
    }
    return uuid;
}

bool SessionHistoryManager::Touch(const QString &uuid)
{
    if (uuid.isEmpty() || !logapp::LooksLikeUuid(uuid))
    {
        return false;
    }

    // Cheap pre-check: if @p uuid isn't even in the index there's
    // nothing to bump. Without this, an already-evicted uuid
    // (multi-window peer cleared the recents store between menu
    // rebuild and click) would still pay for the 1.5 s GUI freeze
    // under cross-process contention before returning empty-handed.
    //
    // The peek is taken under `mMutex` for the same reason the
    // post-lock phase below is: `IRecentsIndexStorage` is a virtual
    // interface and not every implementation is internally
    // thread-safe (the production `QSettings` backend happens to
    // be, but the in-memory storage used in tests is not). The
    // membership can still change between this peek and the
    // cross-process lock acquisition below; the post-lock recheck
    // (`bumped`) is authoritative and corrects either direction
    // (peek saw it but a sibling evicted before the lock; peek
    // missed it but a sibling re-added before the lock).
    {
        QMutexLocker peekLock(&mMutex);
        const QList<RecentSessionEntry> peek = mIndexStorage->Read();
        const auto found = std::find_if(
            peek.begin(), peek.end(), [&](const RecentSessionEntry &e) { return e.uuid == uuid; }
        );
        if (found == peek.end())
        {
            return false;
        }
    }

    // Acquire the cross-process lock *first* so a long contention
    // wait does not freeze `mMutex` (and therefore every same-
    // process `List()` reader). See the same comment on
    // `WriteSnapshot`.
    LockFileGuard crossProc = AcquireRecentsLock(mSessionsDir, WRITE_LOCK_TIMEOUT_RUNTIME_MS);
    if (!crossProc.locked)
    {
        // Fail-closed: skip the bump rather than racing a sibling
        // writer. The next successful `Touch` / `WriteSnapshot`
        // will re-establish the intended order.
        //
        // Returning `false` (the new contract) gates the caller's
        // `openWindowsAtQuit` publish: two sibling processes that
        // simultaneously fail to grab the lock cannot both publish
        // the same uuid, which the pre-fix `return true` allowed
        // (and which caused fan-restore to duplicate windows
        // across launches when two processes raced on shutdown).
        return false;
    }

    bool bumped = false;
    bool changedFired = false;
    {
        QMutexLocker lock(&mMutex);
        try
        {
            QList<RecentSessionEntry> entries = mIndexStorage->Read();

            const auto it = std::find_if(
                entries.begin(), entries.end(), [&](const RecentSessionEntry &e) { return e.uuid == uuid; }
            );
            if (it == entries.end())
            {
                // Race: the entry was evicted between the peek
                // above and the cross-process lock acquisition.
                // The index no longer contains @p uuid, so callers
                // that gate their `openWindowsAtQuit` publish on
                // our return value see "not present".
                bumped = false;
            }
            else
            {
                RecentSessionEntry refreshed = *it;
                refreshed.timestampMsEpoch = QDateTime::currentMSecsSinceEpoch();
                entries.erase(it);
                entries.prepend(refreshed);

                mIndexStorage->Write(entries);
                // Skip the `lastSessionUuid` rewrite when it is
                // already pointing at @p uuid. Multi-window restore
                // Touches every restored window back-to-back;
                // without this guard, every `Touch` would round-trip
                // QSettings even though the value is unchanged.
                const std::optional<QString> currentLast = mIndexStorage->ReadLastUuid();
                if (!currentLast.has_value() || *currentLast != uuid)
                {
                    mIndexStorage->WriteLastUuid(uuid);
                }

                bumped = true;
                changedFired = true;
            }
        }
        catch (const std::exception &e)
        {
            LOGAPP_WARN() << "Touch failed:" << e.what();
            return false;
        }
    }

    if (changedFired)
    {
        emit changed();
    }
    return bumped;
}

void SessionHistoryManager::Remove(const QString &uuid)
{
    if (uuid.isEmpty() || !logapp::LooksLikeUuid(uuid))
    {
        return;
    }

    bool changedFired = false;
    {
        // User-initiated removal -- use the longer timeout so a
        // transient sibling write doesn't make the menu action look
        // broken. Cross-process lock first so the in-process mutex
        // is held only across the fast in-memory work.
        LockFileGuard crossProc = AcquireRecentsLock(mSessionsDir, WRITE_LOCK_TIMEOUT_SHUTDOWN_MS);
        if (!crossProc.locked)
        {
            // Fail-closed: the dangling entry survives one more
            // launch rather than corrupting the index.
            return;
        }

        QMutexLocker lock(&mMutex);
        try
        {
            QList<RecentSessionEntry> entries = mIndexStorage->Read();

            const auto it = std::find_if(
                entries.begin(), entries.end(), [&](const RecentSessionEntry &e) { return e.uuid == uuid; }
            );
            if (it == entries.end())
            {
                return;
            }

            entries.erase(it);

            // Index write *first*, then unlink. A crash between the two
            // leaves the JSON behind as an orphan that
            // `CleanupOrphanFiles` sweeps on next launch (its stem is
            // no longer in the entries list). The reverse order would
            // leave the entries list referencing a missing JSON, which
            // surfaces as "Recent Session Unavailable" warnings that
            // the user can never repair without manually clicking the
            // entry.
            //
            // Within the index pair we still write entries *before* the
            // last-uuid pointer for the secondary reason described in
            // the previous revision: a stale `lastSessionUuid` pointing
            // at a missing file is harmless (`LastSessionPath` returns
            // nullopt) but the opposite order would leave
            // `lastSessionUuid` pointing at a uuid that is still in the
            // entries list -- the kind of phantom-but-parseable entry
            // we are explicitly trying to avoid.
            mIndexStorage->Write(entries);

            const std::optional<QString> currentLast = mIndexStorage->ReadLastUuid();
            if (currentLast.has_value() && *currentLast == uuid)
            {
                // Promote the newest survivor; otherwise drop the
                // pointer.
                const std::optional<QString> next =
                    entries.isEmpty() ? std::optional<QString>{} : std::optional<QString>{entries.front().uuid};
                mIndexStorage->WriteLastUuid(next);
            }

            RemoveUuidFileLocked(uuid);

            changedFired = true;
        }
        catch (const std::exception &e)
        {
            LOGAPP_WARN() << "Remove failed:" << e.what();
            return;
        }
    }

    if (changedFired)
    {
        emit changed();
    }
}

void SessionHistoryManager::Clear()
{
    bool changedFired = false;
    {
        // User-initiated "Clear Recent Sessions" -- shutdown-class
        // timeout so the menu action does the right thing even
        // under contention. Cross-process lock first; see the
        // `WriteSnapshot` comment for the rationale.
        LockFileGuard crossProc = AcquireRecentsLock(mSessionsDir, WRITE_LOCK_TIMEOUT_SHUTDOWN_MS);
        if (!crossProc.locked)
        {
            // Fail-closed: the user will have to re-trigger Clear
            // once the contender finishes.
            return;
        }

        QMutexLocker lock(&mMutex);
        try
        {
            const QList<RecentSessionEntry> entries = mIndexStorage->Read();

            // Index wipe *first*, then unlink the per-uuid JSONs. A
            // crash between the two leaves orphan files that the next
            // launch's `CleanupOrphanFiles` sweep removes; the reverse
            // order (unlink, then wipe) would leave the index briefly
            // referencing missing files, the failure mode this fix is
            // designed to prevent.
            mIndexStorage->Write({});
            mIndexStorage->WriteLastUuid(std::nullopt);

            for (const auto &e : entries)
            {
                RemoveUuidFileLocked(e.uuid);
            }

            changedFired = true;
        }
        catch (const std::exception &e)
        {
            LOGAPP_WARN() << "Clear failed:" << e.what();
            return;
        }
    }

    if (changedFired)
    {
        emit changed();
    }
}

std::optional<QString> SessionHistoryManager::LastSessionPath() const
{
    QMutexLocker lock(&mMutex);
    const std::optional<QString> uuid = mIndexStorage->ReadLastUuid();
    if (!uuid.has_value() || uuid->isEmpty())
    {
        return std::nullopt;
    }

    const QString path = PathForUuid(*uuid);
    // Empty path = the uuid stem failed the strict shape check
    // inside `PathForUuid` (corrupt profile). Treat as "no last
    // session" rather than escaping the sessions directory.
    if (path.isEmpty() || !QFileInfo::exists(path))
    {
        // Stale pointer (per-uuid JSON deleted out from under us).
        // Surface "no last session" rather than a path that will fail
        // to load.
        return std::nullopt;
    }
    return path;
}

QString SessionHistoryManager::PathForUuid(const QString &uuid) const
{
    // Refuse anything that is not strictly a UUID stem. The pre-fix
    // path was naive concatenation; a corrupted QSettings value or
    // a hand-edited profile could plant `"../etc/passwd"` and have
    // every consumer (`Remove`, `LastSessionPath`, the eviction
    // unlink in `WriteSnapshot`, the open-recent click path)
    // resolve a path *outside* the sessions directory. Returning
    // an empty string makes every downstream `QFile`/`QFileInfo`
    // call a fast no-op rather than reaching into a sensitive
    // location.
    if (!logapp::LooksLikeUuid(uuid))
    {
        return QString();
    }
    return mSessionsDir.filePath(uuid + QStringLiteral(".json"));
}

QString SessionHistoryManager::BuildLabel(const loglib::LogConfiguration &configuration)
{
    if (!configuration.source.has_value() || configuration.source->locators.empty())
    {
        return QStringLiteral("(no source)");
    }

    const QString primary = QString::fromStdString(configuration.source->locators.front());
    // Network streams persist their producer URI / display name as
    // the locator (e.g. `"TCP 127.0.0.1:5170"`). Running it through
    // `QFileInfo::fileName()` strips at the colon / path separator
    // and produces nonsense like `"5170"`; treat the locator as an
    // opaque label instead. Production avoids creating these (see
    // `MainWindow::ShouldAutoSaveSession`), but legacy entries from
    // pre-gate builds may still be in the index.
    QString primaryLabel;
    if (configuration.source->kind == loglib::LogConfiguration::Source::Kind::NetworkStream)
    {
        primaryLabel = primary;
    }
    else
    {
        // `Kind::File`: collapse the directory path so the menu
        // entry shows just the filename. Falls back to the raw
        // string when `fileName()` returns empty (e.g. trailing
        // slash) so we never display a blank entry.
        primaryLabel = QFileInfo(primary).fileName();
        if (primaryLabel.isEmpty())
        {
            primaryLabel = primary;
        }
    }
    const auto extra = static_cast<int>(configuration.source->locators.size()) - 1;
    if (extra <= 0)
    {
        return primaryLabel;
    }
    return QStringLiteral("%1 + %2 more").arg(primaryLabel).arg(extra);
}

RecentSessionEntry SessionHistoryManager::MakeEntryMetadata(const loglib::LogConfiguration &configuration)
{
    RecentSessionEntry entry;
    entry.label = BuildLabel(configuration);
    if (configuration.source.has_value() && !configuration.source->locators.empty())
    {
        entry.primaryLocator = QString::fromStdString(configuration.source->locators.front());
        entry.fileCount = static_cast<int>(configuration.source->locators.size());
    }
    return entry;
}

void SessionHistoryManager::RemoveUuidFileLocked(const QString &uuid)
{
    // Belt-and-braces: every call site already validates the uuid,
    // but `PathForUuid` is the canonical gate so we re-check here.
    // Empty path => `LooksLikeUuid(uuid) == false`, which means
    // some upstream call site forgot to validate before threading
    // a stem in.
    const QString path = PathForUuid(uuid);
    if (path.isEmpty())
    {
        return;
    }
    if (QFileInfo::exists(path))
    {
        // Ignore failures: a leftover file is harmless and the index
        // entry has already been removed by the caller.
        QFile(path).remove();
    }
}

QStringList SessionHistoryManager::EvictLocked(QList<RecentSessionEntry> &entries)
{
    QStringList evictedUuids;
    // Read the runtime cap once per eviction so a Preferences edit
    // mid-session takes effect on the next write without forcing
    // existing entries through an eager prune (which would be a
    // surprise for the user shrinking the cap while a sibling
    // window is mid-stream).
    const int cap = MaxEntries();
    while (entries.size() > cap)
    {
        const RecentSessionEntry evicted = entries.takeLast();
        evictedUuids.append(evicted.uuid);
    }
    return evictedUuids;
}

QStringList SessionHistoryManager::OpenWindowsAtQuit()
{
    QMutexLocker lock(&OpenWindowsMutex());
    // Read-only fast path: skip the cross-process lock entirely.
    // `AddOpenWindowUuid` / `RemoveOpenWindowUuid` /
    // `SetOpenWindowsAtQuit` all serialise their writes under the
    // cross-process lock, so the worst case here is a torn read of
    // the QSettings list -- acceptable on the shutdown /
    // restore-on-launch path, where blocking the GUI on a sibling's
    // writer queue would be the more user-visible problem. Also
    // avoids the `mkpath` side effect of `AcquireRecentsLock` at
    // process startup before any session has been auto-saved.
    return ReadOpenWindowsAtQuit();
}

void SessionHistoryManager::SetOpenWindowsAtQuit(const QStringList &uuids)
{
    QMutexLocker lock(&OpenWindowsMutex());
    // Used by startup-clear and the aboutToQuit safety-net -- both
    // tolerate the longer wait, neither is on an interactive path.
    LockFileGuard crossProc = AcquireRecentsLock(DefaultSessionsDir(), WRITE_LOCK_TIMEOUT_SHUTDOWN_MS);
    if (!crossProc.locked)
    {
        // Fail-closed: the persisted list reflects whatever the
        // previous writer left behind. On a crash-loop this could
        // mean a window is restored twice, but that is strictly
        // better than a torn write that loses every uuid.
        return;
    }
    WriteOpenWindowsAtQuit(uuids);
}

QStringList SessionHistoryManager::TakeOpenWindowsAtQuit()
{
    QMutexLocker lock(&OpenWindowsMutex());
    // Shutdown-class timeout: this runs once at startup and the
    // alternative (returning an empty list when contended) means
    // skipping the entire fan-restore. The lock contention window
    // is narrow in practice -- a sibling writer's `AddOpenWindowUuid`
    // finishes in milliseconds -- so the long timeout is essentially
    // unused except as a safety net.
    LockFileGuard crossProc = AcquireRecentsLock(DefaultSessionsDir(), WRITE_LOCK_TIMEOUT_SHUTDOWN_MS);
    if (!crossProc.locked)
    {
        // Fail-closed: return an empty list (and skip the wipe) when
        // the lock is contended. The pre-fix code returned the
        // *read-but-not-wiped* list, which let two sibling processes
        // both restore the same uuids from the same persisted set
        // (the wipe was supposed to be the gate that prevents
        // duplicate fan-restore, and skipping it broke that
        // invariant). Returning empty does cost a missed restore
        // this launch, but the next launch can recover (the
        // siblings' own `AddOpenWindowUuid` calls will republish
        // their windows over the runtime of this process).
        return {};
    }
    const QStringList uuids = ReadOpenWindowsAtQuit();
    WriteOpenWindowsAtQuit({});
    return uuids;
}

void SessionHistoryManager::AddOpenWindowUuid(const QString &uuid)
{
    if (uuid.isEmpty())
    {
        return;
    }
    QMutexLocker lock(&OpenWindowsMutex());
    LockFileGuard crossProc = AcquireRecentsLock(DefaultSessionsDir(), WRITE_LOCK_TIMEOUT_RUNTIME_MS);
    if (!crossProc.locked)
    {
        // Fail-closed: skip the publish. The next AutoSave (or the
        // `aboutToQuit` safety-net) will re-attempt to capture this
        // window for restore.
        return;
    }
    QStringList uuids = ReadOpenWindowsAtQuit();
    if (uuids.contains(uuid))
    {
        return;
    }
    uuids.append(uuid);
    WriteOpenWindowsAtQuit(uuids);
}

void SessionHistoryManager::AddOpenWindowUuids(const QStringList &uuids)
{
    if (uuids.isEmpty())
    {
        return;
    }
    QMutexLocker lock(&OpenWindowsMutex());
    // Shutdown-class timeout: the primary caller is the `aboutToQuit`
    // fan in `main()`, which would otherwise pay N * runtime-timeout
    // for N windows. Folding the publish into one lock acquisition
    // keeps the worst-case OS-quit stall bounded even with many
    // restorable windows. Other callers (the static
    // `AddOpenWindowUuid` per-window publish path) keep the runtime
    // timeout because they are on the interactive auto-save path.
    LockFileGuard crossProc = AcquireRecentsLock(DefaultSessionsDir(), WRITE_LOCK_TIMEOUT_SHUTDOWN_MS);
    if (!crossProc.locked)
    {
        // Fail-closed: skip the publish. Mirrors the per-uuid path.
        return;
    }
    QStringList merged = ReadOpenWindowsAtQuit();
    bool changed = false;
    for (const QString &uuid : uuids)
    {
        if (uuid.isEmpty() || merged.contains(uuid))
        {
            continue;
        }
        merged.append(uuid);
        changed = true;
    }
    if (changed)
    {
        WriteOpenWindowsAtQuit(merged);
    }
}

void SessionHistoryManager::RemoveOpenWindowUuid(const QString &uuid)
{
    if (uuid.isEmpty())
    {
        return;
    }
    QMutexLocker lock(&OpenWindowsMutex());
    LockFileGuard crossProc = AcquireRecentsLock(DefaultSessionsDir(), WRITE_LOCK_TIMEOUT_RUNTIME_MS);
    if (!crossProc.locked)
    {
        // Fail-closed: leave the stale uuid in place. The next
        // launch's restore loop will hit `QFileInfo::exists` and
        // skip it if the JSON is gone, or restore harmlessly if it
        // is still on disk.
        return;
    }
    QStringList uuids = ReadOpenWindowsAtQuit();
    if (!uuids.removeOne(uuid))
    {
        return;
    }
    WriteOpenWindowsAtQuit(uuids);
}

void SessionHistoryManager::CleanupOrphanFiles()
{
    if (!mSessionsDir.exists())
    {
        // Nothing has been auto-saved yet -- no directory, no orphans.
        return;
    }

    // Cross-process lock so we don't race a sibling `--new-instance`
    // primary mid-`WriteSnapshot` and misclassify its in-flight JSON
    // (already on disk, not yet in the index) as an orphan. Cleanup
    // runs from `main()` at startup which is mildly time-sensitive,
    // so use the runtime timeout. Cross-process first; see the
    // `WriteSnapshot` comment.
    LockFileGuard crossProc = AcquireRecentsLock(mSessionsDir, WRITE_LOCK_TIMEOUT_RUNTIME_MS);
    if (!crossProc.locked)
    {
        // Fail-closed: an orphan that survives this launch will be
        // swept up on the next startup. The alternative (deleting
        // a sibling's in-flight JSON) is worse.
        return;
    }

    QMutexLocker lock(&mMutex);
    QSet<QString> known;
    QStringList jsonFiles;
    try
    {
        // Build the set of uuids the index currently references so we
        // can bulk-distinguish orphans from live entries with one
        // allocation.
        const QList<RecentSessionEntry> entries = mIndexStorage->Read();
        known.reserve(entries.size());
        for (const RecentSessionEntry &entry : entries)
        {
            known.insert(entry.uuid);
        }

        // `*.json` for live entries plus `*.json.tmp` for leftovers
        // from a crash mid-`LogConfigurationManager::Save` (the atomic
        // write path streams into `<uuid>.json.tmp` before rename).
        // The lock file (`recents.lock`) and any future sibling
        // metadata never match either glob.
        jsonFiles =
            mSessionsDir.entryList({QStringLiteral("*.json"), QStringLiteral("*.json.tmp")}, QDir::Files);
    }
    catch (const std::exception &e)
    {
        LOGAPP_WARN() << "CleanupOrphanFiles: storage read failed:" << e.what();
        return;
    }

    static const QString TMP_SUFFIX = QStringLiteral(".json.tmp");
    static const QString JSON_SUFFIX = QStringLiteral(".json");
    int deletions = 0;
    for (const QString &fileName : jsonFiles)
    {
        // Hard cap on the per-launch deletion count: a pathological
        // sessions directory (hundreds of orphans from a long-running
        // misconfigured peer) should not be allowed to wedge startup
        // on a slow filesystem. Anything past the cap is left for the
        // next launch to mop up.
        if (deletions >= CLEANUP_DELETIONS_PER_LAUNCH)
        {
            LOGAPP_WARN() << "CleanupOrphanFiles: hit per-launch deletion cap of"
                          << CLEANUP_DELETIONS_PER_LAUNCH << "; the rest will be swept next launch.";
            break;
        }
        // Normalise the stem to the uuid: `QFileInfo::completeBaseName`
        // only strips the last suffix, so `<uuid>.json.tmp` would
        // become `<uuid>.json` and never match the `known` set --
        // which would silently make every `.tmp` an "orphan" by
        // accident rather than by intent. We classify explicitly so
        // the membership check matches the function's stated
        // contract (delete `<uuid>.{json,json.tmp}` iff `<uuid>` is
        // not in the index).
        QString stem;
        if (fileName.endsWith(TMP_SUFFIX))
        {
            stem = fileName.left(fileName.size() - TMP_SUFFIX.size());
        }
        else if (fileName.endsWith(JSON_SUFFIX))
        {
            stem = fileName.left(fileName.size() - JSON_SUFFIX.size());
        }
        else
        {
            // Filter shape changed in entryList -- skip rather than
            // delete an unexpected file.
            continue;
        }
        if (known.contains(stem))
        {
            continue;
        }
        // Defensive: only delete files whose stem looks like a
        // QUuid. The `sessionsDir` is app-managed by convention,
        // but a user (or unrelated tool) dropping `notes.json` in
        // there should not have it silently deleted on next launch.
        // Matches the stem-validation gate in
        // `MainWindow::RestoreLastSessionFromPath`, so the orphan
        // sweeper's contract stays in lockstep with the consumer's.
        if (!logapp::LooksLikeUuid(stem))
        {
            continue;
        }
        // Best-effort removal: ignore failures (filesystem error,
        // file vanished between listing and unlink, ...) -- the
        // worst case is the orphan survives one more launch.
        if (QFile(mSessionsDir.filePath(fileName)).remove())
        {
            ++deletions;
        }
    }
}

// -----------------------------------------------------------------------------
// QSettingsRecentsIndexStorage
// -----------------------------------------------------------------------------

QList<RecentSessionEntry> QSettingsRecentsIndexStorage::Read() const
{
    QSettings settings;
    int size = settings.value(QLatin1String(SETTINGS_SIZE_KEY), 0).toInt();
    // Cap the read size against a corrupted-profile attack: an
    // attacker (or a buggy migration from another tool) could plant
    // `recentSessions/size = INT_MAX` and watch us happily allocate
    // 2 GiB of `RecentSessionEntry`. The cap leaves plenty of
    // headroom for the legitimate `MaxEntries()` cap while bounding
    // worst-case allocation to O(KB). Anything past the runtime
    // cap gets evicted on the next `Write` anyway. We multiply
    // against the hard upper bound rather than the live preference
    // so a profile written with a smaller `MaxEntries()` still
    // round-trips its persisted entries through a sibling reader
    // that has the cap turned up.
    constexpr int CORRUPT_PROFILE_CAP =
        SessionHistoryManager::MAX_ENTRIES_UPPER_BOUND * CORRUPT_PROFILE_SIZE_CAP_MULTIPLIER;
    if (size < 0)
    {
        size = 0;
    }
    else if (size > CORRUPT_PROFILE_CAP)
    {
        LOGAPP_WARN() << "QSettingsRecentsIndexStorage::Read: capping recentSessions/size from" << size << "to"
                      << CORRUPT_PROFILE_CAP << "(corrupt profile suspected).";
        size = CORRUPT_PROFILE_CAP;
    }

    QList<RecentSessionEntry> out;
    out.reserve(size);
    for (int i = 0; i < size; ++i)
    {
        RecentSessionEntry entry;
        entry.uuid = settings.value(EntryKey(i, QStringLiteral("uuid"))).toString();
        if (entry.uuid.isEmpty())
        {
            // Corrupt slot -- skip so the rest of the index survives.
            continue;
        }
        // Storage-boundary uuid validation: drop slots whose uuid
        // is not strictly uuid-shaped. The pre-fix path happily
        // surfaced `"../foo"`-style values that would later be
        // composed into a filesystem path by `PathForUuid`. We now
        // gate at every layer (storage Read, PathForUuid,
        // RemoveUuidFileLocked) so a malicious / corrupt entry
        // cannot survive into any unlink call.
        if (!logapp::LooksLikeUuid(entry.uuid))
        {
            LOGAPP_WARN() << "QSettingsRecentsIndexStorage::Read: dropping malformed uuid at slot" << i << ":"
                          << entry.uuid;
            continue;
        }
        entry.label = settings.value(EntryKey(i, QStringLiteral("label"))).toString();
        entry.primaryLocator = settings.value(EntryKey(i, QStringLiteral("primary"))).toString();
        entry.fileCount = settings.value(EntryKey(i, QStringLiteral("fileCount")), 0).toInt();
        entry.timestampMsEpoch = settings.value(EntryKey(i, QStringLiteral("ts")), 0).toLongLong();
        out.push_back(std::move(entry));
    }
    return out;
}

void QSettingsRecentsIndexStorage::Write(const QList<RecentSessionEntry> &entries)
{
    QSettings settings;

    // Wipe the entries sub-group so a shrinking list doesn't leave
    // stale `recentSessions/entries/<i>/*` keys behind. We touch
    // only the entries sub-tree so `size` and `lastSessionUuid` --
    // which live one level up under `recentSessions/` -- survive
    // this scrub and get re-asserted below / by `WriteLastUuid`.
    settings.beginGroup(QLatin1String(SETTINGS_ENTRIES_GROUP));
    settings.remove(QString());
    settings.endGroup();

    // Crash-/torn-write recovery order: write the *entry slots*
    // first, `sync()` them to disk, *then* publish `size`. The
    // pre-fix order set `size = N` before any of the per-slot
    // values existed; a crash (or QSettings buffer drop) between
    // the two left `size = N` pointing at N empty slots, which
    // the reader could happily turn into N default-constructed
    // entries -- N "Recent Session Unavailable" warnings on the
    // next launch. The new order means a crash either before the
    // first `sync()` (no `size` published, no entries visible) or
    // before the second `sync()` (slots persisted, but `size` is
    // still the old value -- reader returns the old list, not a
    // half-written new one). The storage-side `LooksLikeUuid` gate
    // in `Read` is the second line of defense against the rare
    // torn-write that lands in between.
    for (int i = 0; i < entries.size(); ++i)
    {
        const auto &e = entries[i];
        settings.setValue(EntryKey(i, QStringLiteral("uuid")), e.uuid);
        settings.setValue(EntryKey(i, QStringLiteral("label")), e.label);
        settings.setValue(EntryKey(i, QStringLiteral("primary")), e.primaryLocator);
        settings.setValue(EntryKey(i, QStringLiteral("fileCount")), e.fileCount);
        settings.setValue(EntryKey(i, QStringLiteral("ts")), e.timestampMsEpoch);
    }
    settings.sync();
    SyncWarnIfNeeded(settings, "Write entries");

    settings.setValue(QLatin1String(SETTINGS_SIZE_KEY), entries.size());
    // Stamp the layout version so a future build can detect on-disk
    // schema drift and run a migration. Cheap to write on every
    // mutation (one int key) and avoids a "fixup once on first
    // write" code path that would itself need locking.
    settings.setValue(QLatin1String(SETTINGS_VERSION_KEY), CURRENT_RECENTS_VERSION);
    settings.sync();
    SyncWarnIfNeeded(settings, "Write size");
}

std::optional<QString> QSettingsRecentsIndexStorage::ReadLastUuid() const
{
    QSettings settings;
    const QVariant raw = settings.value(QLatin1String(SETTINGS_LAST_UUID_KEY));
    if (!raw.isValid())
    {
        return std::nullopt;
    }
    const QString value = raw.toString();
    if (value.isEmpty())
    {
        return std::nullopt;
    }
    return value;
}

void QSettingsRecentsIndexStorage::WriteLastUuid(const std::optional<QString> &uuid)
{
    QSettings settings;
    if (uuid.has_value() && !uuid->isEmpty())
    {
        settings.setValue(QLatin1String(SETTINGS_LAST_UUID_KEY), *uuid);
    }
    else
    {
        settings.remove(QLatin1String(SETTINGS_LAST_UUID_KEY));
    }
    settings.sync();
    SyncWarnIfNeeded(settings, "WriteLastUuid");
}
