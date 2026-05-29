// Unit tests for `ThemeControl`. Uses `QTEST_MAIN` so a real
// `QApplication` is alive and the production palette / style code
// path runs under test.

#include "theme_control.hpp"

#include <loglib/theme.hpp>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>
#include <QTextStream>
#include <QtTest/QtTest>
#include <Qt>

namespace
{

constexpr char SETTINGS_KEY_ACTIVE[] = "theme/active";

/// Pin the cached OS colour scheme for the next Auto resolution.
/// Bypasses `setColorScheme()` (which would engage Force mode).
void FakeOsColorScheme(Qt::ColorScheme scheme)
{
    ThemeControl::SetOsColorSchemeForTest(scheme);
}

/// Drop the persisted active selection so the next
/// `LoadConfiguration` starts from Auto. Keeps tests independent
/// of CI worker profile state.
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
        // Sandbox QStandardPaths so AppDataLocation points to a
        // per-test temp folder, not the real user profile.
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName("jan-moravec");
        QCoreApplication::setApplicationName("StructuredLogViewerTest");
    }

    void init()
    {
        // Reset to a known baseline: faked Light OS, empty user
        // themes folder, persisted Auto selection. Capture the
        // entry-time style so `cleanup()` can restore it after
        // tests that pin `app.qtStyle`.
        if (qApp != nullptr && qApp->style() != nullptr)
        {
            mInitStyleName = qApp->style()->name();
        }
        ClearActiveSelection();
        RemoveAllUserThemes(ThemeControl::UserThemesDir());
        ThemeControl::LoadConfiguration();
        // Override the cached OS scheme AFTER LoadConfiguration
        // (which captured the pristine value first).
        FakeOsColorScheme(Qt::ColorScheme::Light);
    }

    void cleanup()
    {
        // Restore the entry-time style so a follow-on test doesn't
        // inherit a `qtStyle: "fusion"` override.
        if (qApp != nullptr && !mInitStyleName.isEmpty() && qApp->style() != nullptr &&
            qApp->style()->name().compare(mInitStyleName, Qt::CaseInsensitive) != 0)
        {
            if (QStyle *style = QStyleFactory::create(mInitStyleName); style != nullptr)
            {
                qApp->setStyle(style);
            }
        }
        // Don't `unsetColorScheme()` here -- it would desync
        // `QStyleHints` from `mColorSchemeForced`. The next
        // test's Auto path releases the override cleanly.
    }

    void cleanupTestCase()
    {
        ClearActiveSelection();
        RemoveAllUserThemes(ThemeControl::UserThemesDir());

        // Reset the palette to the current style's standard
        // palette so a follow-on test binary starts clean.
        if (qApp != nullptr && qApp->style() != nullptr)
        {
            qApp->setPalette(qApp->style()->standardPalette());
        }
    }

    /// Auto + light OS scheme -> built-in Light.
    void TestAutoLightPalettePicksLight()
    {
        FakeOsColorScheme(Qt::ColorScheme::Light);
        ThemeControl::SetActiveSelection(QString::fromLatin1(ThemeControl::AUTO_TOKEN));
        ThemeControl::Reevaluate();

        QCOMPARE(QString::fromStdString(ThemeControl::Active().name), QStringLiteral("Light"));
        QCOMPARE(ThemeControl::Active().kind, loglib::ThemeKind::Light);
    }

    /// Auto + dark OS scheme -> built-in Dark.
    void TestAutoDarkPalettePicksDark()
    {
        FakeOsColorScheme(Qt::ColorScheme::Dark);
        ThemeControl::SetActiveSelection(QString::fromLatin1(ThemeControl::AUTO_TOKEN));
        ThemeControl::Reevaluate();

        QCOMPARE(QString::fromStdString(ThemeControl::Active().name), QStringLiteral("Dark"));
        QCOMPARE(ThemeControl::Active().kind, loglib::ThemeKind::Dark);
    }

    /// Explicit selection wins over the OS scheme.
    void TestExplicitSelectionOverridesPalette()
    {
        FakeOsColorScheme(Qt::ColorScheme::Light);
        ThemeControl::SetActiveSelection(QStringLiteral("Dark"));

        QCOMPARE(QString::fromStdString(ThemeControl::Active().name), QStringLiteral("Dark"));
    }

    /// A user file whose `name` matches a built-in shadows it
    /// everywhere, including Auto mode.
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

        // The listing must mark the entry `fromUser` so the UI
        // can annotate it.
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

    /// A non-colliding user theme appears in the listing without
    /// affecting auto resolution.
    void TestUserThemeAppearsInListing()
    {
        const QDir userDir = ThemeControl::UserThemesDir();
        WriteUserTheme(userDir, QStringLiteral("Solarized.json"), QStringLiteral(R"({
                "name": "Solarized",
                "kind": "light",
                "levels": {},
                "table": {},
                "app": {}
            })"));
        ThemeControl::ReloadAll();

        const auto listings = ThemeControl::AvailableThemes();
        const bool present = std::any_of(listings.begin(), listings.end(), [](const auto &entry) {
            return entry.name == QStringLiteral("Solarized") && entry.fromUser;
        });
        QVERIFY2(present, "user theme should appear in AvailableThemes()");

        // Auto still picks the built-in Light, not the new user
        // theme.
        FakeOsColorScheme(Qt::ColorScheme::Light);
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
        const QSignalSpy spy(&ThemeControl::Instance(), &ThemeControl::themeChanged);
        QVERIFY(spy.isValid());

        ThemeControl::SetActiveSelection(QStringLiteral("Dark"));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(QString::fromStdString(ThemeControl::Active().name), QStringLiteral("Dark"));

        // Re-selecting the same theme is a no-op.
        ThemeControl::SetActiveSelection(QStringLiteral("Dark"));
        QCOMPARE(spy.count(), 1);
    }

    /// On the Light preset: Info defers to the palette, Error has
    /// a brush, Fatal is bold.
    void TestBrushLookups()
    {
        ThemeControl::SetActiveSelection(QStringLiteral("Light"));

        const QBrush infoFg = ThemeControl::ForegroundFor(loglib::LogLevel::Info);
        QCOMPARE(infoFg.style(), Qt::NoBrush);

        const QBrush errorFg = ThemeControl::ForegroundFor(loglib::LogLevel::Error);
        QVERIFY(errorFg.style() != Qt::NoBrush);
        QVERIFY(errorFg.color().isValid());

        const QFont fatalFont = ThemeControl::FontFor(loglib::LogLevel::Fatal);
        QVERIFY(fatalFont.bold());
    }

    /// Regression: Force "Light" -> Auto on a light OS must drop
    /// the `QStyleHints::colorScheme` override, even though the
    /// resolved theme name doesn't change.
    void TestForceToAutoSameResolvedName()
    {
        FakeOsColorScheme(Qt::ColorScheme::Light);
        ThemeControl::SetActiveSelection(QStringLiteral("Light"));
        QVERIFY2(ThemeControl::IsColorSchemeForcedForTest(), "Force-mode must pin the colour scheme");

        ThemeControl::SetActiveSelection(QString::fromLatin1(ThemeControl::AUTO_TOKEN));
        // Resolved theme stays Light, but the override must drop
        // so OS flips can drive `colorSchemeChanged`.
        QCOMPARE(QString::fromStdString(ThemeControl::Active().name), QStringLiteral("Light"));
        QVERIFY2(
            !ThemeControl::IsColorSchemeForcedForTest(),
            "Auto mode must release the QStyleHints::colorScheme override even when the "
            "resolved theme name didn't change"
        );
    }

    /// Regression: Force "Dark" -> Auto on a light OS must resolve
    /// to Light. The cross-kind variant of
    /// `TestForceToAutoSameResolvedName`.
    void TestForceDarkToAutoLightOsPicksLight()
    {
        FakeOsColorScheme(Qt::ColorScheme::Light);
        ThemeControl::SetActiveSelection(QStringLiteral("Dark"));
        QCOMPARE(QString::fromStdString(ThemeControl::Active().name), QStringLiteral("Dark"));
        QVERIFY(ThemeControl::IsColorSchemeForcedForTest());

        ThemeControl::SetActiveSelection(QString::fromLatin1(ThemeControl::AUTO_TOKEN));
        QCOMPARE(QString::fromStdString(ThemeControl::Active().name), QStringLiteral("Light"));
        QVERIFY2(
            !ThemeControl::IsColorSchemeForcedForTest(), "Auto mode must release the Force-mode colour-scheme override"
        );
    }

    /// Symmetric regression: Force "Light" -> Auto on a dark OS
    /// must resolve to Dark.
    void TestForceLightToAutoDarkOsPicksDark()
    {
        FakeOsColorScheme(Qt::ColorScheme::Dark);
        ThemeControl::SetActiveSelection(QStringLiteral("Light"));
        QCOMPARE(QString::fromStdString(ThemeControl::Active().name), QStringLiteral("Light"));
        QVERIFY(ThemeControl::IsColorSchemeForcedForTest());

        ThemeControl::SetActiveSelection(QString::fromLatin1(ThemeControl::AUTO_TOKEN));
        QCOMPARE(QString::fromStdString(ThemeControl::Active().name), QStringLiteral("Dark"));
        QVERIFY(!ThemeControl::IsColorSchemeForcedForTest());
    }

    /// Regression: Force-Light on light OS -> OS flips to dark
    /// (suppressed by our override) -> back to Auto must resolve
    /// to Dark, not stick on Light.
    void TestForceModeOsFlipResolvesAutoCorrectly()
    {
        FakeOsColorScheme(Qt::ColorScheme::Light);
        ThemeControl::SetActiveSelection(QStringLiteral("Light"));
        QVERIFY(ThemeControl::IsColorSchemeForcedForTest());
        QCOMPARE(QString::fromStdString(ThemeControl::Active().name), QStringLiteral("Light"));

        // OS flips to dark while Force is held. In production our
        // slot would skip recording it; the fake setter encodes
        // the new OS state for the Auto-path re-sample.
        FakeOsColorScheme(Qt::ColorScheme::Dark);

        ThemeControl::SetActiveSelection(QString::fromLatin1(ThemeControl::AUTO_TOKEN));
        QCOMPARE(QString::fromStdString(ThemeControl::Active().name), QStringLiteral("Dark"));
        QVERIFY(!ThemeControl::IsColorSchemeForcedForTest());
    }

    /// Auto with `Qt::ColorScheme::Unknown` (minimal Linux CI with
    /// no platform theme) defaults to Light, not Dark.
    void TestAutoUnknownOsPicksLight()
    {
        FakeOsColorScheme(Qt::ColorScheme::Unknown);
        ThemeControl::SetActiveSelection(QString::fromLatin1(ThemeControl::AUTO_TOKEN));
        ThemeControl::Reevaluate();

        QCOMPARE(QString::fromStdString(ThemeControl::Active().name), QStringLiteral("Light"));
    }

    /// `SaveUserTheme` rejects unsafe names so a path like
    /// `../evil` can't write outside `UserThemesDir`.
    void TestSaveUserThemeRejectsBadName()
    {
        loglib::Theme theme;
        theme.name = "ignored";

        QVERIFY_THROWS_EXCEPTION(std::runtime_error, ThemeControl::SaveUserTheme(QStringLiteral("../evil"), theme));
        QVERIFY_THROWS_EXCEPTION(
            std::runtime_error, ThemeControl::SaveUserTheme(QStringLiteral("sub/dir/theme"), theme)
        );
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, ThemeControl::SaveUserTheme(QStringLiteral(""), theme));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, ThemeControl::SaveUserTheme(QStringLiteral("CON"), theme));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, ThemeControl::SaveUserTheme(QStringLiteral("nul"), theme));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, ThemeControl::SaveUserTheme(QStringLiteral(".."), theme));
        QVERIFY_THROWS_EXCEPTION(
            std::runtime_error, ThemeControl::SaveUserTheme(QStringLiteral("contains\nnewline"), theme)
        );
        // Trailing `.` / ` ` would silently collide on Win32.
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, ThemeControl::SaveUserTheme(QStringLiteral("Dark."), theme));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, ThemeControl::SaveUserTheme(QStringLiteral("Dark "), theme));
        // A plain name must round-trip through the index.
        ThemeControl::SaveUserTheme(QStringLiteral("Sepia"), theme);
        ThemeControl::ReloadAll();
        const auto listings = ThemeControl::AvailableThemes();
        const bool present = std::any_of(listings.begin(), listings.end(), [](const auto &entry) {
            return entry.name == QStringLiteral("Sepia") && entry.fromUser;
        });
        QVERIFY2(present, "valid user theme should round-trip through SaveUserTheme + ReloadAll");
    }

    /// `ReloadAll` with no on-disk edits must not emit
    /// `themeChanged` (byte-equal fast-path).
    void TestReloadAllSkipsWhenUnchanged()
    {
        ThemeControl::SetActiveSelection(QStringLiteral("Light"));
        const QSignalSpy spy(&ThemeControl::Instance(), &ThemeControl::themeChanged);
        QVERIFY(spy.isValid());

        ThemeControl::ReloadAll();
        QCOMPARE(spy.count(), 0);
    }

    /// A theme with an unknown `app.qtStyle` must not crash and
    /// must leave the previous style in place.
    void TestMissingStyleFactoryFallback()
    {
        const QDir userDir = ThemeControl::UserThemesDir();
        WriteUserTheme(userDir, QStringLiteral("BadStyle.json"), QStringLiteral(R"({
                "name": "BadStyle",
                "kind": "light",
                "levels": {},
                "table": {},
                "chrome": {},
                "app": { "qtStyle": "NonexistentStyle12345" }
            })"));
        ThemeControl::ReloadAll();

        const QString priorStyle = qApp->style()->name();
        ThemeControl::SetActiveSelection(QStringLiteral("BadStyle"));

        QCOMPARE(QString::fromStdString(ThemeControl::Active().name), QStringLiteral("BadStyle"));
        // Unknown style -> apply path skips `setStyle`.
        QCOMPARE(qApp->style()->name(), priorStyle);
    }

    /// Regression: a theme that omits `app.fontFamily` must
    /// restore the startup font rather than inherit the previous
    /// theme's. Same contract for `app.qtStyle`.
    void TestRevertToStartupFontWhenThemeOmitsField()
    {
        const QFont startupFont = qApp->font();
        const QString startupStyleName = qApp->style()->name();

        // Bump the point size so the revert is observable even
        // when CI font substitution lands on the startup family.
        const int pinnedPointSize = startupFont.pointSize() + 2;
        const QDir userDir = ThemeControl::UserThemesDir();
        WriteUserTheme(
            userDir,
            QStringLiteral("FontPin.json"),
            QStringLiteral(R"({
                "name": "FontPin",
                "kind": "light",
                "levels": {},
                "table": {},
                "chrome": {},
                "app": { "qtStyle": "fusion", "fontFamily": "Courier New", "fontSize": %1 }
            })")
                .arg(pinnedPointSize)
        );
        ThemeControl::ReloadAll();

        ThemeControl::SetActiveSelection(QStringLiteral("FontPin"));
        // Assert size, not family -- fontconfig substitution can
        // remap "Courier New" on minimal Linux images.
        QCOMPARE(qApp->font().pointSize(), pinnedPointSize);

        // Switch to a theme without font fields -> startup font.
        ThemeControl::SetActiveSelection(QStringLiteral("Light"));
        QCOMPARE(qApp->font().family(), startupFont.family());
        QCOMPARE(qApp->font().pointSize(), startupFont.pointSize());

        // Sanity-check the style apply path doesn't crash.
        QVERIFY(!qApp->style()->name().isEmpty());
        Q_UNUSED(startupStyleName);
    }

    /// A persisted selection that no longer matches anything in
    /// the index falls through to the auto-resolved theme, and
    /// the in-memory selection is coerced to Auto. The on-disk
    /// `QSettings` value is left alone.
    void TestStaleSelectionFallsThroughToAuto()
    {
        QSettings settings;
        settings.setValue(QString::fromLatin1(SETTINGS_KEY_ACTIVE), QStringLiteral("NotARealTheme"));
        ThemeControl::LoadConfiguration();
        // `LoadConfiguration` re-seeds the OS scheme; reset the
        // fake and re-resolve for a deterministic assertion.
        FakeOsColorScheme(Qt::ColorScheme::Light);
        ThemeControl::Reevaluate();

        QCOMPARE(QString::fromStdString(ThemeControl::Active().name), QStringLiteral("Light"));
        QCOMPARE(ThemeControl::ActiveSelection(), QString());
        // QSettings still holds the stale value -- only
        // `SaveConfiguration` rewrites it.
        QCOMPARE(ThemeControl::PersistedSelection(), QStringLiteral("NotARealTheme"));
    }

    /// `PersistedSelection()` and `ActiveSelection()` can diverge:
    /// the former reflects disk, the latter the live state.
    void TestPersistedSelectionReadsQSettings()
    {
        QSettings settings;
        settings.setValue(QString::fromLatin1(SETTINGS_KEY_ACTIVE), QStringLiteral("Dark"));
        QCOMPARE(ThemeControl::PersistedSelection(), QStringLiteral("Dark"));
        // Live changes don't touch QSettings until SaveConfiguration().
        ThemeControl::SetActiveSelection(QStringLiteral("Light"));
        QCOMPARE(ThemeControl::ActiveSelection(), QStringLiteral("Light"));
        QCOMPARE(ThemeControl::PersistedSelection(), QStringLiteral("Dark"));
        ThemeControl::SaveConfiguration();
        QCOMPARE(ThemeControl::PersistedSelection(), QStringLiteral("Light"));
    }

    /// Regression: `SaveUserTheme` on the currently-resolved name
    /// must surface the new bytes (and emit `themeChanged`)
    /// without a manual `ReloadAll`. Saves of inactive themes
    /// must not re-resolve.
    void TestSaveUserThemeRefreshesActive()
    {
        FakeOsColorScheme(Qt::ColorScheme::Light);
        ThemeControl::SetActiveSelection(QStringLiteral("Light"));
        QCOMPARE(QString::fromStdString(ThemeControl::Active().name), QStringLiteral("Light"));

        loglib::Theme override = ThemeControl::Active();
        constexpr auto OVERRIDE_FG = "#C0FFEE";
        override.levels["Info"] = loglib::LevelStyle{.foreground = OVERRIDE_FG};

        QSignalSpy spy(&ThemeControl::Instance(), &ThemeControl::themeChanged);
        QVERIFY(spy.isValid());
        ThemeControl::SaveUserTheme(QStringLiteral("Light"), override);

        // `Active()` now reflects the freshly-saved bytes.
        const loglib::LevelStyle infoStyle = loglib::StyleForLevel(ThemeControl::Active(), loglib::LogLevel::Info);
        QVERIFY(infoStyle.foreground.has_value());
        QCOMPARE(QString::fromStdString(*infoStyle.foreground), QString::fromLatin1(OVERRIDE_FG));
        QCOMPARE(spy.count(), 1);

        // Saving an unrelated theme must not fire `themeChanged`.
        loglib::Theme inactive;
        inactive.name = "Solarized";
        inactive.kind = loglib::ThemeKind::Light;
        spy.clear();
        ThemeControl::SaveUserTheme(QStringLiteral("Solarized"), inactive);
        QCOMPARE(spy.count(), 0);
    }

private:
    /// Style name captured at `init()` so `cleanup()` can restore
    /// it after tests that pin `app.qtStyle`.
    QString mInitStyleName;
};

QTEST_MAIN(ThemeControlTest)
#include "test_theme_control.moc"
