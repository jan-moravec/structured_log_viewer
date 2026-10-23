#include <loglib/user_paths.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QStandardPaths>
#include <QTest>

#include <filesystem>
#include <string_view>

namespace
{

[[nodiscard]] QString FromView(std::string_view text)
{
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

[[nodiscard]] QString QtPath(const std::filesystem::path &path)
{
#ifdef Q_OS_WIN
    return QDir::fromNativeSeparators(QString::fromStdWString(path.wstring()));
#else
    return QString::fromStdString(path.generic_string());
#endif
}

struct ApplicationIdentityGuard
{
    ApplicationIdentityGuard()
        : mOrganization(QCoreApplication::organizationName()),
          mApplication(QCoreApplication::applicationName()),
          mTestMode(QStandardPaths::isTestModeEnabled())
    {
        QStandardPaths::setTestModeEnabled(false);
        QCoreApplication::setOrganizationName(FromView(loglib::USER_DATA_ORGANIZATION_NAME));
        QCoreApplication::setApplicationName(FromView(loglib::USER_DATA_APPLICATION_NAME));
    }

    ~ApplicationIdentityGuard()
    {
        QCoreApplication::setOrganizationName(mOrganization);
        QCoreApplication::setApplicationName(mApplication);
        QStandardPaths::setTestModeEnabled(mTestMode);
    }

    ApplicationIdentityGuard(const ApplicationIdentityGuard &) = delete;
    ApplicationIdentityGuard &operator=(const ApplicationIdentityGuard &) = delete;

    QString mOrganization;
    QString mApplication;
    bool mTestMode = false;
};

} // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage): `Q_OBJECT` QtTest fixture; moc expects this declaration shape.
class UserDataDirectoryParityTest : public QObject
{
    Q_OBJECT

private slots:
    void TestMatchesAppDataLocation()
    {
        const ApplicationIdentityGuard identity;
        const QString expected = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        const QString actual = QtPath(loglib::UserDataDirectory());

        QVERIFY2(!expected.isEmpty(), "QStandardPaths::AppDataLocation was empty");
        QCOMPARE(actual, expected);
        QCOMPARE(
            QtPath(loglib::UserRegexTemplateDirectory()), QDir(expected).filePath(QStringLiteral("regex_templates"))
        );
    }
};

QTEST_MAIN(UserDataDirectoryParityTest)
#include "test_user_data_directory.moc"
