#include "session_history_manager.hpp"

#include <QFileInfo>
#include <QLatin1String>
#include <QLockFile>
#include <QMutexLocker>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QUuid>
#include <QVariant>

#include <algorithm>
#include <exception>

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
} // namespace

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

    if (!mSessionsDir.exists())
    {
        // mkpath is idempotent + safe across concurrent callers.
        mSessionsDir.mkpath(QStringLiteral("."));
    }

    // Cross-process lock: serialize index read-modify-write across
    // sibling MainWindow processes. Held for the duration of the
    // index update so a concurrent `WriteSnapshot` in another
    // process either sees our committed entries or waits and then
    // bases its update on them -- never both racing on the same
    // `entries` snapshot. The per-uuid JSON write outside the lock
    // is safe because uuids are unique per (process, window).
    QLockFile crossProc(LockFilePath());
    crossProc.setStaleLockTime(0);
    const bool locked = crossProc.tryLock(LOCK_FILE_TIMEOUT_MS);
    // A stale-lock or filesystem-error case still lets us proceed
    // -- losing one entry in a contended cross-process scenario is
    // strictly better than blocking the UI forever.

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
        if (locked)
        {
            crossProc.unlock();
        }
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

    if (locked)
    {
        crossProc.unlock();
    }
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

    QLockFile crossProc(LockFilePath());
    crossProc.setStaleLockTime(0);
    const bool locked = crossProc.tryLock(LOCK_FILE_TIMEOUT_MS);

    QList<RecentSessionEntry> entries = mIndexStorage->Read();

    const auto it = std::find_if(
        entries.begin(), entries.end(), [&](const RecentSessionEntry &e) { return e.uuid == uuid; }
    );
    if (it == entries.end())
    {
        if (locked)
        {
            crossProc.unlock();
        }
        return;
    }

    RecentSessionEntry refreshed = *it;
    refreshed.timestampMsEpoch = QDateTime::currentMSecsSinceEpoch();
    entries.erase(it);
    entries.prepend(refreshed);

    mIndexStorage->Write(entries);
    mIndexStorage->WriteLastUuid(uuid);

    if (locked)
    {
        crossProc.unlock();
    }
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

    QLockFile crossProc(LockFilePath());
    crossProc.setStaleLockTime(0);
    const bool locked = crossProc.tryLock(LOCK_FILE_TIMEOUT_MS);

    QList<RecentSessionEntry> entries = mIndexStorage->Read();

    const auto it = std::find_if(
        entries.begin(), entries.end(), [&](const RecentSessionEntry &e) { return e.uuid == uuid; }
    );
    if (it == entries.end())
    {
        if (locked)
        {
            crossProc.unlock();
        }
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

    if (locked)
    {
        crossProc.unlock();
    }
    lock.unlock();
    emit changed();
}

void SessionHistoryManager::Clear()
{
    QMutexLocker lock(&mMutex);

    QLockFile crossProc(LockFilePath());
    crossProc.setStaleLockTime(0);
    const bool locked = crossProc.tryLock(LOCK_FILE_TIMEOUT_MS);

    const QList<RecentSessionEntry> entries = mIndexStorage->Read();
    for (const auto &e : entries)
    {
        RemoveUuidFileLocked(e.uuid);
    }
    mIndexStorage->Write({});
    mIndexStorage->WriteLastUuid(std::nullopt);

    if (locked)
    {
        crossProc.unlock();
    }
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

QStringList SessionHistoryManager::OpenWindowsAtQuit()
{
    QSettings settings;
    return settings.value(QLatin1String(SETTINGS_OPEN_WINDOWS_KEY)).toStringList();
}

void SessionHistoryManager::SetOpenWindowsAtQuit(const QStringList &uuids)
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
