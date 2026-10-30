// Tests for how `SessionHistoryManager` acquires its recents lock file.

#include "session_history_manager.hpp"

#include <loglib/log_configuration.hpp>

#include <QDir>
#include <QList>
#include <QLockFile>
#include <QString>
#include <QTemporaryDir>
#include <QTest>

#include <memory>
#include <optional>

#ifdef Q_OS_WIN
#include <windows.h>

#include <chrono>
#include <thread>
#endif

namespace
{
/// Index storage that keeps each case away from `QSettings`.
class InMemoryRecentsIndexStorage final : public IRecentsIndexStorage
{
public:
    QList<RecentSessionEntry> Read() const override
    {
        return mEntries;
    }

    void Write(const QList<RecentSessionEntry> &entries) override
    {
        mEntries = entries;
    }

    std::optional<QString> ReadLastUuid() const override
    {
        return mLastUuid;
    }

    void WriteLastUuid(const std::optional<QString> &uuid) override
    {
        mLastUuid = uuid;
    }

private:
    QList<RecentSessionEntry> mEntries;
    std::optional<QString> mLastUuid;
};

[[nodiscard]] loglib::LogConfiguration MakeConfiguration()
{
    loglib::LogConfiguration configuration;
    configuration.source =
        loglib::Source{.kind = loglib::Source::Kind::File, .locators = {std::string("C:/logs/lock-test.json")}};
    return configuration;
}

#ifdef Q_OS_WIN
/// Keeps a file in the Windows delete-pending state until released.
///
/// While the handle stays open the name remains reserved and creating
/// the file again fails with an access error, as it does when a file
/// scanner still holds a lock file that its owner has just removed.
class DeletePendingFile
{
public:
    explicit DeletePendingFile(const QString &path)
    {
        const std::wstring nativePath = QDir::toNativeSeparators(path).toStdWString();
        mHandle = CreateFileW(
            nativePath.c_str(),
            GENERIC_READ | DELETE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );
        if (mHandle == INVALID_HANDLE_VALUE)
        {
            return;
        }
        FILE_DISPOSITION_INFO disposition{};
        disposition.DeleteFile = TRUE;
        mPending = SetFileInformationByHandle(mHandle, FileDispositionInfo, &disposition, sizeof(disposition)) != FALSE;
    }

    DeletePendingFile(const DeletePendingFile &) = delete;
    DeletePendingFile &operator=(const DeletePendingFile &) = delete;

    ~DeletePendingFile()
    {
        Release();
    }

    [[nodiscard]] bool IsPending() const noexcept
    {
        return mPending;
    }

    void Release() noexcept
    {
        if (mHandle != INVALID_HANDLE_VALUE)
        {
            CloseHandle(mHandle);
            mHandle = INVALID_HANDLE_VALUE;
        }
    }

private:
    HANDLE mHandle = INVALID_HANDLE_VALUE;
    bool mPending = false;
};
#endif
} // namespace

class SessionHistoryLockTest : public QObject
{
    Q_OBJECT

private slots:
    void TestWriteSnapshotSucceedsWithFreeLock()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());
        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());

        QVERIFY(!manager.WriteSnapshot(MakeConfiguration()).isEmpty());
    }

#ifdef Q_OS_WIN
    void TestDeletePendingLockFileReportsPermissionError()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());
        const QString lockPath = QDir(sessionsDir.path()).filePath(QStringLiteral("recents.lock"));
        const DeletePendingFile pending(lockPath);
        QVERIFY(pending.IsPending());

        QLockFile probe(lockPath);
        QVERIFY(!probe.tryLock(0));
        QCOMPARE(probe.error(), QLockFile::PermissionError);
    }

    void TestWriteSnapshotWaitsForDeletePendingLockFile()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());
        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());
        DeletePendingFile pending(QDir(sessionsDir.path()).filePath(QStringLiteral("recents.lock")));
        QVERIFY(pending.IsPending());

        std::jthread releaser([&pending]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            pending.Release();
        });

        QVERIFY(!manager.WriteSnapshot(MakeConfiguration()).isEmpty());
    }

    void TestWriteSnapshotFailsClosedWhenLockFileStaysDeletePending()
    {
        const QTemporaryDir sessionsDir;
        QVERIFY(sessionsDir.isValid());
        SessionHistoryManager manager(QDir(sessionsDir.path()), std::make_unique<InMemoryRecentsIndexStorage>());
        const DeletePendingFile pending(QDir(sessionsDir.path()).filePath(QStringLiteral("recents.lock")));
        QVERIFY(pending.IsPending());

        const auto start = std::chrono::steady_clock::now();
        const QString uuid = manager.WriteSnapshot(MakeConfiguration());
        const auto elapsed = std::chrono::steady_clock::now() - start;

        QVERIFY(uuid.isEmpty());
        QVERIFY(manager.List().isEmpty());
        QVERIFY2(elapsed >= std::chrono::seconds(1), "the refusal must be retried until the timeout");
        QVERIFY2(elapsed < std::chrono::seconds(10), "the wait must stay bounded");
    }
#endif
};

QTEST_GUILESS_MAIN(SessionHistoryLockTest)
#include "test_session_history_lock.moc"
