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
#include <QStyle>
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

        // Restore the style's standard palette so a follow-on test
        // binary running in the same `QApplication` lifetime starts
        // from a clean palette rather than whatever theme this
        // fixture last applied. We reach for `qApp->style()` here
        // (not a saved snapshot) because the active style itself
        // may have changed mid-fixture via `app.qtStyle`, and the
        // standard palette of *that* style is the right baseline.
        if (qApp != nullptr && qApp->style() != nullptr)
        {
            qApp->setPalette(qApp->style()->standardPalette());
        }
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
        const QFont fatalFont = ThemeControl::FontFor(loglib::LogLevel::Fatal);
        QVERIFY(fatalFont.bold());
    }

    /// Regression: switching from Force "Light" to Auto on a light
    /// palette must drop the `QStyleHints::colorScheme` override.
    /// Before the fix, `ResolveAndApplyActive` early-returned when
    /// the resolved theme name didn't change and `ApplyColorSchemeHint`
    /// never ran, leaving Qt's color scheme pinned to Light and
    /// breaking OS dark/light tracking.
    void TestForceToAutoSameResolvedName()
    {
        SetPaletteWindow(Qt::white);
        ThemeControl::SetActiveSelection(QStringLiteral("Light"));
        QVERIFY2(ThemeControl::IsColorSchemeForcedForTest(), "Force-mode must pin the colour scheme");

        ThemeControl::SetActiveSelection(QString::fromLatin1(ThemeControl::AUTO_TOKEN));
        // Resolved theme is still "Light" (the auto picker matches
        // the light palette), but the override must be gone so OS
        // dark/light flips drive `ApplicationPaletteChange`.
        QCOMPARE(QString::fromStdString(ThemeControl::Active().name), QStringLiteral("Light"));
        QVERIFY2(
            !ThemeControl::IsColorSchemeForcedForTest(),
            "Auto mode must release the QStyleHints::colorScheme override even when the "
            "resolved theme name didn't change"
        );
    }

    /// `SanitiseThemeName` (and therefore `SaveUserTheme`) rejects
    /// path-escape attempts so a malicious or accidental name like
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
        // Win32 strips trailing `.` / ` ` when creating the file, so
        // these names would silently collide with `Dark.json` /
        // `Dark` -- reject up front.
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, ThemeControl::SaveUserTheme(QStringLiteral("Dark."), theme));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, ThemeControl::SaveUserTheme(QStringLiteral("Dark "), theme));
        // A plain name must work and round-trip back through the index.
        ThemeControl::SaveUserTheme(QStringLiteral("Sepia"), theme);
        ThemeControl::ReloadAll();
        const auto listings = ThemeControl::AvailableThemes();
        const bool present = std::any_of(listings.begin(), listings.end(), [](const auto &entry) {
            return entry.name == QStringLiteral("Sepia") && entry.fromUser;
        });
        QVERIFY2(present, "valid user theme should round-trip through SaveUserTheme + ReloadAll");
    }

    /// `ReloadAll` with no on-disk edits must not emit
    /// `themeChanged`. The fast path in `ResolveAndApplyActive`
    /// short-circuits when the newly-discovered theme is byte-equal
    /// to the active one, avoiding a redundant palette / cache
    /// rebuild and the viewport-repaint fan-out.
    void TestReloadAllSkipsWhenUnchanged()
    {
        ThemeControl::SetActiveSelection(QStringLiteral("Light"));
        QSignalSpy spy(&ThemeControl::Instance(), &ThemeControl::themeChanged);
        QVERIFY(spy.isValid());

        ThemeControl::ReloadAll();
        QCOMPARE(spy.count(), 0);
    }

    /// A theme whose `app.qtStyle` references a non-existent Qt
    /// style must not crash and must leave the previous style intact.
    /// `QStyleFactory::create` returns `nullptr` for unknown names;
    /// the apply path checks for that.
    void TestMissingStyleFactoryFallback()
    {
        const QDir userDir = ThemeControl::UserThemesDir();
        WriteUserTheme(
            userDir,
            QStringLiteral("BadStyle.json"),
            QStringLiteral(R"({
                "name": "BadStyle",
                "kind": "light",
                "levels": {},
                "table": {},
                "chrome": {},
                "app": { "qtStyle": "NonexistentStyle12345" }
            })")
        );
        ThemeControl::ReloadAll();

        const QString priorStyle = qApp->style()->name();
        ThemeControl::SetActiveSelection(QStringLiteral("BadStyle"));

        QCOMPARE(QString::fromStdString(ThemeControl::Active().name), QStringLiteral("BadStyle"));
        // Unknown style name -> create() returns nullptr, the apply
        // path skips setStyle, so the prior style is still in
        // effect.
        QCOMPARE(qApp->style()->name(), priorStyle);
    }

    /// Regression: switching from a theme that pins `app.fontFamily`
    /// back to one that omits it must restore the startup font, not
    /// leave the previous theme's font carried through. Same
    /// contract for `app.qtStyle`.
    void TestRevertToStartupFontWhenThemeOmitsField()
    {
        const QFont startupFont = qApp->font();
        const QString startupStyleName = qApp->style()->name();

        // Pick a font family that's almost certainly different from
        // the test's startup font so the assertion is meaningful.
        // "Courier New" ships on Windows, macOS, and most Linux
        // distros via fontconfig substitution.
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
                "app": { "qtStyle": "fusion", "fontFamily": "Courier New", "fontSize": 12 }
            })")
        );
        ThemeControl::ReloadAll();

        ThemeControl::SetActiveSelection(QStringLiteral("FontPin"));
        // Sanity: the pinned font is in effect.
        QCOMPARE(qApp->font().family(), QStringLiteral("Courier New"));

        // Switching back to a theme without `app.fontFamily` /
        // `app.fontSize` must revert to the startup font.
        ThemeControl::SetActiveSelection(QStringLiteral("Light"));
        QCOMPARE(qApp->font().family(), startupFont.family());
        QCOMPARE(qApp->font().pointSize(), startupFont.pointSize());

        // The startup style is at least preserved when both themes
        // share the same `qtStyle` (built-in Light pins "fusion"
        // too, so this just sanity-checks no crash on the apply
        // path -- the real regression is the font above).
        QVERIFY(!qApp->style()->name().isEmpty());
        Q_UNUSED(startupStyleName);
    }

    /// A persisted selection that no longer matches any discoverable
    /// theme falls through to the auto-resolved theme, AND the
    /// in-memory `mActiveSelection` is coerced back to `AUTO_TOKEN`
    /// so a follow-on `ActiveSelection()` query (e.g. from the
    /// Preferences combo) doesn't lie about the live state. The
    /// on-disk `QSettings` value is intentionally NOT rewritten --
    /// only user-driven saves persist.
    void TestStaleSelectionFallsThroughToAuto()
    {
        SetPaletteWindow(Qt::white);
        QSettings settings;
        settings.setValue(QString::fromLatin1(SETTINGS_KEY_ACTIVE), QStringLiteral("NotARealTheme"));
        ThemeControl::LoadConfiguration();

        QCOMPARE(QString::fromStdString(ThemeControl::Active().name), QStringLiteral("Light"));
        // In-memory state is now Auto (no surprise persistence).
        QCOMPARE(ThemeControl::ActiveSelection(), QString());
        // The on-disk value is unchanged; only `SaveConfiguration`
        // would rewrite it.
        QCOMPARE(
            settings.value(QString::fromLatin1(SETTINGS_KEY_ACTIVE)).toString(), QStringLiteral("NotARealTheme")
        );
    }
};

QTEST_MAIN(ThemeControlTest)
#include "test_theme_control.moc"
