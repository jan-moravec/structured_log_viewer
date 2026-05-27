// Unit tests for `ThemeControl` -- the singleton that owns the active
// theme bundle and drives the auto-switch between Light and Dark based
// on the OS palette.
//
// Runs with `QTEST_MAIN` so a `QApplication` is alive; the cache
// behind `qApp->palette()` and `qApp->setStyle()` is the production
// path under test.

#include "theme_control.hpp"

#include <loglib/theme.hpp>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QPalette>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTextStream>
#include <QtTest/QtTest>

namespace
{

constexpr char SETTINGS_KEY_ACTIVE[] = "theme/active";

/// Force-set the application's `Window` colour so the dark-mode
/// heuristic in `ThemeControl::Reevaluate` returns a deterministic
/// value regardless of the OS palette running the tests.
void SetPaletteWindow(const QColor &color)
{
    QPalette palette = qApp->palette();
    palette.setColor(QPalette::Window, color);
    qApp->setPalette(palette);
}

/// Drop the active selection so the next `LoadConfiguration` starts
/// from Auto. Keeps tests independent of whatever was persisted by a
/// previous run (CI workers reuse a user profile).
void ClearActiveSelection()
{
    QSettings settings;
    settings.remove(QString::fromLatin1(SETTINGS_KEY_ACTIVE));
}

void WriteUserTheme(const QDir &dir, const QString &fileName, const QString &json)
{
    QFile file(dir.filePath(fileName));
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QTextStream stream(&file);
    stream << json;
}

void RemoveAllUserThemes(const QDir &dir)
{
    const QStringList entries = dir.entryList(QStringList{QStringLiteral("*.json")}, QDir::Files);
    for (const QString &name : entries)
    {
        QFile::remove(dir.filePath(name));
    }
}

} // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage): `Q_OBJECT` QtTest fixture.
class ThemeControlTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        // Sandbox QStandardPaths so the test never touches the
        // real user profile. AppDataLocation will resolve to a
        // per-test-binary temp folder that pytest-style ctest
        // reuses across cases in this fixture.
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName("jan-moravec");
        QCoreApplication::setApplicationName("StructuredLogViewerTest");
    }

    void init()
    {
        // Each test starts from a known palette (Light) and an
        // empty user-themes folder so cases stay independent.
        // `LoadConfiguration` re-reads `theme/active` from QSettings
        // (now empty -> Auto) AND re-discovers themes, so the
        // in-memory `mActiveSelection` from a previous test does
        // not leak through.
        SetPaletteWindow(Qt::white);
        ClearActiveSelection();
        RemoveAllUserThemes(ThemeControl::UserThemesDir());
        ThemeControl::LoadConfiguration();
    }

    void cleanupTestCase()
    {
        ClearActiveSelection();
        RemoveAllUserThemes(ThemeControl::UserThemesDir());
    }

    /// Auto + light palette -> built-in Light.
    void TestAutoLightPalettePicksLight()
    {
        SetPaletteWindow(Qt::white);
        ThemeControl::SetActiveSelection(QString::fromLatin1(ThemeControl::AUTO_TOKEN));
        ThemeControl::Reevaluate();

        QCOMPARE(QString::fromStdString(ThemeControl::Active().name), QStringLiteral("Light"));
        QCOMPARE(ThemeControl::Active().kind, loglib::ThemeKind::Light);
    }

    /// Auto + dark palette -> built-in Dark.
    void TestAutoDarkPalettePicksDark()
    {
        SetPaletteWindow(QColor(0x22, 0x22, 0x22));
        ThemeControl::SetActiveSelection(QString::fromLatin1(ThemeControl::AUTO_TOKEN));
        ThemeControl::Reevaluate();

        QCOMPARE(QString::fromStdString(ThemeControl::Active().name), QStringLiteral("Dark"));
        QCOMPARE(ThemeControl::Active().kind, loglib::ThemeKind::Dark);
    }

    /// Explicit selection wins over the palette brightness.
    void TestExplicitSelectionOverridesPalette()
    {
        SetPaletteWindow(Qt::white);
        ThemeControl::SetActiveSelection(QStringLiteral("Dark"));

        QCOMPARE(QString::fromStdString(ThemeControl::Active().name), QStringLiteral("Dark"));
    }

    /// A user-dir JSON whose `name` matches a built-in shadows it
    /// everywhere -- including Auto mode -- so user overrides
    /// apply without flipping the active selection.
    void TestUserThemeShadowsBuiltin()
    {
        const QDir userDir = ThemeControl::UserThemesDir();
        constexpr auto OVERRIDE_FG = "#123456";
        WriteUserTheme(
            userDir,
            QStringLiteral("light.json"),
            QStringLiteral(R"({
                "name": "Light",
                "kind": "light",
                "levels": { "Info": { "foreground": "%1" } },
                "table": {},
                "app": {}
            })")
                .arg(QString::fromLatin1(OVERRIDE_FG))
        );

        ThemeControl::ReloadAll();
        const loglib::Theme &active = ThemeControl::Active();
        QCOMPARE(QString::fromStdString(active.name), QStringLiteral("Light"));
        const loglib::LevelStyle infoStyle = loglib::StyleForLevel(active, loglib::LogLevel::Info);
        QVERIFY(infoStyle.foreground.has_value());
        QCOMPARE(QString::fromStdString(*infoStyle.foreground), QString::fromLatin1(OVERRIDE_FG));

        // The listing should report the entry as fromUser=true so
        // the Preferences combo can annotate it.
        const auto listings = ThemeControl::AvailableThemes();
        bool foundUserLight = false;
        for (const auto &entry : listings)
        {
            if (entry.name == QStringLiteral("Light"))
            {
                foundUserLight = entry.fromUser;
                break;
            }
        }
        QVERIFY2(foundUserLight, "user-authored Light theme should shadow the built-in");
    }

    /// A user-named theme not colliding with a built-in becomes
    /// available without affecting the auto resolution.
    void TestUserThemeAppearsInListing()
    {
        const QDir userDir = ThemeControl::UserThemesDir();
        WriteUserTheme(
            userDir,
            QStringLiteral("Solarized.json"),
            QStringLiteral(R"({
                "name": "Solarized",
                "kind": "light",
                "levels": {},
                "table": {},
                "app": {}
            })")
        );
        ThemeControl::ReloadAll();

        const auto listings = ThemeControl::AvailableThemes();
        const bool present = std::any_of(listings.begin(), listings.end(), [](const auto &entry) {
            return entry.name == QStringLiteral("Solarized") && entry.fromUser;
        });
        QVERIFY2(present, "user theme should appear in AvailableThemes()");

        // Auto resolution is unchanged: light palette picks the
        // built-in Light, not the new user theme.
        SetPaletteWindow(Qt::white);
        ThemeControl::SetActiveSelection(QString::fromLatin1(ThemeControl::AUTO_TOKEN));
        ThemeControl::Reevaluate();
        QCOMPARE(QString::fromStdString(ThemeControl::Active().name), QStringLiteral("Light"));

        // Explicit selection of the user theme works.
        ThemeControl::SetActiveSelection(QStringLiteral("Solarized"));
        QCOMPARE(QString::fromStdString(ThemeControl::Active().name), QStringLiteral("Solarized"));
    }

    /// A selection change that actually flips the resolved theme
    /// fires `themeChanged` exactly once.
    void TestSelectionChangeFiresSignal()
    {
        ThemeControl::SetActiveSelection(QStringLiteral("Light"));
        QSignalSpy spy(&ThemeControl::Instance(), &ThemeControl::themeChanged);
        QVERIFY(spy.isValid());

        ThemeControl::SetActiveSelection(QStringLiteral("Dark"));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(QString::fromStdString(ThemeControl::Active().name), QStringLiteral("Dark"));

        // Re-applying the same selection is a no-op.
        ThemeControl::SetActiveSelection(QStringLiteral("Dark"));
        QCOMPARE(spy.count(), 1);
    }

    /// Brushes for Info on the Light preset are invalid (palette
    /// default), while Error has a foreground brush.
    void TestBrushLookups()
    {
        ThemeControl::SetActiveSelection(QStringLiteral("Light"));

        const QBrush infoFg = ThemeControl::ForegroundFor(loglib::LogLevel::Info);
        QCOMPARE(infoFg.style(), Qt::NoBrush);

        const QBrush errorFg = ThemeControl::ForegroundFor(loglib::LogLevel::Error);
        QVERIFY(errorFg.style() != Qt::NoBrush);
        QVERIFY(errorFg.color().isValid());

        // Fatal is bold in the preset.
        QFont base;
        const QFont fatalFont = ThemeControl::FontFor(loglib::LogLevel::Fatal, base);
        QVERIFY(fatalFont.bold());
    }
};

QTEST_MAIN(ThemeControlTest)
#include "test_theme_control.moc"
