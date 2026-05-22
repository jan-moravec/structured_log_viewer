#include "session_history_manager.hpp"

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
    guard.lock->setStaleLockTime(0);
    guard.locked = guard.lock->tryLock(timeoutMs);
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
    QString uuid = reuseUuid;
    if (uuid.isEmpty())
    {
        // Strip the curly braces so the uuid is filesystem-friendly
        // (Windows handles them fine but a clean stem makes ad-hoc
        // inspection easier).
        uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }

    // Scope the locks so both `mMutex` and the cross-process
    // `QLockFile` are released before we `emit changed()`. A future
    // direct connection on `changed` that triggers another mutator
    // on the same thread would otherwise deadlock on `QLockFile`
    // (it is not reentrant within a process).
    bool changedFired = false;
    {
        QMutexLocker lock(&mMutex);

        // The cross-process guard (acquired below) takes care of
        // `mkpath` for us, but we still need the directory before
        // we build the per-uuid JSON path -- AcquireRecentsLock
        // guarantees it exists when the returned lock is held.
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

        const QString jsonPath = PathForUuid(uuid);

        // Reuse the library serializer so we share the schema with
        // a manual Save Session. Failures bubble out via the catch
        // -- we never want a stale index entry that points at a
        // non-existent JSON, but we also do not want to force every
        // auto-save caller on the GUI thread to wrap us in
        // try / catch.
        try
        {
            loglib::LogConfigurationManager::Save(configuration, jsonPath.toStdString(), loglib::SaveScope::Full);
        }
        catch (const std::exception &)
        {
            return QString();
        }

        QList<RecentSessionEntry> entries = mIndexStorage->Read();

        RecentSessionEntry entry = MakeEntryMetadata(configuration);
        entry.uuid = uuid;
        entry.timestampMsEpoch = QDateTime::currentMSecsSinceEpoch();

        // Replace existing entry by uuid, otherwise push front.
        const auto it = std::find_if(
            entries.begin(), entries.end(), [&](const RecentSessionEntry &e) { return e.uuid == uuid; }
        );
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
    if (uuid.isEmpty())
    {
        return false;
    }

    // Cheap pre-check: if @p uuid isn't even in the index there's
    // nothing to bump. Without this, an already-evicted uuid
    // (multi-window peer cleared the recents store between menu
    // rebuild and click) would still pay for the 1.5 s GUI freeze
    // under cross-process contention before returning empty-handed.
    //
    // No `mMutex` here: `QSettings` reads are thread-safe and the
    // pre-check only needs a coherent point-in-time snapshot --
    // taking the in-process mutex would only serialise this peek
    // with concurrent same-process writers, which still race the
    // post-lock recheck (`foundUnderLock`) below regardless. The
    // membership can change between this peek and the
    // cross-process lock acquisition; the post-lock recheck is
    // authoritative and corrects either direction (peek saw it but
    // a sibling evicted before the lock; peek missed it but a
    // sibling re-added before the lock).
    {
        const QList<RecentSessionEntry> peek = mIndexStorage->Read();
        const auto found = std::find_if(
            peek.begin(), peek.end(), [&](const RecentSessionEntry &e) { return e.uuid == uuid; }
        );
        if (found == peek.end())
        {
            return false;
        }
    }

    bool foundUnderLock = true;
    bool changedFired = false;
    {
        QMutexLocker lock(&mMutex);

        LockFileGuard crossProc = AcquireRecentsLock(mSessionsDir, WRITE_LOCK_TIMEOUT_RUNTIME_MS);
        if (!crossProc.locked)
        {
            // Fail-closed: skip the bump rather than racing a
            // sibling writer. The next successful `Touch` /
            // `WriteSnapshot` will re-establish the intended order.
            // Still report `true` to the caller -- the pre-check
            // saw @p uuid in the index, so as far as the caller's
            // intent ("is this a recents entry I own?") goes, the
            // answer is yes; we just couldn't grab the lock to
            // reorder.
            return true;
        }

        QList<RecentSessionEntry> entries = mIndexStorage->Read();

        const auto it = std::find_if(
            entries.begin(), entries.end(), [&](const RecentSessionEntry &e) { return e.uuid == uuid; }
        );
        if (it == entries.end())
        {
            // Race: the entry was evicted between the peek above
            // and our acquisition of the cross-process lock. The
            // index no longer contains @p uuid, so callers that
            // gate their `openWindowsAtQuit` publish on our return
            // value should treat this as "not present".
            foundUnderLock = false;
        }
        else
        {
            RecentSessionEntry refreshed = *it;
            refreshed.timestampMsEpoch = QDateTime::currentMSecsSinceEpoch();
            entries.erase(it);
            entries.prepend(refreshed);

            mIndexStorage->Write(entries);
            // Skip the `lastSessionUuid` rewrite when it is already
            // pointing at @p uuid. Multi-window restore Touches every
            // restored window back-to-back; without this guard, every
            // `Touch` would round-trip QSettings even though the value
            // is unchanged.
            const std::optional<QString> currentLast = mIndexStorage->ReadLastUuid();
            if (!currentLast.has_value() || *currentLast != uuid)
            {
                mIndexStorage->WriteLastUuid(uuid);
            }

            changedFired = true;
        }
    }

    if (changedFired)
    {
        emit changed();
    }
    return foundUnderLock;
}

void SessionHistoryManager::Remove(const QString &uuid)
{
    if (uuid.isEmpty())
    {
        return;
    }

    bool changedFired = false;
    {
        QMutexLocker lock(&mMutex);

        // User-initiated removal -- use the longer timeout so a
        // transient sibling write doesn't make the menu action look
        // broken.
        LockFileGuard crossProc = AcquireRecentsLock(mSessionsDir, WRITE_LOCK_TIMEOUT_SHUTDOWN_MS);
        if (!crossProc.locked)
        {
            // Fail-closed: the dangling entry survives one more
            // launch rather than corrupting the index.
            return;
        }

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

    if (changedFired)
    {
        emit changed();
    }
}

void SessionHistoryManager::Clear()
{
    bool changedFired = false;
    {
        QMutexLocker lock(&mMutex);

        // User-initiated "Clear Recent Sessions" -- shutdown-class
        // timeout so the menu action does the right thing even
        // under contention.
        LockFileGuard crossProc = AcquireRecentsLock(mSessionsDir, WRITE_LOCK_TIMEOUT_SHUTDOWN_MS);
        if (!crossProc.locked)
        {
            // Fail-closed: the user will have to re-trigger Clear
            // once the contender finishes.
            return;
        }

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
    if (!QFileInfo::exists(path))
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
    const QString path = PathForUuid(uuid);
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
    while (entries.size() > MAX_ENTRIES)
    {
        const RecentSessionEntry evicted = entries.takeLast();
        evictedUuids.append(evicted.uuid);
    }
    return evictedUuids;
}

namespace
{
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
} // namespace

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
    const QStringList uuids = ReadOpenWindowsAtQuit();
    if (crossProc.locked)
    {
        WriteOpenWindowsAtQuit({});
    }
    // Lock-acquisition timeout: return the read value without
    // wiping. We'll re-read (and likely re-restore) on the next
    // launch, which is the safer half of the trade-off compared
    // to silently dropping every uuid.
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
    QMutexLocker lock(&mMutex);
    if (!mSessionsDir.exists())
    {
        // Nothing has been auto-saved yet -- no directory, no orphans.
        return;
    }

    // Cross-process lock so we don't race a sibling `--new-instance`
    // primary mid-`WriteSnapshot` and misclassify its in-flight JSON
    // (already on disk, not yet in the index) as an orphan. Cleanup
    // runs from `main()` at startup which is mildly time-sensitive,
    // so use the runtime timeout.
    LockFileGuard crossProc = AcquireRecentsLock(mSessionsDir, WRITE_LOCK_TIMEOUT_RUNTIME_MS);
    if (!crossProc.locked)
    {
        // Fail-closed: an orphan that survives this launch will be
        // swept up on the next startup. The alternative (deleting
        // a sibling's in-flight JSON) is worse.
        return;
    }

    // Build the set of uuids the index currently references so we can
    // bulk-distinguish orphans from live entries with one allocation.
    const QList<RecentSessionEntry> entries = mIndexStorage->Read();
    QSet<QString> known;
    known.reserve(entries.size());
    for (const RecentSessionEntry &entry : entries)
    {
        known.insert(entry.uuid);
    }

    // `*.json` for live entries plus `*.json.tmp` for leftovers from
    // a crash mid-`LogConfigurationManager::Save` (the atomic write
    // path streams into `<uuid>.json.tmp` before rename). The lock
    // file (`recents.lock`) and any future sibling metadata never
    // match either glob.
    const QStringList jsonFiles =
        mSessionsDir.entryList({QStringLiteral("*.json"), QStringLiteral("*.json.tmp")}, QDir::Files);
    static const QString TMP_SUFFIX = QStringLiteral(".json.tmp");
    static const QString JSON_SUFFIX = QStringLiteral(".json");
    for (const QString &fileName : jsonFiles)
    {
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
        if (QUuid::fromString(stem).isNull())
        {
            continue;
        }
        // Best-effort removal: ignore failures (filesystem error,
        // file vanished between listing and unlink, ...) -- the
        // worst case is the orphan survives one more launch.
        QFile(mSessionsDir.filePath(fileName)).remove();
    }
}

// -----------------------------------------------------------------------------
// QSettingsRecentsIndexStorage
// -----------------------------------------------------------------------------

QList<RecentSessionEntry> QSettingsRecentsIndexStorage::Read() const
{
    QSettings settings;
    const int size = settings.value(QLatin1String(SETTINGS_SIZE_KEY), 0).toInt();
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

    settings.setValue(QLatin1String(SETTINGS_SIZE_KEY), entries.size());
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
}
