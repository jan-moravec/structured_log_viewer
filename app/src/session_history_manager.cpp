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
/// changes in a way that needs migration code.
constexpr int CURRENT_RECENTS_VERSION = 1;

/// Upper bound on the entry count we trust from QSettings to guard
/// against a corrupted profile claiming gigabytes of entries. We
/// still evict past `MAX_ENTRIES` via `EvictLocked`.
constexpr int CORRUPT_PROFILE_SIZE_CAP_MULTIPLIER = 4;

/// Per-launch cap on orphan deletions. Stops a pathological
/// sessions directory (hundreds of orphans) from wedging startup
/// on a slow filesystem; the rest is swept on subsequent launches.
constexpr int CLEANUP_DELETIONS_PER_LAUNCH = 5000;

/// Once-per-process warning suppression for "could not acquire the
/// recents lock". Logging every timeout would drown real faults in
/// noise; the first failure is enough for triage.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables,readability-identifier-naming)
std::atomic<bool> g_recents_lock_warned{false};

/// Process-wide kill switch for `openWindowsAtQuit` mutations.
/// `true` (default) on the canonical primary; `false` on
/// `--new-instance` peers so their auto-save / detach calls never
/// touch the canonical primary's persisted restore set. See the
/// header for the full rationale.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables,readability-identifier-naming)
std::atomic<bool> g_publishing_enabled{true};

/// Once-per-process warning when `QSettings::sync()` reports an
/// error (read-only volume, ENOSPC, malformed profile). Same
/// flag-and-skip pattern as `g_recents_lock_warned`.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables,readability-identifier-naming)
std::atomic<bool> g_settings_status_warned{false};
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
    logapp::LogWarning() << "QSettings::sync() returned" << static_cast<int>(status) << "after" << context
                         << "; subsequent failures will be silent this process.";
}

/// Cross-process lock timeouts.
///
/// Runtime: covers GUI-thread mutators (`WriteSnapshot`, `Touch`,
/// `AddOpenWindowUuid`, `RemoveOpenWindowUuid`, `CleanupOrphanFiles`).
/// The timeout doubles as the worst-case GUI freeze under sibling
/// contention; 1.5 s stays under the 2 s "UI feels wedged" threshold.
///
/// Shutdown: covers user-initiated / shutdown mutators (`Remove`,
/// `Clear`, `SetOpenWindowsAtQuit`). Not on an interactive path; a
/// lost write across a launch / quit boundary is worse than a
/// longer wait.
///
/// Policy: callers fail closed. On timeout the mutator returns
/// without writing rather than racing the QSettings store (where
/// `Write` clears + rewrites the entries sub-tree). Worst case is
/// a dropped recents entry; previously-persisted state stays intact.
constexpr int WRITE_LOCK_TIMEOUT_RUNTIME_MS = 1500;
constexpr int WRITE_LOCK_TIMEOUT_SHUTDOWN_MS = 5000;

QString EntryKey(int index, const QString &field)
{
    return QStringLiteral("%1/%2/%3").arg(QLatin1String(SETTINGS_ENTRIES_GROUP)).arg(index).arg(field);
}

/// Try to acquire the cross-process recents lock. Callers must
/// check `guard.locked` and bail when it is false: the lock is a
/// strict gate, not a best-effort hint -- silently proceeding on
/// timeout would race `Write`'s clear+rewrite of the entries
/// sub-group and corrupt the index.
///
/// Side effect: materialises @p sessionsDir via `mkpath` so the
/// lock file can be created in place.
///
/// Invariant: `locked == true` implies `lock != nullptr`. Move-from
/// clears the source's `locked`.
struct LockFileGuard
{
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    std::unique_ptr<QLockFile> lock;
    bool locked = false;
    // NOLINTEND(misc-non-private-member-variables-in-classes)

    LockFileGuard() = default;
    LockFileGuard(const LockFileGuard &) = delete;
    LockFileGuard &operator=(const LockFileGuard &) = delete;
    LockFileGuard(LockFileGuard &&other) noexcept
        : lock(std::move(other.lock)), locked(other.locked)
    {
        other.locked = false;
    }
    LockFileGuard &operator=(LockFileGuard &&other) noexcept
    {
        if (this != &other)
        {
            // Unlock the current lock before the `unique_ptr`
            // reset destroys it; the cross-process recovery path
            // needs explicit `unlock()`.
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

/// Read the persisted `openWindowsAtQuit` list.
QStringList ReadOpenWindowsAtQuit()
{
    const QSettings settings;
    return settings.value(QLatin1String(SETTINGS_OPEN_WINDOWS_KEY)).toStringList();
}

void WriteOpenWindowsAtQuit(const QStringList &uuids)
{
    QSettings settings;
    if (uuids.isEmpty())
    {
        // Remove the key so a later read returns an empty list
        // without us having to special-case it.
        settings.remove(QLatin1String(SETTINGS_OPEN_WINDOWS_KEY));
    }
    else
    {
        settings.setValue(QLatin1String(SETTINGS_OPEN_WINDOWS_KEY), uuids);
    }
    settings.sync();
    SyncWarnIfNeeded(settings, "WriteOpenWindowsAtQuit");
}

/// Serialises read-modify-write of `openWindowsAtQuit` across the
/// windows in this process. The cross-process `QLockFile` in the
/// `Add` / `Remove` / `Set` paths handles inter-process races.
QMutex &OpenWindowsMutex()
{
    static QMutex mutex;
    return mutex;
}

LockFileGuard AcquireRecentsLock(const QDir &sessionsDir, int timeoutMs)
{
    LockFileGuard guard;
    // The lock file lives in `sessionsDir`; ensure it exists before
    // `tryLock`. `mkpath` is idempotent and concurrent-safe.
    if (!sessionsDir.exists() && !QDir(sessionsDir).mkpath(QStringLiteral(".")))
    {
        return guard;
    }
    guard.lock = std::make_unique<QLockFile>(sessionsDir.filePath(QStringLiteral("recents.lock")));
    // 30 s recovery window for crashed-holder locks; live-holder
    // contention still fails closed via the `tryLock` timeout below.
    constexpr int STALE_LOCK_TIMEOUT_MS = 30 * 1000;
    guard.lock->setStaleLockTime(STALE_LOCK_TIMEOUT_MS);
    guard.locked = guard.lock->tryLock(timeoutMs);
    if (!guard.locked && !g_recents_lock_warned.exchange(true))
    {
        // First failure only; error code disambiguates contention
        // vs. filesystem refusal.
        logapp::LogWarning() << "SessionHistoryManager: failed to acquire recents lock after" << timeoutMs
                             << "ms (error code:" << static_cast<int>(guard.lock->error())
                             << "); subsequent timeouts will be silent this process.";
    }
    return guard;
}
} // namespace

QDir SessionHistoryManager::DefaultSessionsDir()
{
    // One source of truth so the lock file is in the same place
    // regardless of who computes it.
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (base.isEmpty())
    {
        // Empty on exotic / portable setups; fall back to temp.
        base = QDir::tempPath();
    }
    return QDir(base).filePath(QStringLiteral("sessions"));
}

bool SessionHistoryManager::RestoreLastSessionOnLaunch()
{
    const QSettings settings;
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
    const QSettings settings;
    const QVariant raw = settings.value(QLatin1String(SETTINGS_MAX_ENTRIES_KEY));
    if (!raw.isValid())
    {
        return MAX_ENTRIES;
    }
    bool ok = false;
    const int value = raw.toInt(&ok);
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
    const QDir &sessionsDir, std::unique_ptr<IRecentsIndexStorage> indexStorage, QObject *parent
)
    : QObject(parent), mSessionsDir(sessionsDir), mIndexStorage(std::move(indexStorage))
{
    Q_ASSERT(mIndexStorage != nullptr);
    // Directory created lazily on first write.
}

SessionHistoryManager::~SessionHistoryManager() = default;

QList<RecentSessionEntry> SessionHistoryManager::List() const
{
    const QMutexLocker lock(&mMutex);
    return mIndexStorage->Read();
}

QString SessionHistoryManager::WriteSnapshot(const loglib::LogConfiguration &configuration, const QString &reuseUuid)
{
    return WriteSnapshotAndPublish(configuration, reuseUuid, /*publishOpenWindow=*/false);
}

QString SessionHistoryManager::WriteSnapshotAndPublish(
    const loglib::LogConfiguration &configuration, const QString &reuseUuid, bool publishOpenWindow, bool *publishedOut
)
{
    // Default the out-flag to false. Only the narrow "snapshot
    // succeeded AND publish landed" path flips it true.
    if (publishedOut != nullptr)
    {
        *publishedOut = false;
    }
    QString uuid = reuseUuid;
    if (uuid.isEmpty())
    {
        // Strip braces so the uuid stem is filesystem-friendly.
        uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    else if (!logapp::LooksLikeUuid(uuid))
    {
        // Reject non-uuid `reuseUuid` before it reaches `PathForUuid`
        // and escapes the managed pool.
        logapp::LogWarning() << "WriteSnapshot rejecting non-uuid reuseUuid:" << uuid;
        return {};
    }

    // Release both locks before `emit changed()`; a slot
    // re-entering a mutator on the same thread would deadlock on
    // the non-reentrant `QLockFile`.
    bool changedFired = false;
    {
        // Lock order: cross-process first, `mMutex` second. The
        // file lock can block; holding `mMutex` across the wait
        // would freeze every same-process reader.
        const LockFileGuard crossProc = AcquireRecentsLock(mSessionsDir, WRITE_LOCK_TIMEOUT_RUNTIME_MS);
        if (!crossProc.locked)
        {
            return {};
        }

        const QMutexLocker lock(&mMutex);

        const QString jsonPath = PathForUuid(uuid);
        if (jsonPath.isEmpty())
        {
            // Defensive: stem was validated above.
            return {};
        }

        // Envelope around serializer + storage calls: any failure
        // leaves in-process state untouched.
        try
        {
            loglib::LogConfigurationManager::Save(configuration, jsonPath.toStdString(), loglib::SaveScope::Full);

            QList<RecentSessionEntry> entries = mIndexStorage->Read();

            RecentSessionEntry entry = MakeEntryMetadata(configuration);
            entry.uuid = uuid;
            entry.timestampMsEpoch = QDateTime::currentMSecsSinceEpoch();

            // Fast path: if the entry is already at the head with
            // unchanged metadata, skip the list rewrite. The JSON
            // is refreshed already and the timestamp drift is
            // invisible (menu re-reads on `aboutToShow`).
            const auto it = std::find_if(entries.begin(), entries.end(), [&](const RecentSessionEntry &e) {
                return e.uuid == uuid;
            });
            const bool inPlaceFastPath = it != entries.end() && it == entries.begin() && it->label == entry.label &&
                                         it->primaryLocator == entry.primaryLocator && it->fileCount == entry.fileCount;
            if (inPlaceFastPath)
            {
                // Only refresh `lastSessionUuid` if it drifted.
                const std::optional<QString> currentLast = mIndexStorage->ReadLastUuid();
                if (!currentLast.has_value() || *currentLast != uuid)
                {
                    mIndexStorage->WriteLastUuid(uuid);
                }
            }
            else
            {
                // Replace existing entry by uuid; otherwise push front.
                if (it != entries.end())
                {
                    entries.erase(it);
                }
                entries.prepend(entry);

                const QStringList evictedUuids = EvictLocked(entries);

                // Index write *first*, then unlink. A crash between
                // the two leaves an orphan JSON that the next launch's
                // `CleanupOrphanFiles` reaps. The reverse order would
                // leave a dangling index entry pointing at a missing
                // file -- a "Recent Session Unavailable" warning.
                mIndexStorage->Write(entries);
                mIndexStorage->WriteLastUuid(uuid);

                for (const QString &evictedUuid : evictedUuids)
                {
                    RemoveUuidFileLocked(evictedUuid);
                }
            }

            // Fold the open-windows publish under the lock we
            // already hold; halves the worst-case GUI freeze.
            if (publishOpenWindow && IsPublishingEnabled())
            {
                const QMutexLocker openWindowsLock(&OpenWindowsMutex());
                QStringList openUuids = ReadOpenWindowsAtQuit();
                if (!openUuids.contains(uuid))
                {
                    openUuids.append(uuid);
                    WriteOpenWindowsAtQuit(openUuids);
                }
                if (publishedOut != nullptr)
                {
                    *publishedOut = true;
                }
            }
        }
        catch (const std::exception &e)
        {
            logapp::LogWarning() << "WriteSnapshot failed:" << e.what();
            return {};
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

    // Pre-check skips the lock wait when @p uuid was already
    // evicted; the post-lock recheck is authoritative.
    {
        const QMutexLocker peekLock(&mMutex);
        const QList<RecentSessionEntry> peek = mIndexStorage->Read();
        const auto found =
            std::find_if(peek.begin(), peek.end(), [&](const RecentSessionEntry &e) { return e.uuid == uuid; });
        if (found == peek.end())
        {
            return false;
        }
    }

    // Cross-process lock first so the wait does not freeze
    // `mMutex` (and every same-process reader).
    const LockFileGuard crossProc = AcquireRecentsLock(mSessionsDir, WRITE_LOCK_TIMEOUT_RUNTIME_MS);
    if (!crossProc.locked)
    {
        // Fail closed: returning `false` gates the caller's
        // `openWindowsAtQuit` publish so contended siblings
        // cannot both publish the same uuid.
        return false;
    }

    bool bumped = false;
    bool changedFired = false;
    {
        const QMutexLocker lock(&mMutex);
        try
        {
            QList<RecentSessionEntry> entries = mIndexStorage->Read();

            const auto it = std::find_if(entries.begin(), entries.end(), [&](const RecentSessionEntry &e) {
                return e.uuid == uuid;
            });
            if (it == entries.end())
            {
                // Race: a sibling evicted between peek and lock.
                bumped = false;
            }
            else
            {
                RecentSessionEntry refreshed = *it;
                refreshed.timestampMsEpoch = QDateTime::currentMSecsSinceEpoch();
                entries.erase(it);
                entries.prepend(refreshed);

                mIndexStorage->Write(entries);
                // Skip rewrite when `lastSessionUuid` already
                // points here; multi-window restore rarely changes it.
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
            logapp::LogWarning() << "Touch failed:" << e.what();
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
        // User-initiated: longer timeout so a transient sibling
        // write doesn't make the menu action look broken.
        const LockFileGuard crossProc = AcquireRecentsLock(mSessionsDir, WRITE_LOCK_TIMEOUT_SHUTDOWN_MS);
        if (!crossProc.locked)
        {
            return;
        }

        const QMutexLocker lock(&mMutex);
        try
        {
            QList<RecentSessionEntry> entries = mIndexStorage->Read();

            const auto it = std::find_if(entries.begin(), entries.end(), [&](const RecentSessionEntry &e) {
                return e.uuid == uuid;
            });
            if (it == entries.end())
            {
                return;
            }

            entries.erase(it);

            // Write index first, unlink second: a crash between
            // them leaves an orphan that `CleanupOrphanFiles`
            // reaps. The reverse order leaves a dangling entry.
            mIndexStorage->Write(entries);

            const std::optional<QString> currentLast = mIndexStorage->ReadLastUuid();
            if (currentLast.has_value() && *currentLast == uuid)
            {
                // Promote the newest survivor, or drop the pointer.
                const std::optional<QString> next =
                    entries.isEmpty() ? std::optional<QString>{} : std::optional<QString>{entries.front().uuid};
                mIndexStorage->WriteLastUuid(next);
            }

            RemoveUuidFileLocked(uuid);

            changedFired = true;
        }
        catch (const std::exception &e)
        {
            logapp::LogWarning() << "Remove failed:" << e.what();
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
        // User-initiated; shutdown-class timeout so the menu
        // action survives contention.
        const LockFileGuard crossProc = AcquireRecentsLock(mSessionsDir, WRITE_LOCK_TIMEOUT_SHUTDOWN_MS);
        if (!crossProc.locked)
        {
            return;
        }

        const QMutexLocker lock(&mMutex);
        try
        {
            const QList<RecentSessionEntry> entries = mIndexStorage->Read();

            // Wipe index first, unlink second: matches the
            // ordering in `Remove`.
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
            logapp::LogWarning() << "Clear failed:" << e.what();
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
    const QMutexLocker lock(&mMutex);
    const std::optional<QString> uuid = mIndexStorage->ReadLastUuid();
    if (!uuid.has_value() || uuid->isEmpty())
    {
        return std::nullopt;
    }

    QString path = PathForUuid(*uuid);
    // Empty => non-uuid stem (corrupt profile). Missing file =>
    // JSON deleted out from under us. Both -> "no last session".
    if (path.isEmpty() || !QFileInfo::exists(path))
    {
        return std::nullopt;
    }
    return path;
}

QString SessionHistoryManager::PathForUuid(const QString &uuid) const
{
    // Refuse non-uuid stems so a corrupt profile cannot escape
    // the sessions directory.
    if (!logapp::LooksLikeUuid(uuid))
    {
        return {};
    }
    return mSessionsDir.filePath(uuid + QStringLiteral(".json"));
}

QString SessionHistoryManager::BuildLabel(const loglib::LogConfiguration &configuration)
{
    // Keep this a pure function of `Source.{locators, kind}` --
    // the fast-path comparator in `WriteSnapshotAndPublish`
    // relies on those being the only inputs.
    if (!configuration.source.has_value() || configuration.source->locators.empty())
    {
        return QStringLiteral("(no source)");
    }

    const QString primary = QString::fromStdString(configuration.source->locators.front());
    // Network stream locators are URIs (e.g. `"TCP host:port"`);
    // `QFileInfo::fileName()` would mangle them.
    QString primaryLabel;
    if (configuration.source->kind == loglib::LogConfiguration::Source::Kind::NetworkStream)
    {
        primaryLabel = primary;
    }
    else
    {
        // File: show just the filename.
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

void SessionHistoryManager::RemoveUuidFileLocked(const QString &uuid) const
{
    const QString path = PathForUuid(uuid);
    if (path.isEmpty())
    {
        return;
    }
    if (QFileInfo::exists(path))
    {
        // Best-effort: a leftover file is harmless.
        QFile(path).remove();
    }
}

QStringList SessionHistoryManager::EvictLocked(QList<RecentSessionEntry> &entries)
{
    QStringList evictedUuids;
    // Read the cap once: Preferences edits take effect on the
    // next write, never eagerly prune existing entries.
    const int cap = MaxEntries();
    while (entries.size() > cap)
    {
        const RecentSessionEntry evicted = entries.takeLast();
        evictedUuids.append(evicted.uuid);
    }
    return evictedUuids;
}

QStringList SessionHistoryManager::OpenWindowsAtQuitUnlocked()
{
    const QMutexLocker lock(&OpenWindowsMutex());
    // Read-only: skip the cross-process lock. A torn read is
    // acceptable here; callers needing an authoritative snapshot
    // use `TakeOpenWindowsAtQuit`.
    return ReadOpenWindowsAtQuit();
}

void SessionHistoryManager::SetOpenWindowsAtQuit(const QStringList &uuids)
{
    if (!IsPublishingEnabled())
    {
        // `--new-instance` peer isolation. Mirrors Add / Remove.
        return;
    }
    // Lock order: crossProc -> OpenWindowsMutex.
    const LockFileGuard crossProc = AcquireRecentsLock(DefaultSessionsDir(), WRITE_LOCK_TIMEOUT_SHUTDOWN_MS);
    if (!crossProc.locked)
    {
        return;
    }
    const QMutexLocker lock(&OpenWindowsMutex());
    WriteOpenWindowsAtQuit(uuids);
}

QStringList SessionHistoryManager::TakeOpenWindowsAtQuit()
{
    const LockFileGuard crossProc = AcquireRecentsLock(DefaultSessionsDir(), WRITE_LOCK_TIMEOUT_SHUTDOWN_MS);
    if (!crossProc.locked)
    {
        // Fail closed: return empty AND skip the wipe so two
        // contended siblings don't both restore the same uuids.
        // Live siblings republish via `AddOpenWindowUuid`.
        return {};
    }
    const QMutexLocker lock(&OpenWindowsMutex());
    const QStringList uuids = ReadOpenWindowsAtQuit();
    WriteOpenWindowsAtQuit({});
    return uuids;
}

bool SessionHistoryManager::AddOpenWindowUuid(const QString &uuid)
{
    if (uuid.isEmpty())
    {
        return false;
    }
    if (!IsPublishingEnabled())
    {
        // Peer isolation: `--new-instance` peers never publish.
        return false;
    }
    const LockFileGuard crossProc = AcquireRecentsLock(DefaultSessionsDir(), WRITE_LOCK_TIMEOUT_RUNTIME_MS);
    if (!crossProc.locked)
    {
        // Fail closed; next AutoSave / aboutToQuit retries.
        return false;
    }
    const QMutexLocker lock(&OpenWindowsMutex());
    QStringList uuids = ReadOpenWindowsAtQuit();
    if (uuids.contains(uuid))
    {
        // Already published; keep the caller's latch in sync.
        return true;
    }
    uuids.append(uuid);
    WriteOpenWindowsAtQuit(uuids);
    return true;
}

void SessionHistoryManager::AddOpenWindowUuids(const QStringList &uuids)
{
    if (uuids.isEmpty())
    {
        return;
    }
    if (!IsPublishingEnabled())
    {
        return;
    }
    // One acquisition for N windows so the `aboutToQuit` fan
    // does not stall on OS-quit.
    const LockFileGuard crossProc = AcquireRecentsLock(DefaultSessionsDir(), WRITE_LOCK_TIMEOUT_SHUTDOWN_MS);
    if (!crossProc.locked)
    {
        return;
    }
    const QMutexLocker lock(&OpenWindowsMutex());
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
    if (!IsPublishingEnabled())
    {
        // Peer isolation: symmetrical to Add so a peer can't strip
        // the canonical primary's own uuid from the set.
        return;
    }
    const LockFileGuard crossProc = AcquireRecentsLock(DefaultSessionsDir(), WRITE_LOCK_TIMEOUT_RUNTIME_MS);
    if (!crossProc.locked)
    {
        // Fail closed; the next launch filters via `QFileInfo::exists`.
        return;
    }
    const QMutexLocker lock(&OpenWindowsMutex());
    QStringList uuids = ReadOpenWindowsAtQuit();
    if (!uuids.removeOne(uuid))
    {
        return;
    }
    WriteOpenWindowsAtQuit(uuids);
}

void SessionHistoryManager::SetPublishingEnabled(bool enabled) noexcept
{
    g_publishing_enabled.store(enabled, std::memory_order_release);
}

bool SessionHistoryManager::IsPublishingEnabled() noexcept
{
    return g_publishing_enabled.load(std::memory_order_acquire);
}

SessionHistoryManager::CleanupReport SessionHistoryManager::CleanupOrphanFiles()
{
    CleanupReport report;
    if (!mSessionsDir.exists())
    {
        return report;
    }

    // Cross-process lock prevents misclassifying a sibling's
    // in-flight JSON as an orphan.
    const LockFileGuard crossProc = AcquireRecentsLock(mSessionsDir, WRITE_LOCK_TIMEOUT_RUNTIME_MS);
    if (!crossProc.locked)
    {
        // Survivors get swept next launch.
        return report;
    }

    const QMutexLocker lock(&mMutex);
    QSet<QString> known;
    QStringList jsonFiles;
    try
    {
        const QList<RecentSessionEntry> entries = mIndexStorage->Read();
        known.reserve(entries.size());
        for (const RecentSessionEntry &entry : entries)
        {
            known.insert(entry.uuid);
        }

        // `*.json` plus `*.json.tmp` from a crash mid atomic-Save.
        jsonFiles = mSessionsDir.entryList({QStringLiteral("*.json"), QStringLiteral("*.json.tmp")}, QDir::Files);
    }
    catch (const std::exception &e)
    {
        logapp::LogWarning() << "CleanupOrphanFiles: storage read failed:" << e.what();
        return report;
    }

    static const QString TMP_SUFFIX = QStringLiteral(".json.tmp");
    static const QString JSON_SUFFIX = QStringLiteral(".json");
    for (const QString &fileName : jsonFiles)
    {
        // Cap deletions per launch; the rest waits.
        if (report.deletedCount >= CLEANUP_DELETIONS_PER_LAUNCH)
        {
            logapp::LogWarning() << "CleanupOrphanFiles: hit per-launch deletion cap of" << CLEANUP_DELETIONS_PER_LAUNCH
                                 << "; the rest will be swept next launch.";
            report.capped = true;
            break;
        }
        // Classify by suffix; `completeBaseName` would collapse
        // `<uuid>.json.tmp` to `<uuid>.json` and misclassify it.
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
            continue;
        }
        if (known.contains(stem))
        {
            continue;
        }
        // Only delete uuid-shaped files; never wipe a user-dropped
        // `notes.json`.
        if (!logapp::LooksLikeUuid(stem))
        {
            continue;
        }
        // Best-effort; survivors wait one more launch.
        if (QFile(mSessionsDir.filePath(fileName)).remove())
        {
            ++report.deletedCount;
        }
    }
    return report;
}

// -----------------------------------------------------------------------------
// QSettingsRecentsIndexStorage
// -----------------------------------------------------------------------------

QList<RecentSessionEntry> QSettingsRecentsIndexStorage::Read() const
{
    const QSettings settings;
    int size = settings.value(QLatin1String(SETTINGS_SIZE_KEY), 0).toInt();
    // Cap against a corrupt profile (`size = INT_MAX` would allocate
    // gigabytes). Multiplier leaves room for a peer with a larger
    // `MaxEntries` to round-trip.
    constexpr int CORRUPT_PROFILE_CAP =
        SessionHistoryManager::MAX_ENTRIES_UPPER_BOUND * CORRUPT_PROFILE_SIZE_CAP_MULTIPLIER;
    if (size < 0)
    {
        size = 0;
    }
    else if (size > CORRUPT_PROFILE_CAP)
    {
        logapp::LogWarning() << "QSettingsRecentsIndexStorage::Read: capping recentSessions/size from" << size << "to"
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
            continue;
        }
        // Drop non-uuid stems so `"../foo"`-style values cannot
        // reach `PathForUuid`.
        if (!logapp::LooksLikeUuid(entry.uuid))
        {
            logapp::LogWarning() << "QSettingsRecentsIndexStorage::Read: dropping malformed uuid at slot" << i << ":"
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
    // stale per-slot keys. `size` and `lastSessionUuid` are outside
    // the group and survive.
    settings.beginGroup(QLatin1String(SETTINGS_ENTRIES_GROUP));
    settings.remove(QString());
    settings.endGroup();

    // Write slots + sync, then publish `size`. A crash between
    // the two syncs keeps `size` at the old value (old list, not
    // a half-written new one).
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
    // Stamp the layout version for future migration detection.
    settings.setValue(QLatin1String(SETTINGS_VERSION_KEY), CURRENT_RECENTS_VERSION);
    settings.sync();
    SyncWarnIfNeeded(settings, "Write size");
}

std::optional<QString> QSettingsRecentsIndexStorage::ReadLastUuid() const
{
    const QSettings settings;
    const QVariant raw = settings.value(QLatin1String(SETTINGS_LAST_UUID_KEY));
    if (!raw.isValid())
    {
        return std::nullopt;
    }
    QString value = raw.toString();
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
