// Unit tests for `ThemeControl`. Uses `QTEST_MAIN` so a real
// `QApplication` is alive and the production palette / style code
// path runs under test.

#include "theme_control.hpp"

#include <loglib/theme.hpp>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QScopedPointer>
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

/// Drop the persisted active selection so the next
/// `ThemeControl` constructor starts from Auto. Keeps tests
/// independent of CI worker profile state.
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

        // Construct a fresh ThemeControl per test. The ctor runs
        // discovery + apply, mirroring what `main()` does after
        // `QApplication`.
        mTheme.reset(new ThemeControl());

        // Override the cached OS scheme AFTER construction (which
        // captured the pristine value first).
        mTheme->SetOsColorSchemeForTest(Qt::ColorScheme::Light);
    }

    void cleanup()
    {
        // Tear down the per-test ThemeControl before restoring the
        // process-wide style: any pending signal connections drop
        // with it and can't fire during cleanup.
        mTheme.reset();

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
        // `QStyleHints` from the next test's Force/Auto state.
        // The next test's Auto path releases any leftover override
        // cleanly when it constructs its own ThemeControl.
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
        mTheme->SetOsColorSchemeForTest(Qt::ColorScheme::Light);
        mTheme->SetActiveSelection(QString::fromLatin1(ThemeControl::AUTO_TOKEN));
        mTheme->Reevaluate();

        QCOMPARE(QString::fromStdString(mTheme->Active().name), QStringLiteral("Light"));
        QCOMPARE(mTheme->Active().kind, loglib::ThemeKind::Light);
    }

    /// Auto + dark OS scheme -> built-in Dark.
    void TestAutoDarkPalettePicksDark()
    {
        mTheme->SetOsColorSchemeForTest(Qt::ColorScheme::Dark);
        mTheme->SetActiveSelection(QString::fromLatin1(ThemeControl::AUTO_TOKEN));
        mTheme->Reevaluate();

        QCOMPARE(QString::fromStdString(mTheme->Active().name), QStringLiteral("Dark"));
        QCOMPARE(mTheme->Active().kind, loglib::ThemeKind::Dark);
    }

    /// Explicit selection wins over the OS scheme.
    void TestExplicitSelectionOverridesPalette()
    {
        mTheme->SetOsColorSchemeForTest(Qt::ColorScheme::Light);
        mTheme->SetActiveSelection(QStringLiteral("Dark"));

        QCOMPARE(QString::fromStdString(mTheme->Active().name), QStringLiteral("Dark"));
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

        mTheme->ReloadAll();
        const loglib::Theme &active = mTheme->Active();
        QCOMPARE(QString::fromStdString(active.name), QStringLiteral("Light"));
        const loglib::LevelStyle infoStyle = loglib::StyleForLevel(active, loglib::LogLevel::Info);
        QVERIFY(infoStyle.foreground.has_value());
        QCOMPARE(QString::fromStdString(*infoStyle.foreground), QString::fromLatin1(OVERRIDE_FG));

        // The listing must mark the entry `fromUser` so the UI
        // can annotate it.
        const auto listings = mTheme->AvailableThemes();
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
        mTheme->ReloadAll();

        const auto listings = mTheme->AvailableThemes();
        const bool present = std::any_of(listings.begin(), listings.end(), [](const auto &entry) {
            return entry.name == QStringLiteral("Solarized") && entry.fromUser;
        });
        QVERIFY2(present, "user theme should appear in AvailableThemes()");

        // Auto still picks the built-in Light, not the new user
        // theme.
        mTheme->SetOsColorSchemeForTest(Qt::ColorScheme::Light);
        mTheme->SetActiveSelection(QString::fromLatin1(ThemeControl::AUTO_TOKEN));
        mTheme->Reevaluate();
        QCOMPARE(QString::fromStdString(mTheme->Active().name), QStringLiteral("Light"));

        // Explicit selection of the user theme works.
        mTheme->SetActiveSelection(QStringLiteral("Solarized"));
        QCOMPARE(QString::fromStdString(mTheme->Active().name), QStringLiteral("Solarized"));
    }

    /// A selection change that actually flips the resolved theme
    /// fires `themeChanged` exactly once.
    void TestSelectionChangeFiresSignal()
    {
        mTheme->SetActiveSelection(QStringLiteral("Light"));
        const QSignalSpy spy(mTheme.data(), &ThemeControl::themeChanged);
        QVERIFY(spy.isValid());

        mTheme->SetActiveSelection(QStringLiteral("Dark"));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(QString::fromStdString(mTheme->Active().name), QStringLiteral("Dark"));

        // Re-selecting the same theme is a no-op.
        mTheme->SetActiveSelection(QStringLiteral("Dark"));
        QCOMPARE(spy.count(), 1);
    }

    /// On the Light preset: Info defers to the palette, Error has
    /// a brush, Fatal is bold.
    void TestBrushLookups()
    {
        mTheme->SetActiveSelection(QStringLiteral("Light"));

        const QBrush infoFg = mTheme->ForegroundFor(loglib::LogLevel::Info);
        QCOMPARE(infoFg.style(), Qt::NoBrush);

        const QBrush errorFg = mTheme->ForegroundFor(loglib::LogLevel::Error);
        QVERIFY(errorFg.style() != Qt::NoBrush);
        QVERIFY(errorFg.color().isValid());

        const QFont fatalFont = mTheme->FontFor(loglib::LogLevel::Fatal);
        QVERIFY(fatalFont.bold());
    }

    /// Regression: Force "Light" -> Auto on a light OS must drop
    /// the `QStyleHints::colorScheme` override, even though the
    /// resolved theme name doesn't change.
    void TestForceToAutoSameResolvedName()
    {
        mTheme->SetOsColorSchemeForTest(Qt::ColorScheme::Light);
        mTheme->SetActiveSelection(QStringLiteral("Light"));
        QVERIFY2(mTheme->IsColorSchemeForcedForTest(), "Force-mode must pin the colour scheme");

        mTheme->SetActiveSelection(QString::fromLatin1(ThemeControl::AUTO_TOKEN));
        // Resolved theme stays Light, but the override must drop
        // so OS flips can drive `colorSchemeChanged`.
        QCOMPARE(QString::fromStdString(mTheme->Active().name), QStringLiteral("Light"));
        QVERIFY2(
            !mTheme->IsColorSchemeForcedForTest(),
            "Auto mode must release the QStyleHints::colorScheme override even when the "
            "resolved theme name didn't change"
        );
    }

    /// Regression: Force "Dark" -> Auto on a light OS must resolve
    /// to Light. The cross-kind variant of
    /// `TestForceToAutoSameResolvedName`.
    void TestForceDarkToAutoLightOsPicksLight()
    {
        mTheme->SetOsColorSchemeForTest(Qt::ColorScheme::Light);
        mTheme->SetActiveSelection(QStringLiteral("Dark"));
        QCOMPARE(QString::fromStdString(mTheme->Active().name), QStringLiteral("Dark"));
        QVERIFY(mTheme->IsColorSchemeForcedForTest());

        mTheme->SetActiveSelection(QString::fromLatin1(ThemeControl::AUTO_TOKEN));
        QCOMPARE(QString::fromStdString(mTheme->Active().name), QStringLiteral("Light"));
        QVERIFY2(!mTheme->IsColorSchemeForcedForTest(), "Auto mode must release the Force-mode colour-scheme override");
    }

    /// Symmetric regression: Force "Light" -> Auto on a dark OS
    /// must resolve to Dark.
    void TestForceLightToAutoDarkOsPicksDark()
    {
        mTheme->SetOsColorSchemeForTest(Qt::ColorScheme::Dark);
        mTheme->SetActiveSelection(QStringLiteral("Light"));
        QCOMPARE(QString::fromStdString(mTheme->Active().name), QStringLiteral("Light"));
        QVERIFY(mTheme->IsColorSchemeForcedForTest());

        mTheme->SetActiveSelection(QString::fromLatin1(ThemeControl::AUTO_TOKEN));
        QCOMPARE(QString::fromStdString(mTheme->Active().name), QStringLiteral("Dark"));
        QVERIFY(!mTheme->IsColorSchemeForcedForTest());
    }

    /// Regression: Force-Light on light OS -> OS flips to dark
    /// (suppressed by our override) -> back to Auto must resolve
    /// to Dark, not stick on Light.
    void TestForceModeOsFlipResolvesAutoCorrectly()
    {
        mTheme->SetOsColorSchemeForTest(Qt::ColorScheme::Light);
        mTheme->SetActiveSelection(QStringLiteral("Light"));
        QVERIFY(mTheme->IsColorSchemeForcedForTest());
        QCOMPARE(QString::fromStdString(mTheme->Active().name), QStringLiteral("Light"));

        // OS flips to dark while Force is held. In production our
        // slot would skip recording it; the fake setter encodes
        // the new OS state for the Auto-path re-sample.
        mTheme->SetOsColorSchemeForTest(Qt::ColorScheme::Dark);

        mTheme->SetActiveSelection(QString::fromLatin1(ThemeControl::AUTO_TOKEN));
        QCOMPARE(QString::fromStdString(mTheme->Active().name), QStringLiteral("Dark"));
        QVERIFY(!mTheme->IsColorSchemeForcedForTest());
    }

    /// Auto with `Qt::ColorScheme::Unknown` (minimal Linux CI with
    /// no platform theme) defaults to Light, not Dark.
    void TestAutoUnknownOsPicksLight()
    {
        mTheme->SetOsColorSchemeForTest(Qt::ColorScheme::Unknown);
        mTheme->SetActiveSelection(QString::fromLatin1(ThemeControl::AUTO_TOKEN));
        mTheme->Reevaluate();

        QCOMPARE(QString::fromStdString(mTheme->Active().name), QStringLiteral("Light"));
    }

    /// `SaveUserTheme` rejects unsafe names so a path like
    /// `../evil` can't write outside `UserThemesDir`.
    void TestSaveUserThemeRejectsBadName()
    {
        loglib::Theme theme;
        theme.name = "ignored";

        QVERIFY_THROWS_EXCEPTION(std::runtime_error, mTheme->SaveUserTheme(QStringLiteral("../evil"), theme));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, mTheme->SaveUserTheme(QStringLiteral("sub/dir/theme"), theme));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, mTheme->SaveUserTheme(QStringLiteral(""), theme));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, mTheme->SaveUserTheme(QStringLiteral("CON"), theme));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, mTheme->SaveUserTheme(QStringLiteral("nul"), theme));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, mTheme->SaveUserTheme(QStringLiteral(".."), theme));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, mTheme->SaveUserTheme(QStringLiteral("contains\nnewline"), theme));
        // Trailing `.` / ` ` would silently collide on Win32.
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, mTheme->SaveUserTheme(QStringLiteral("Dark."), theme));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, mTheme->SaveUserTheme(QStringLiteral("Dark "), theme));
        // A plain name must round-trip through the index.
        mTheme->SaveUserTheme(QStringLiteral("Sepia"), theme);
        mTheme->ReloadAll();
        const auto listings = mTheme->AvailableThemes();
        const bool present = std::any_of(listings.begin(), listings.end(), [](const auto &entry) {
            return entry.name == QStringLiteral("Sepia") && entry.fromUser;
        });
        QVERIFY2(present, "valid user theme should round-trip through SaveUserTheme + ReloadAll");
    }

    /// `ReloadAll` with no on-disk edits must not emit
    /// `themeChanged` (byte-equal fast-path).
    void TestReloadAllSkipsWhenUnchanged()
    {
        mTheme->SetActiveSelection(QStringLiteral("Light"));
        const QSignalSpy spy(mTheme.data(), &ThemeControl::themeChanged);
        QVERIFY(spy.isValid());

        mTheme->ReloadAll();
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
        mTheme->ReloadAll();

        const QString priorStyle = qApp->style()->name();
        mTheme->SetActiveSelection(QStringLiteral("BadStyle"));

        QCOMPARE(QString::fromStdString(mTheme->Active().name), QStringLiteral("BadStyle"));
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
        mTheme->ReloadAll();

        mTheme->SetActiveSelection(QStringLiteral("FontPin"));
        // Assert size, not family -- fontconfig substitution can
        // remap "Courier New" on minimal Linux images.
        QCOMPARE(qApp->font().pointSize(), pinnedPointSize);

        // Switch to a theme without font fields -> startup font.
        mTheme->SetActiveSelection(QStringLiteral("Light"));
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
        // Stash a stale value in QSettings, then construct a new
        // ThemeControl so the bad selection runs through the load
        // path. The fixture's `mTheme` is replaced; `cleanup()`
        // tears down whichever instance is live.
        QSettings settings;
        settings.setValue(QString::fromLatin1(SETTINGS_KEY_ACTIVE), QStringLiteral("NotARealTheme"));
        mTheme.reset(new ThemeControl());
        // Construction re-seeds the OS scheme; reset the fake and
        // re-resolve for a deterministic assertion.
        mTheme->SetOsColorSchemeForTest(Qt::ColorScheme::Light);
        mTheme->Reevaluate();

        QCOMPARE(QString::fromStdString(mTheme->Active().name), QStringLiteral("Light"));
        QCOMPARE(mTheme->ActiveSelection(), QString());
        // QSettings still holds the stale value -- only
        // `SaveConfiguration` rewrites it.
        QCOMPARE(mTheme->PersistedSelection(), QStringLiteral("NotARealTheme"));
    }

    /// `PersistedSelection()` and `ActiveSelection()` can diverge:
    /// the former reflects disk, the latter the live state.
    void TestPersistedSelectionReadsQSettings()
    {
        QSettings settings;
        settings.setValue(QString::fromLatin1(SETTINGS_KEY_ACTIVE), QStringLiteral("Dark"));
        QCOMPARE(mTheme->PersistedSelection(), QStringLiteral("Dark"));
        // Live changes don't touch QSettings until SaveConfiguration().
        mTheme->SetActiveSelection(QStringLiteral("Light"));
        QCOMPARE(mTheme->ActiveSelection(), QStringLiteral("Light"));
        QCOMPARE(mTheme->PersistedSelection(), QStringLiteral("Dark"));
        mTheme->SaveConfiguration();
        QCOMPARE(mTheme->PersistedSelection(), QStringLiteral("Light"));
    }

    /// Regression: `SaveUserTheme` on the currently-resolved name
    /// must surface the new bytes (and emit `themeChanged`)
    /// without a manual `ReloadAll`. Saves of inactive themes
    /// must not re-resolve.
    void TestSaveUserThemeRefreshesActive()
    {
        mTheme->SetOsColorSchemeForTest(Qt::ColorScheme::Light);
        mTheme->SetActiveSelection(QStringLiteral("Light"));
        QCOMPARE(QString::fromStdString(mTheme->Active().name), QStringLiteral("Light"));

        loglib::Theme override = mTheme->Active();
        constexpr auto OVERRIDE_FG = "#C0FFEE";
        override.levels["Info"] = loglib::LevelStyle{.foreground = OVERRIDE_FG};

        QSignalSpy spy(mTheme.data(), &ThemeControl::themeChanged);
        QVERIFY(spy.isValid());
        mTheme->SaveUserTheme(QStringLiteral("Light"), override);

        // `Active()` now reflects the freshly-saved bytes.
        const loglib::LevelStyle infoStyle = loglib::StyleForLevel(mTheme->Active(), loglib::LogLevel::Info);
        QVERIFY(infoStyle.foreground.has_value());
        QCOMPARE(QString::fromStdString(*infoStyle.foreground), QString::fromLatin1(OVERRIDE_FG));
        QCOMPARE(spy.count(), 1);

        // Saving an unrelated theme must not fire `themeChanged`.
        loglib::Theme inactive;
        inactive.name = "Solarized";
        inactive.kind = loglib::ThemeKind::Light;
        spy.clear();
        mTheme->SaveUserTheme(QStringLiteral("Solarized"), inactive);
        QCOMPARE(spy.count(), 0);
    }

    /// Built-in themes all ship `levelColumnOverride`, so picking
    /// one flips `HasLevelColumnOverride()` on and populates the
    /// per-level caches. Regression for the headline icon-mode
    /// switch: every consumer reads this single bool.
    void TestBuiltinThemeOptsIntoIconMode()
    {
        mTheme->SetActiveSelection(QStringLiteral("Dark"));

        QVERIFY(mTheme->HasLevelColumnOverride());
        // Built-in dark.json sets icons for all seven levels.
        QVERIFY(!mTheme->IconFor(loglib::LogLevel::Info).isNull());
        QVERIFY(!mTheme->IconFor(loglib::LogLevel::Warn).isNull());
        QVERIFY(!mTheme->IconFor(loglib::LogLevel::Fatal).isNull());
        QVERIFY(!mTheme->IconFor(loglib::LogLevel::Unknown).isNull());
        // Built-in dark.json sets a pillBackground for Info..Fatal
        // but omits it for Trace/Debug/Unknown.
        QVERIFY(mTheme->PillBackgroundFor(loglib::LogLevel::Info).style() != Qt::NoBrush);
        QVERIFY(mTheme->PillBackgroundFor(loglib::LogLevel::Warn).style() != Qt::NoBrush);
        QVERIFY(mTheme->PillBackgroundFor(loglib::LogLevel::Trace).style() == Qt::NoBrush);
        QVERIFY(mTheme->PillBackgroundFor(loglib::LogLevel::Unknown).style() == Qt::NoBrush);
        // The pill foreground always resolves (one of the three
        // fallbacks fires) when icon mode is on.
        QVERIFY(mTheme->PillForegroundFor(loglib::LogLevel::Info).style() != Qt::NoBrush);
        // Built-in dark.json doesn't override header *text*; it does
        // ship a `headerIcon` (Lucide gauge) so the level column has
        // a generic identifier in icon mode that's distinct from the
        // per-level glyphs.
        QVERIFY(!mTheme->LevelColumnHeaderTextOverride().has_value());
        QVERIFY(!mTheme->LevelColumnHeaderIcon().isNull());
    }

    /// A theme that omits `levelColumnOverride` keeps icon mode
    /// off, with all per-level caches cleared.
    void TestUserThemeWithoutOverrideTurnsIconModeOff()
    {
        const QDir userDir = ThemeControl::UserThemesDir();
        WriteUserTheme(userDir, QStringLiteral("plain.json"), QStringLiteral(R"({
                "name": "Plain",
                "kind": "light",
                "levels": { "Info": { "foreground": "#222222" } },
                "table": {},
                "chrome": {},
                "app": {}
            })"));
        mTheme->ReloadAll();
        mTheme->SetActiveSelection(QStringLiteral("Plain"));

        QVERIFY(!mTheme->HasLevelColumnOverride());
        QVERIFY(mTheme->IconFor(loglib::LogLevel::Info).isNull());
        QVERIFY(mTheme->PillBackgroundFor(loglib::LogLevel::Info).style() == Qt::NoBrush);
        QVERIFY(!mTheme->LevelColumnHeaderTextOverride().has_value());
        QVERIFY(mTheme->LevelColumnHeaderIcon().isNull());
    }

    /// Switching from an icon-mode theme to a plain theme must
    /// scrub the per-level caches; otherwise the next paint would
    /// still resolve stale icons.
    void TestSwitchAwayFromIconThemeClearsCaches()
    {
        mTheme->SetActiveSelection(QStringLiteral("Dark"));
        QVERIFY(mTheme->HasLevelColumnOverride());
        QVERIFY(!mTheme->IconFor(loglib::LogLevel::Info).isNull());

        const QDir userDir = ThemeControl::UserThemesDir();
        WriteUserTheme(userDir, QStringLiteral("plain.json"), QStringLiteral(R"({
                "name": "Plain",
                "kind": "light",
                "levels": {},
                "table": {},
                "chrome": {},
                "app": {}
            })"));
        mTheme->ReloadAll();
        mTheme->SetActiveSelection(QStringLiteral("Plain"));

        QVERIFY(!mTheme->HasLevelColumnOverride());
        QVERIFY(mTheme->IconFor(loglib::LogLevel::Info).isNull());
        QVERIFY(mTheme->PillBackgroundFor(loglib::LogLevel::Warn).style() == Qt::NoBrush);
    }

    /// `header: ""` is a legitimate override -- it means "render
    /// no header text" -- and must round-trip through the runtime
    /// caches as a present-but-empty optional.
    void TestThemeHeaderOverrideRoundTripIncludingEmptyString()
    {
        const QDir userDir = ThemeControl::UserThemesDir();
        WriteUserTheme(userDir, QStringLiteral("blank_header.json"), QStringLiteral(R"({
                "name": "BlankHeader",
                "kind": "light",
                "levels": {},
                "table": {},
                "chrome": {},
                "app": {},
                "levelColumnOverride": {
                    "header": "",
                    "headerIcon": ":/icons/level-info.svg",
                    "levels": {
                        "Info": { "icon": ":/icons/level-info.svg" }
                    }
                }
            })"));
        mTheme->ReloadAll();
        mTheme->SetActiveSelection(QStringLiteral("BlankHeader"));

        QVERIFY(mTheme->HasLevelColumnOverride());
        const std::optional<QString> headerText = mTheme->LevelColumnHeaderTextOverride();
        QVERIFY(headerText.has_value());
        QVERIFY(headerText->isEmpty());
        QVERIFY(!mTheme->LevelColumnHeaderIcon().isNull());
    }

    /// User theme with a `..` path traversal in its icon path must
    /// be rejected at cache-build time. The level still flips to
    /// icon-mode-on (because the override block is present), but
    /// `IconFor` returns a null icon -- which `LogModel` handles
    /// as "no icon for this level".
    void TestThemeControlRejectsParentPathTraversal()
    {
        const QDir userDir = ThemeControl::UserThemesDir();
        WriteUserTheme(userDir, QStringLiteral("traversal.json"), QStringLiteral(R"({
                "name": "Traversal",
                "kind": "light",
                "levels": {},
                "table": {},
                "chrome": {},
                "app": {},
                "levelColumnOverride": {
                    "levels": {
                        "Info":  { "icon": "../../../etc/passwd" },
                        "Warn":  { "icon": ":/icons/level-warn.svg" }
                    }
                }
            })"));
        mTheme->ReloadAll();
        mTheme->SetActiveSelection(QStringLiteral("Traversal"));

        QVERIFY(mTheme->HasLevelColumnOverride());
        // Traversal path was rejected -> null icon. Sibling level
        // with a clean path still resolves.
        QVERIFY(mTheme->IconFor(loglib::LogLevel::Info).isNull());
        QVERIFY(!mTheme->IconFor(loglib::LogLevel::Warn).isNull());
    }

    /// `QDir::cleanPath` normalises Windows-style backslash
    /// separators to forward slashes before the traversal guard
    /// inspects the path, so a `..\..\` escape is caught the same
    /// way `../../` is. This pins the cross-platform behaviour
    /// so a future refactor that swaps in a different cleanup
    /// helper can't silently regress on Windows.
    void TestThemeControlRejectsBackslashTraversal()
    {
        const QDir userDir = ThemeControl::UserThemesDir();
        WriteUserTheme(userDir, QStringLiteral("traversal_backslash.json"), QStringLiteral(R"({
                "name": "TraversalBackslash",
                "kind": "light",
                "levels": {},
                "table": {},
                "chrome": {},
                "app": {},
                "levelColumnOverride": {
                    "levels": {
                        "Info":  { "icon": "..\\..\\..\\etc\\passwd" },
                        "Warn":  { "icon": ":/icons/level-warn.svg" }
                    }
                }
            })"));
        mTheme->ReloadAll();
        mTheme->SetActiveSelection(QStringLiteral("TraversalBackslash"));

        QVERIFY(mTheme->HasLevelColumnOverride());
        QVERIFY(mTheme->IconFor(loglib::LogLevel::Info).isNull());
        QVERIFY(!mTheme->IconFor(loglib::LogLevel::Warn).isNull());
    }

    /// A bare `..` segment (no separator) must also be rejected:
    /// `QDir::cleanPath("..")` returns `..` unchanged, which the
    /// guard catches via the explicit equality check.
    void TestThemeControlRejectsBareParentSegment()
    {
        const QDir userDir = ThemeControl::UserThemesDir();
        WriteUserTheme(userDir, QStringLiteral("traversal_bare.json"), QStringLiteral(R"({
                "name": "TraversalBare",
                "kind": "light",
                "levels": {},
                "table": {},
                "chrome": {},
                "app": {},
                "levelColumnOverride": {
                    "levels": {
                        "Info":  { "icon": ".." },
                        "Warn":  { "icon": ":/icons/level-warn.svg" }
                    }
                }
            })"));
        mTheme->ReloadAll();
        mTheme->SetActiveSelection(QStringLiteral("TraversalBare"));

        QVERIFY(mTheme->HasLevelColumnOverride());
        QVERIFY(mTheme->IconFor(loglib::LogLevel::Info).isNull());
        QVERIFY(!mTheme->IconFor(loglib::LogLevel::Warn).isNull());
    }

    /// Every built-in theme listed in the index must parse cleanly,
    /// ship a non-empty ``levels`` map, and (post subtle-default
    /// rework) carry a non-empty ``levelsHighContrast`` map. Catches
    /// typos in the new theme JSONs at test time instead of when a
    /// user picks a theme that fails to load.
    void TestThemeControlBuiltInThemesAreWellFormed()
    {
        const auto listings = mTheme->AvailableThemes();
        // The exact count is intentionally not pinned (so adding
        // another built-in doesn't require an unrelated test edit),
        // but we expect at least the 15 named themes after the
        // gallery expansion.
        QVERIFY(listings.size() >= 15);

        const QStringList expected = {
            QStringLiteral("Light"),
            QStringLiteral("Dark"),
            QStringLiteral("GitHub Dark"),
            QStringLiteral("GitHub Light"),
            QStringLiteral("Material Dark"),
            QStringLiteral("Material Light"),
            QStringLiteral("Monokai Dark"),
            QStringLiteral("Monokai Light"),
            QStringLiteral("Solarized Dark"),
            QStringLiteral("Solarized Light"),
            QStringLiteral("Nord"),
            QStringLiteral("Dracula"),
            QStringLiteral("Tokyo Night"),
            QStringLiteral("Catppuccin Mocha"),
            QStringLiteral("Paper Light"),
        };
        for (const QString &name : expected)
        {
            const auto loaded = mTheme->Load(name);
            QVERIFY2(loaded.has_value(), qUtf8Printable(QStringLiteral("missing built-in: ") + name));
            QVERIFY2(!loaded->levels.empty(), qUtf8Printable(QStringLiteral("built-in has empty levels: ") + name));
            QVERIFY2(
                !loaded->levelsHighContrast.empty(),
                qUtf8Printable(QStringLiteral("built-in has empty levelsHighContrast: ") + name)
            );
        }
    }

    /// `SetHighContrast(true)` projects `levelsHighContrast` brushes
    /// over the subtle defaults; `false` restores them. Toggling to
    /// the same value is a no-op (no signal, no rebuild).
    void TestThemeControlHighContrastTogglesBrushes()
    {
        const QDir userDir = ThemeControl::UserThemesDir();
        WriteUserTheme(userDir, QStringLiteral("highContrast.json"), QStringLiteral(R"({
                "name": "HighContrastFixture",
                "kind": "dark",
                "levels": {
                    "Error": { "foreground": "#FCA5A5", "background": "#352121" }
                },
                "levelsHighContrast": {
                    "Error": { "foreground": "#FCA5A5", "background": "#4C1D1D" }
                },
                "table": {},
                "chrome": {},
                "app": {}
            })"));
        mTheme->ReloadAll();
        mTheme->SetActiveSelection(QStringLiteral("HighContrastFixture"));
        // Start from a known default.
        mTheme->SetHighContrast(false);

        QVERIFY(mTheme->HasLevelsHighContrast());
        QVERIFY(!mTheme->IsHighContrast());
        QCOMPARE(mTheme->BackgroundFor(loglib::LogLevel::Error).color(), QColor(QStringLiteral("#352121")));

        QSignalSpy spy(mTheme.get(), &ThemeControl::themeChanged);
        mTheme->SetHighContrast(true);
        QCOMPARE(spy.count(), 1);
        QVERIFY(mTheme->IsHighContrast());
        QCOMPARE(mTheme->BackgroundFor(loglib::LogLevel::Error).color(), QColor(QStringLiteral("#4C1D1D")));

        // Idempotent: same value -> no signal, no rebuild.
        mTheme->SetHighContrast(true);
        QCOMPARE(spy.count(), 1);

        mTheme->SetHighContrast(false);
        QCOMPARE(spy.count(), 2);
        QCOMPARE(mTheme->BackgroundFor(loglib::LogLevel::Error).color(), QColor(QStringLiteral("#352121")));
    }

    /// Sparse `levelsHighContrast` falls back to `levels` per-level:
    /// only the overridden entries change when the toggle is on.
    void TestThemeControlHighContrastSparseFallback()
    {
        const QDir userDir = ThemeControl::UserThemesDir();
        WriteUserTheme(userDir, QStringLiteral("sparseContrast.json"), QStringLiteral(R"({
                "name": "SparseContrast",
                "kind": "dark",
                "levels": {
                    "Warn":  { "foreground": "#FCD34D", "background": "#272620" },
                    "Error": { "foreground": "#FCA5A5", "background": "#352121" }
                },
                "levelsHighContrast": {
                    "Error": { "foreground": "#FCA5A5", "background": "#4C1D1D" }
                },
                "table": {},
                "chrome": {},
                "app": {}
            })"));
        mTheme->ReloadAll();
        mTheme->SetActiveSelection(QStringLiteral("SparseContrast"));
        mTheme->SetHighContrast(true);

        // `Error` flips to the override.
        QCOMPARE(mTheme->BackgroundFor(loglib::LogLevel::Error).color(), QColor(QStringLiteral("#4C1D1D")));
        // `Warn` is absent from the override map -- keep the subtle bg.
        QCOMPARE(mTheme->BackgroundFor(loglib::LogLevel::Warn).color(), QColor(QStringLiteral("#272620")));
    }

    /// A theme that omits `levelsHighContrast` reports the empty
    /// map via `HasLevelsHighContrast()`; toggling the pref is then
    /// a no-op for brush resolution AND must skip the
    /// `themeChanged()` cascade entirely (no full table repaint /
    /// no QSS re-stamp on widgets that listen on the signal). The
    /// fast-path in `SetHighContrast` is what makes the
    /// preferences-dialog "grey out the checkbox" UX coherent: the
    /// flag stays in sync for a later theme switch that DOES ship
    /// the block, but the user sees nothing in the meantime.
    void TestThemeControlHighContrastEmptyMap()
    {
        const QDir userDir = ThemeControl::UserThemesDir();
        WriteUserTheme(userDir, QStringLiteral("noContrast.json"), QStringLiteral(R"({
                "name": "NoContrast",
                "kind": "dark",
                "levels": {
                    "Error": { "foreground": "#FCA5A5", "background": "#352121" }
                },
                "table": {},
                "chrome": {},
                "app": {}
            })"));
        mTheme->ReloadAll();
        mTheme->SetActiveSelection(QStringLiteral("NoContrast"));

        QVERIFY(!mTheme->HasLevelsHighContrast());

        mTheme->SetHighContrast(false);
        const QColor before = mTheme->BackgroundFor(loglib::LogLevel::Error).color();
        // Spy attached AFTER the initial state-flip so the
        // potential rebuild from `SetActiveSelection` above doesn't
        // pollute the count. We're only interested in what
        // `SetHighContrast` does once the active theme is settled.
        QSignalSpy themeChangedSpy(mTheme.data(), &ThemeControl::themeChanged);
        QVERIFY(themeChangedSpy.isValid());
        mTheme->SetHighContrast(true);
        const QColor after = mTheme->BackgroundFor(loglib::LogLevel::Error).color();
        QCOMPARE(before, after);
        QCOMPARE(themeChangedSpy.count(), 0);
        // Flag still flipped so a later theme switch picks it up.
        QVERIFY(mTheme->IsHighContrast());
        // Toggling back: same fast-path, still no emit.
        mTheme->SetHighContrast(false);
        QCOMPARE(themeChangedSpy.count(), 0);
        QVERIFY(!mTheme->IsHighContrast());
    }

    /// A `:/` Qt resource path is allowed for both built-in and
    /// user themes; the qrc namespace is shared. Pin both halves
    /// (built-in + user) so a refactor can't silently lock user
    /// themes out of the resource namespace.
    void TestThemeControlAcceptsQtResourcePath()
    {
        const QDir userDir = ThemeControl::UserThemesDir();
        WriteUserTheme(userDir, QStringLiteral("resource_path.json"), QStringLiteral(R"({
                "name": "ResourcePath",
                "kind": "light",
                "levels": {},
                "table": {},
                "chrome": {},
                "app": {},
                "levelColumnOverride": {
                    "levels": {
                        "Info":  { "icon": ":/icons/level-info.svg" },
                        "Warn":  { "icon": ":/icons/level-warn.svg" }
                    }
                }
            })"));
        mTheme->ReloadAll();
        mTheme->SetActiveSelection(QStringLiteral("ResourcePath"));

        QVERIFY(mTheme->HasLevelColumnOverride());
        QVERIFY(!mTheme->IconFor(loglib::LogLevel::Info).isNull());
        QVERIFY(!mTheme->IconFor(loglib::LogLevel::Warn).isNull());
    }

private:
    /// Style name captured at `init()` so `cleanup()` can restore
    /// it after tests that pin `app.qtStyle`.
    QString mInitStyleName;

    /// Per-test theme controller, owned by the fixture. Replaces
    /// the previous singleton-based pattern: each test starts with
    /// a freshly-discovered theme index and clean state, and the
    /// `cleanup()` reset guarantees no signal connections survive
    /// into the next test.
    QScopedPointer<ThemeControl> mTheme;
};

QTEST_MAIN(ThemeControlTest)
#include "test_theme_control.moc"
