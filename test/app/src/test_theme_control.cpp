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
#include <Qt>
#include <QtTest/QtTest>

namespace
{

constexpr char SETTINGS_KEY_ACTIVE[] = "theme/active";

/// Pin the perceived OS colour scheme for the next Auto resolution.
/// Tests run on a wide variety of host platforms (Windows desktop,
/// minimal Linux CI image with no platform theme), so we cannot
/// rely on `QStyleHints::colorScheme()` returning a deterministic
/// value. `ThemeControl::SetOsColorSchemeForTest` writes the
/// cached `mOsColorScheme` directly, bypassing the
/// `setColorScheme()` path (which would also engage Force-mode
/// bookkeeping).
void FakeOsColorScheme(Qt::ColorScheme scheme)
{
    ThemeControl::SetOsColorSchemeForTest(scheme);
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
        // Each test starts from a known faked OS colour scheme
        // (Light) and an empty user-themes folder so cases stay
        // independent. `LoadConfiguration` re-reads `theme/active`
        // from QSettings (now empty -> Auto) AND re-discovers themes,
        // so the in-memory `mActiveSelection` from a previous test
        // does not leak through. Capture the style on entry so the
        // matching `cleanup()` can restore it (some tests pin
        // `app.qtStyle: "fusion"` via theme JSON).
        if (qApp != nullptr && qApp->style() != nullptr)
        {
            mInitStyleName = qApp->style()->name();
        }
        ClearActiveSelection();
        RemoveAllUserThemes(ThemeControl::UserThemesDir());
        ThemeControl::LoadConfiguration();
        // Seed AFTER LoadConfiguration: the first call captures the
        // pristine OS scheme into `mOsColorScheme` and we then
        // override it for the test. Force-mode tests further down
        // also call `FakeOsColorScheme` to flip the perceived OS
        // mid-test.
        FakeOsColorScheme(Qt::ColorScheme::Light);
    }

    void cleanup()
    {
        // Restore the entry-time style so a follow-on test that
        // never names `qtStyle` doesn't inherit the previous
        // test's fusion override. `cleanupTestCase` does the same
        // for the palette / standard palette; we mirror the
        // contract per test so any single case runs in isolation.
        if (qApp != nullptr && !mInitStyleName.isEmpty() && qApp->style() != nullptr &&
            qApp->style()->name().compare(mInitStyleName, Qt::CaseInsensitive) != 0)
        {
            if (QStyle *style = QStyleFactory::create(mInitStyleName); style != nullptr)
            {
                qApp->setStyle(style);
            }
        }
        // We deliberately do NOT call `unsetColorScheme()` here even
        // though tests may leave a Force-mode override active --
        // doing so would desync `QStyleHints` from the singleton's
        // `mColorSchemeForced` flag. The next test's `init()` ->
        // `LoadConfiguration` -> `ResolveAndApplyActive` Auto-path
        // releases the override through the singleton's own
        // bookkeeping, which keeps both sides in sync.
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

        // Auto resolution is unchanged: a light OS picks the
        // built-in Light, not the new user theme.
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
    /// OS must drop the `QStyleHints::colorScheme` override. Before
    /// the fix, `ResolveAndApplyActive` early-returned when the
    /// resolved theme name didn't change and `ApplyColorSchemeHint`
    /// never ran, leaving Qt's color scheme pinned to Light and
    /// breaking OS dark/light tracking.
    void TestForceToAutoSameResolvedName()
    {
        FakeOsColorScheme(Qt::ColorScheme::Light);
        ThemeControl::SetActiveSelection(QStringLiteral("Light"));
        QVERIFY2(ThemeControl::IsColorSchemeForcedForTest(), "Force-mode must pin the colour scheme");

        ThemeControl::SetActiveSelection(QString::fromLatin1(ThemeControl::AUTO_TOKEN));
        // Resolved theme is still "Light" (the auto picker matches
        // the light OS scheme), but the override must be gone so OS
        // dark/light flips drive `colorSchemeChanged`.
        QCOMPARE(QString::fromStdString(ThemeControl::Active().name), QStringLiteral("Light"));
        QVERIFY2(
            !ThemeControl::IsColorSchemeForcedForTest(),
            "Auto mode must release the QStyleHints::colorScheme override even when the "
            "resolved theme name didn't change"
        );
    }

    /// Regression: switching from Force "Dark" to Auto on a light
    /// OS must resolve to Light, NOT Dark. Before the fix,
    /// `IsDarkPalette()` sampled `qApp->palette().color(Window)`
    /// which was the Dark theme palette we had just pushed via
    /// `ApplyPalette` -- so the Auto picker re-resolved to Dark
    /// and the early-return path in `ResolveAndApplyActive`
    /// (resolved name unchanged) only released the colour-scheme
    /// override without ever re-applying the right theme. Pins
    /// the cross-brightness Force->Auto contract that the
    /// `TestForceToAutoSameResolvedName` sibling test couldn't
    /// exercise (it forced the same kind as the OS palette).
    void TestForceDarkToAutoLightOsPicksLight()
    {
        FakeOsColorScheme(Qt::ColorScheme::Light);
        ThemeControl::SetActiveSelection(QStringLiteral("Dark"));
        QCOMPARE(QString::fromStdString(ThemeControl::Active().name), QStringLiteral("Dark"));
        QVERIFY(ThemeControl::IsColorSchemeForcedForTest());

        ThemeControl::SetActiveSelection(QString::fromLatin1(ThemeControl::AUTO_TOKEN));
        QCOMPARE(QString::fromStdString(ThemeControl::Active().name), QStringLiteral("Light"));
        QVERIFY2(
            !ThemeControl::IsColorSchemeForcedForTest(),
            "Auto mode must release the Force-mode colour-scheme override"
        );
    }

    /// Symmetric regression: Force "Light" -> Auto on a dark OS
    /// must resolve to Dark. Same root cause as the test above,
    /// inverted.
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

    /// Regression for bug "Force-mode + OS flip + back to Auto picks
    /// the wrong kind": when the user is in Force-Light on a light
    /// OS, then the OS flips to dark while we're still in Force
    /// mode (Qt suppresses `colorSchemeChanged` because our override
    /// pins the value), and then the user switches to Auto, the
    /// resolved theme must be Dark. The fix snapshots the OS scheme
    /// into `mOsColorScheme` BEFORE engaging the override, then
    /// re-samples after `unsetColorScheme()` in the Auto path -- so
    /// the cache is whatever the OS actually is at the time of the
    /// transition.
    void TestForceModeOsFlipResolvesAutoCorrectly()
    {
        FakeOsColorScheme(Qt::ColorScheme::Light);
        ThemeControl::SetActiveSelection(QStringLiteral("Light"));
        QVERIFY(ThemeControl::IsColorSchemeForcedForTest());
        // Force "Light" while OS is light. mOsColorScheme should
        // still be Light at this point.
        QCOMPARE(QString::fromStdString(ThemeControl::Active().name), QStringLiteral("Light"));

        // OS flips to dark. In production Qt would fire
        // `colorSchemeChanged(Dark)` but our slot would skip
        // recording because `mColorSchemeForced` is true. The OS
        // state lives "elsewhere" -- in production, in the
        // platform theme; here, we encode it via the test setter
        // so the Auto-path re-sample finds it.
        FakeOsColorScheme(Qt::ColorScheme::Dark);

        // User switches back to Auto. The Auto picker must
        // re-resolve to Dark, not stick on Light.
        ThemeControl::SetActiveSelection(QString::fromLatin1(ThemeControl::AUTO_TOKEN));
        QCOMPARE(QString::fromStdString(ThemeControl::Active().name), QStringLiteral("Dark"));
        QVERIFY(!ThemeControl::IsColorSchemeForcedForTest());
    }

    /// Auto with no OS preference (`Qt::ColorScheme::Unknown` --
    /// minimal Linux CI image without a platform theme) defaults to
    /// Light. Pins that the fallback isn't accidentally Dark, which
    /// would surprise users on bare-bones Linux desktops.
    void TestAutoUnknownOsPicksLight()
    {
        FakeOsColorScheme(Qt::ColorScheme::Unknown);
        ThemeControl::SetActiveSelection(QString::fromLatin1(ThemeControl::AUTO_TOKEN));
        ThemeControl::Reevaluate();

        QCOMPARE(QString::fromStdString(ThemeControl::Active().name), QStringLiteral("Light"));
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

        // Pick a font family that resolves to something distinct
        // from the startup family on the host. We bump the
        // point-size deliberately too, so the revert assertion is
        // observable even on a CI runner whose font substitution
        // happens to land on the startup font's family.
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
        // Sanity: the pinned point size is in effect. We assert on
        // the size rather than the family because fontconfig
        // substitution on minimal Linux CI images can land on a
        // different family than "Courier New" -- the size is the
        // unambiguous signal that the theme's font field flowed
        // through.
        QCOMPARE(qApp->font().pointSize(), pinnedPointSize);

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
        QSettings settings;
        settings.setValue(QString::fromLatin1(SETTINGS_KEY_ACTIVE), QStringLiteral("NotARealTheme"));
        ThemeControl::LoadConfiguration();
        // LoadConfiguration re-seeds mOsColorScheme from the host
        // platform, which is unpredictable on CI. Reset the fake
        // and re-resolve so the assertion below is deterministic.
        FakeOsColorScheme(Qt::ColorScheme::Light);
        ThemeControl::Reevaluate();

        QCOMPARE(QString::fromStdString(ThemeControl::Active().name), QStringLiteral("Light"));
        // In-memory state is now Auto (no surprise persistence).
        QCOMPARE(ThemeControl::ActiveSelection(), QString());
        // `PersistedSelection()` still reports the stale value -- only
        // `SaveConfiguration` would rewrite it. This is the public
        // accessor that the Preferences `Cancel` handler consults.
        QCOMPARE(ThemeControl::PersistedSelection(), QStringLiteral("NotARealTheme"));
    }

    /// `PersistedSelection()` and `ActiveSelection()` can disagree --
    /// the former is what's on disk, the latter is what's live.
    void TestPersistedSelectionReadsQSettings()
    {
        QSettings settings;
        settings.setValue(QString::fromLatin1(SETTINGS_KEY_ACTIVE), QStringLiteral("Dark"));
        QCOMPARE(ThemeControl::PersistedSelection(), QStringLiteral("Dark"));
        // Changing the live selection does NOT touch QSettings until
        // a SaveConfiguration() call.
        ThemeControl::SetActiveSelection(QStringLiteral("Light"));
        QCOMPARE(ThemeControl::ActiveSelection(), QStringLiteral("Light"));
        QCOMPARE(ThemeControl::PersistedSelection(), QStringLiteral("Dark"));
        // SaveConfiguration commits the live value.
        ThemeControl::SaveConfiguration();
        QCOMPARE(ThemeControl::PersistedSelection(), QStringLiteral("Light"));
    }

    /// Regression for the `SaveUserTheme` staleness bug: when the
    /// just-saved theme name matches the currently-resolved one,
    /// `Active()` must return the freshly-saved bytes (and
    /// `themeChanged` must fire) without a follow-up `ReloadAll`.
    /// Saving an *inactive* theme must not re-resolve so unrelated
    /// edits don't surprise-emit the signal.
    void TestSaveUserThemeRefreshesActive()
    {
        // Build a baseline that activates the built-in Light, then
        // save a user "Light" with a distinctive override colour.
        FakeOsColorScheme(Qt::ColorScheme::Light);
        ThemeControl::SetActiveSelection(QStringLiteral("Light"));
        QCOMPARE(QString::fromStdString(ThemeControl::Active().name), QStringLiteral("Light"));

        loglib::Theme override = ThemeControl::Active();
        constexpr auto OVERRIDE_FG = "#C0FFEE";
        override.levels["Info"] = loglib::LevelStyle{.foreground = OVERRIDE_FG};

        QSignalSpy spy(&ThemeControl::Instance(), &ThemeControl::themeChanged);
        QVERIFY(spy.isValid());
        ThemeControl::SaveUserTheme(QStringLiteral("Light"), override);

        // `Active()` is now the freshly-saved bytes, not a stale
        // copy of the pre-save built-in.
        const loglib::LevelStyle infoStyle = loglib::StyleForLevel(ThemeControl::Active(), loglib::LogLevel::Info);
        QVERIFY(infoStyle.foreground.has_value());
        QCOMPARE(QString::fromStdString(*infoStyle.foreground), QString::fromLatin1(OVERRIDE_FG));
        QCOMPARE(spy.count(), 1);

        // Saving an unrelated theme name (not active, not resolved
        // by Auto) must NOT fire `themeChanged` -- the contract for
        // inactive-theme edits stays surprise-free.
        loglib::Theme inactive;
        inactive.name = "Solarized";
        inactive.kind = loglib::ThemeKind::Light;
        spy.clear();
        ThemeControl::SaveUserTheme(QStringLiteral("Solarized"), inactive);
        QCOMPARE(spy.count(), 0);
    }

private:
    /// Snapshot of `qApp->style()->name()` captured by `init()` so
    /// `cleanup()` can restore the style any test mutated via a
    /// theme JSON's `app.qtStyle` override.
    QString mInitStyleName;
};

QTEST_MAIN(ThemeControlTest)
#include "test_theme_control.moc"
