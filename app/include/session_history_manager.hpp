#pragma once

#include <loglib/log_configuration.hpp>

#include <QDateTime>
#include <QDir>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QString>

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
/// Concurrency: every mutating method takes the internal `QMutex`
/// before touching the filesystem + index storage and emits
/// `changed()` after releasing the lock. A cross-process `QLockFile`
/// is layered on top in a follow-up commit (Part 5b backstop); this
/// class is designed to absorb that without an API change.
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
    /// the same mutex used by writers, so the snapshot is consistent.
    [[nodiscard]] QList<RecentSessionEntry> List() const;

    /// Persist @p configuration as a full session snapshot under
    /// `sessionsDir/<uuid>.json` and bump (or insert) the matching
    /// index entry. Returns the assigned uuid. If @p reuseUuid is
    /// non-empty and already maps to a current entry, the snapshot is
    /// rewritten in place (so a single MainWindow can amend its own
    /// session repeatedly without bloating the recents list).
    QString WriteSnapshot(const loglib::LogConfiguration &configuration, const QString &reuseUuid = QString());

    /// Move @p uuid to the top of the recents list and refresh its
    /// timestamp. No-op if @p uuid is not in the index.
    void Touch(const QString &uuid);

    /// Remove the entry + its per-uuid JSON file. No-op if @p uuid is
    /// not in the index.
    void Remove(const QString &uuid);

    /// Drop every entry and delete every per-uuid JSON file under
    /// `sessionsDir`.
    void Clear();

    /// Path to the last-session JSON, if any. Used by the
    /// restore-on-launch flow in `main.cpp`. The companion uuid is the
    /// most recently `WriteSnapshot`-ed entry; `lastSessionUuid` in the
    /// index storage tracks it.
    [[nodiscard]] std::optional<QString> LastSessionPath() const;

    /// Per-uuid JSON path. Public so the Recent Sessions menu can
    /// reopen an entry through `MainWindow::DoLoadConfiguration`.
    [[nodiscard]] QString PathForUuid(const QString &uuid) const;

    /// Sessions directory passed in at construction. Exposed for the
    /// `QLockFile` strategy in Part 5b and for tests that want to
    /// inspect on-disk state.
    [[nodiscard]] QDir SessionsDir() const
    {
        return mSessionsDir;
    }

    /// Read the `restoreLastSessionOnLaunch` user preference. Default
    /// is `true` (opt-in to a smooth restart). Backed by `QSettings`;
    /// kept here so the preference lives in the same module as the
    /// other recents-related keys.
    static bool RestoreLastSessionOnLaunch();
    static void SetRestoreLastSessionOnLaunch(bool enabled);

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

    /// Capacity-evict oldest entries until size <= MAX_ENTRIES. Caller
    /// holds `mMutex`.
    void EvictLocked(QList<RecentSessionEntry> &entries);

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
