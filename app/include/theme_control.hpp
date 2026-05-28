#pragma once

#include <loglib/log_level.hpp>
#include <loglib/theme.hpp>

#include <QBrush>
#include <QDir>
#include <QFont>
#include <QList>
#include <QObject>
#include <QString>

#include <array>
#include <map>
#include <optional>

/// Singleton that owns the active theme bundle, exposes per-`LogLevel`
/// brushes / fonts to the table model, and drives the auto-switch
/// between Light and Dark presets based on the OS palette.
///
/// Persistence: a single `theme/active` `QSettings` key. The empty
/// string means "Auto" (follow OS brightness); any non-empty value
/// selects that theme by name unconditionally. Theme JSON files
/// themselves are not stored in `QSettings`; they live in
/// `resources/themes/*.json` (shipped read-only) and
/// `<AppDataLocation>/themes/*.json` (user-writable). A user file
/// whose `name` matches a built-in shadows the built-in everywhere,
/// including Auto mode -- so users can override defaults without
/// changing their selection.
class ThemeControl : public QObject
{
    Q_OBJECT

public:
    /// Sentinel value of the `theme/active` setting that means
    /// "Auto: pick Light or Dark by palette brightness".
    static constexpr const char *AUTO_TOKEN = "";

    /// Process-wide instance. Connect signals against the instance,
    /// query static helpers for everything else.
    static ThemeControl &Instance();

    /// Read `theme/active`, discover available themes, resolve the
    /// active theme, push `qApp->setStyle` / `qApp->setFont`, and
    /// build the brush cache. Call once at startup before
    /// `MainWindow` is constructed.
    static void LoadConfiguration();

    /// Commit the in-memory active selection to `QSettings`.
    static void SaveConfiguration();

    /// Re-evaluate the auto-switch without re-reading from disk.
    /// Used by `MainWindow` after `QEvent::ApplicationPaletteChange`
    /// so OS-level dark-mode toggles flip the active theme.
    /// No-op when the resolved theme is unchanged.
    static void Reevaluate();

    /// Re-scan resources + user dir, then `Reevaluate()`. Surfaced
    /// to users via the Preferences "Reload themes from disk" button.
    static void ReloadAll();

    /// Currently resolved theme (after auto / selection logic).
    static const loglib::Theme &Active();

    /// Cached brushes / fonts for use in `LogModel::data()`. Return
    /// an invalid `QBrush` when the active theme does not set a
    /// color for @p level -- the model treats that as "use palette
    /// default" by returning an empty `QVariant` to the view.
    static QBrush ForegroundFor(loglib::LogLevel level) noexcept;
    static QBrush BackgroundFor(loglib::LogLevel level) noexcept;
    /// Returns @p base modified to honour the theme's bold/italic
    /// flags for @p level. When neither flag is set this still
    /// returns @p base unchanged, so the caller can pass the
    /// view's current font and get back a font that matches the
    /// view's family/size even after a font change.
    static QFont FontFor(loglib::LogLevel level, const QFont &base) noexcept;
    /// True iff the active theme specifies any styling at all for
    /// @p level (foreground, background, bold, or italic). Lets
    /// the model short-circuit role lookups for unstyled levels.
    static bool HasStyle(loglib::LogLevel level) noexcept;

    /// True iff the active theme sets bold or italic for @p level.
    /// Lets the model skip `FontRole` entirely when only colour
    /// tweaks apply, avoiding a per-cell QFont copy on every
    /// paint of a coloured-but-not-bold row.
    static bool HasFontStyle(loglib::LogLevel level) noexcept;

    /// Active selection token from `QSettings`. Empty means Auto.
    static QString ActiveSelection();

    /// Update the in-memory selection; call `SaveConfiguration` to
    /// persist. Triggers `Reevaluate()` + `themeChanged()` when the
    /// resolved theme changes.
    static void SetActiveSelection(const QString &nameOrAuto);

    /// One discovered theme. `fromUser` lets the UI label user
    /// themes with `(user)` and distinguish them from built-ins.
    struct ThemeListing
    {
        QString name;
        loglib::ThemeKind kind = loglib::ThemeKind::Light;
        bool fromUser = false;
    };

    /// Sorted alphabetically by name. Built-in entries shadowed by
    /// a user file appear once, marked `fromUser=true`.
    static QList<ThemeListing> AvailableThemes();

    /// Theme by name, or nullopt when no entry matches. User-dir
    /// entries shadow built-ins.
    static std::optional<loglib::Theme> Load(const QString &name);

    /// `<AppDataLocation>/themes`. Created on demand. Falls back to
    /// `<temp>/StructuredLogViewer/themes` when AppData is empty.
    static QDir UserThemesDir();

    /// Open the user themes folder in the OS file manager. Returns
    /// false on failure (e.g. when the folder couldn't be created).
    static bool RevealUserThemesDir();

    /// Write @p theme to `<UserThemesDir>/<name>.json`. The on-disk
    /// `name` field is the stable identity used by `DiscoverThemes`
    /// to key the index; renaming the file on disk leaves the
    /// in-app name pinned to @p name. Throws `std::runtime_error`
    /// when @p name fails `SanitiseThemeName` (path separators,
    /// `..`, reserved Win32 device names, control characters) or on
    /// serialise / write failure. The write is atomic
    /// (`QSaveFile`).
    static void SaveUserTheme(const QString &name, loglib::Theme theme);

    /// Reject @p name when it contains path separators, `..`,
    /// control characters, leading/trailing whitespace, or matches
    /// a reserved Win32 device name (`CON`, `PRN`, `AUX`, `NUL`,
    /// `COM1`-`COM9`, `LPT1`-`LPT9`, case-insensitive). Returns the
    /// sanitised name (currently the input verbatim on acceptance)
    /// or throws `std::runtime_error` on rejection. Exposed for
    /// tests and any future "Save As..." flow.
    [[nodiscard]] static QString SanitiseThemeName(const QString &name);

#ifdef LOGAPP_BUILD_TESTING
    /// Test-only: true iff the last `ApplyColorSchemeHint` call
    /// left an explicit Force-mode `QStyleHints::colorScheme`
    /// override active. `unsetColorScheme()` makes Qt report the
    /// system value for `colorScheme()`, so this internal flag is
    /// the only way to recover "is the override currently held?"
    /// for the Force<->Auto regression test.
    [[nodiscard]] static bool IsColorSchemeForcedForTest() noexcept;
#endif

signals:
    /// Emitted when the resolved active theme changes (selection
    /// flipped, auto-switch tripped, or `ReloadAll` brought in a
    /// new file). Receivers should refresh per-cell brushes and
    /// re-apply table chrome.
    void themeChanged();

private:
    ThemeControl();

    void DoLoadConfiguration();
    void DoReloadAll();
    void DoReevaluate();
    void DoSetActiveSelection(const QString &nameOrAuto);
    void DiscoverThemes();
    void ResolveAndApplyActive(bool emitWhenUnchanged);
    void ApplyTheme(const loglib::Theme &theme);
    void ApplyColorSchemeHint(const loglib::Theme &theme);
    void ApplyPalette(const loglib::Theme &theme);
    void BuildStyleCache(const loglib::Theme &theme);

    /// Per-name index. User-dir entries override built-ins.
    struct IndexEntry
    {
        loglib::Theme theme;
        bool fromUser = false;
    };
    std::map<QString, IndexEntry> mIndex;

    /// Resolved theme (after Auto / selection logic).
    loglib::Theme mActive;
    QString mActiveName; // mActive.name; used for change detection.

    /// `theme/active` value (empty = Auto).
    QString mActiveSelection;

    /// Snapshot of `mActiveSelection` at the time `ApplyTheme` last
    /// ran. Compared against `mActiveSelection` in
    /// `ResolveAndApplyActive` so a Force<->Auto flip that lands on
    /// the same resolved theme name still re-runs
    /// `ApplyColorSchemeHint` (which depends on the selection mode,
    /// not on the theme name). Without this, switching from Force
    /// "Light" to Auto on a light system would leave Qt's
    /// `QStyleHints::colorScheme` pinned to `Light` and OS palette
    /// flips would no longer drive `ApplicationPaletteChange`.
    QString mAppliedSelection;

    /// Pre-built brushes / flags indexed by the `LogLevel` enum
    /// (`Unknown=0 .. Fatal=6`). Sized to `Fatal + 1` so the enum
    /// can index directly without a switch.
    static constexpr size_t LEVEL_SLOTS = 7;
    std::array<QBrush, LEVEL_SLOTS> mForeground;
    std::array<QBrush, LEVEL_SLOTS> mBackground;
    std::array<bool, LEVEL_SLOTS> mBold{};
    std::array<bool, LEVEL_SLOTS> mItalic{};
    std::array<bool, LEVEL_SLOTS> mHasAnyStyle{};

    /// Re-entrancy guard for `ApplyTheme`. `qApp->setPalette` /
    /// `qApp->setStyle` synchronously fan out
    /// `QEvent::ApplicationPaletteChange` / `StyleChange`, which
    /// `MainWindow::event` routes back to `ThemeControl::Reevaluate`.
    /// Without this guard we would re-enter `ApplyTheme` from inside
    /// itself (the resolved theme name is unchanged, so
    /// `ResolveAndApplyActive` would early-out, but for safety we
    /// short-circuit before any work is done).
    bool mApplyingTheme = false;

    /// True iff we currently hold a Force-mode override on
    /// `QStyleHints::colorScheme`. Needed because once
    /// `unsetColorScheme()` has run, `colorScheme()` reports the
    /// system's current value (not `Unknown`), so we cannot
    /// recover "is this currently a forced override?" from Qt
    /// alone. Used to skip redundant `unsetColorScheme()` calls
    /// on Auto-mode re-evaluations after the first one.
    bool mColorSchemeForced = false;
};
