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

/// Process-wide timeout for the cross-process lock. We want to wait
/// long enough to let a sibling process finish its read-modify-write
/// (`Read` + `Write`) but never block UI for a noticeable stretch.
constexpr int LOCK_FILE_TIMEOUT_MS = 2000;

QString EntryKey(int index, const QString &field)
{
    return QStringLiteral("%1/%2/%3").arg(QLatin1String(SETTINGS_ENTRIES_GROUP)).arg(index).arg(field);
}

/// Tries to acquire the cross-process recents lock. Returns a non-null
/// unique_ptr whose deleter unlocks on scope exit; the inner pointer
/// is null when the lock could not be created (e.g. the sessions
/// directory does not exist and `mkpath` failed). Callers proceed
/// regardless of the outcome -- losing one entry under heavy
/// contention beats freezing the UI on the shutdown path.
struct LockFileGuard
{
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
            if (locked && lock)
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
        if (locked && lock)
        {
            lock->unlock();
        }
    }
};

LockFileGuard AcquireRecentsLock(const QDir &sessionsDir)
{
    LockFileGuard guard;
    // The lock file lives next to the per-uuid JSONs; the directory
    // must exist before `tryLock` will succeed. `QDir::mkpath` is
    // non-const, so we make a one-off mutable copy here -- the caller
    // is unaffected because we only need the path string from it.
    // mkpath is idempotent + safe to call concurrently.
    if (!sessionsDir.exists() && !QDir(sessionsDir).mkpath(QStringLiteral(".")))
    {
        // Filesystem refusal (read-only volume, ENOSPC, ...); skip
        // the lock and let the caller fall through. Returning the
        // empty guard signals "no lock acquired".
        return guard;
    }
    guard.lock = std::make_unique<QLockFile>(sessionsDir.filePath(QStringLiteral("recents.lock")));
    guard.lock->setStaleLockTime(0);
    guard.locked = guard.lock->tryLock(LOCK_FILE_TIMEOUT_MS);
    return guard;
}
} // namespace

QDir SessionHistoryManager::DefaultSessionsDir()
{
    // Mirrors `RecentSessionsDir()` in `main.cpp`. Centralised so
    // `main()` and the static `openWindowsAtQuit` helpers below all
    // pick the same path regardless of who computes it.
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

    QMutexLocker lock(&mMutex);

    // The cross-process guard (acquired below) takes care of
    // `mkpath` for us, but we still need the directory before we
    // build the per-uuid JSON path -- AcquireRecentsLock guarantees
    // it exists when the returned lock is held.
    LockFileGuard crossProc = AcquireRecentsLock(mSessionsDir);

    const QString jsonPath = PathForUuid(uuid);

    // Reuse the library serializer so we share the schema with a
    // manual Save Session. Failures bubble out via the catch -- we
    // never want a stale index entry that points at a non-existent
    // JSON, but we also do not want to force every auto-save caller
    // on the GUI thread to wrap us in try / catch.
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

    EvictLocked(entries);

    mIndexStorage->Write(entries);
    mIndexStorage->WriteLastUuid(uuid);

    lock.unlock();
    emit changed();

    return uuid;
}

void SessionHistoryManager::Touch(const QString &uuid)
{
    if (uuid.isEmpty())
    {
        return;
    }

    QMutexLocker lock(&mMutex);

    LockFileGuard crossProc = AcquireRecentsLock(mSessionsDir);

    QList<RecentSessionEntry> entries = mIndexStorage->Read();

    const auto it = std::find_if(
        entries.begin(), entries.end(), [&](const RecentSessionEntry &e) { return e.uuid == uuid; }
    );
    if (it == entries.end())
    {
        return;
    }

    RecentSessionEntry refreshed = *it;
    refreshed.timestampMsEpoch = QDateTime::currentMSecsSinceEpoch();
    entries.erase(it);
    entries.prepend(refreshed);

    mIndexStorage->Write(entries);
    mIndexStorage->WriteLastUuid(uuid);

    lock.unlock();
    emit changed();
}

void SessionHistoryManager::Remove(const QString &uuid)
{
    if (uuid.isEmpty())
    {
        return;
    }

    QMutexLocker lock(&mMutex);

    LockFileGuard crossProc = AcquireRecentsLock(mSessionsDir);

    QList<RecentSessionEntry> entries = mIndexStorage->Read();

    const auto it = std::find_if(
        entries.begin(), entries.end(), [&](const RecentSessionEntry &e) { return e.uuid == uuid; }
    );
    if (it == entries.end())
    {
        return;
    }

    entries.erase(it);
    RemoveUuidFileLocked(uuid);

    const std::optional<QString> currentLast = mIndexStorage->ReadLastUuid();
    if (currentLast.has_value() && *currentLast == uuid)
    {
        // Promote the newest survivor; otherwise drop the pointer.
        const std::optional<QString> next =
            entries.isEmpty() ? std::optional<QString>{} : std::optional<QString>{entries.front().uuid};
        mIndexStorage->WriteLastUuid(next);
    }

    mIndexStorage->Write(entries);

    lock.unlock();
    emit changed();
}

void SessionHistoryManager::Clear()
{
    QMutexLocker lock(&mMutex);

    LockFileGuard crossProc = AcquireRecentsLock(mSessionsDir);

    const QList<RecentSessionEntry> entries = mIndexStorage->Read();
    for (const auto &e : entries)
    {
        RemoveUuidFileLocked(e.uuid);
    }
    mIndexStorage->Write({});
    mIndexStorage->WriteLastUuid(std::nullopt);

    lock.unlock();
    emit changed();
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
    const QString primaryBase = QFileInfo(primary).fileName();
    const auto extra = static_cast<int>(configuration.source->locators.size()) - 1;
    if (extra <= 0)
    {
        return primaryBase;
    }
    return QStringLiteral("%1 + %2 more").arg(primaryBase).arg(extra);
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

void SessionHistoryManager::EvictLocked(QList<RecentSessionEntry> &entries)
{
    while (entries.size() > MAX_ENTRIES)
    {
        const RecentSessionEntry evicted = entries.takeLast();
        RemoveUuidFileLocked(evicted.uuid);
    }
}

QString SessionHistoryManager::LockFilePath() const
{
    return mSessionsDir.filePath(QStringLiteral("recents.lock"));
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
    LockFileGuard crossProc = AcquireRecentsLock(DefaultSessionsDir());
    return ReadOpenWindowsAtQuit();
}

void SessionHistoryManager::SetOpenWindowsAtQuit(const QStringList &uuids)
{
    QMutexLocker lock(&OpenWindowsMutex());
    LockFileGuard crossProc = AcquireRecentsLock(DefaultSessionsDir());
    WriteOpenWindowsAtQuit(uuids);
}

void SessionHistoryManager::AddOpenWindowUuid(const QString &uuid)
{
    if (uuid.isEmpty())
    {
        return;
    }
    QMutexLocker lock(&OpenWindowsMutex());
    LockFileGuard crossProc = AcquireRecentsLock(DefaultSessionsDir());
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
    LockFileGuard crossProc = AcquireRecentsLock(DefaultSessionsDir());
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
    // (already on disk, not yet in the index) as an orphan.
    LockFileGuard crossProc = AcquireRecentsLock(mSessionsDir);

    // Build the set of uuids the index currently references so we can
    // bulk-distinguish orphans from live entries with one allocation.
    const QList<RecentSessionEntry> entries = mIndexStorage->Read();
    QSet<QString> known;
    known.reserve(entries.size());
    for (const RecentSessionEntry &entry : entries)
    {
        known.insert(entry.uuid);
    }

    // `*.json` only; the lock file (`recents.lock`) and any future
    // sibling metadata never match this glob.
    const QStringList jsonFiles = mSessionsDir.entryList({QStringLiteral("*.json")}, QDir::Files);
    for (const QString &fileName : jsonFiles)
    {
        const QString stem = QFileInfo(fileName).completeBaseName();
        if (known.contains(stem))
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
