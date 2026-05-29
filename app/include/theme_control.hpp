#pragma once

#include <loglib/log_level.hpp>
#include <loglib/theme.hpp>

#include <QBrush>
#include <QDir>
#include <QFont>
#include <QList>
#include <QObject>
#include <QString>
#include <Qt>

#include <array>
#include <map>
#include <optional>

class QColor;

/// Singleton that owns the active theme bundle, exposes per-`LogLevel`
/// brushes / fonts to the table model, and drives the auto-switch
/// between Light and Dark presets based on the OS palette.
///
/// Persistence: a single `theme/active` `QSettings` key. The empty
/// string means "Auto" (follow the OS colour scheme); any non-empty
/// value selects that theme by name unconditionally. Theme JSON files
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
    /// "Auto: pick Light or Dark by the OS colour scheme".
    static constexpr const char *AUTO_TOKEN = "";

    /// Process-wide instance. Connect signals against the instance,
    /// query static helpers for everything else.
    static ThemeControl &Instance();

    /// Read `theme/active`, discover available themes, resolve the
    /// active theme, push `qApp->setStyle` / `qApp->setFont`, and
    /// build the brush cache. Call once at startup before
    /// `MainWindow` is constructed.
    static void LoadConfiguration();

    /// Persisted `theme/active` value from `QSettings`. Empty string
    /// means Auto. The single source of truth for "what's on disk";
    /// use `ActiveSelection()` for the in-memory live value (which
    /// may differ during a `PreferencesEditor` preview or after a
    /// stale-selection coercion in `ResolveAndApplyActive`).
    [[nodiscard]] static QString PersistedSelection();

    /// Commit the in-memory active selection to `QSettings`.
    static void SaveConfiguration();

    /// Re-evaluate the auto-switch without re-reading from disk.
    /// Used by `MainWindow` after `QEvent::ApplicationPaletteChange`
    /// so OS-level dark-mode toggles flip the active theme.
    /// No-op when the resolved theme is unchanged.
    static void Reevaluate();

    /// True iff an `ApplyTheme` call is currently in flight.
    /// `qApp->setStyle` / `qApp->setPalette` synchronously fan
    /// out `QEvent::StyleChange` / `ApplicationPaletteChange` to
    /// every widget; `MainWindow::event` checks this flag so it
    /// doesn't re-run the table QSS apply on those self-induced
    /// events (the tail-end `themeChanged` slot covers them).
    [[nodiscard]] static bool IsApplyingTheme() noexcept;

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
    /// Pre-built `QFont` for @p level. Built off `qApp->font()` at
    /// the time of the last `ApplyTheme`, with bold/italic applied
    /// per the active theme. Callers that need the unmodified
    /// app font for a level the theme leaves un-styled should gate
    /// on `HasFontStyle(level)` first and skip the lookup -- this
    /// returns `qApp->font()` unchanged in that case, but the
    /// gating saves the per-cell `QVariant` round-trip in the
    /// `LogModel::data` hot path.
    static QFont FontFor(loglib::LogLevel level) noexcept;

    /// True iff the active theme sets bold or italic for @p level.
    /// Lets the model skip `FontRole` entirely when only colour
    /// tweaks apply, avoiding a per-cell QFont copy on every
    /// paint of a coloured-but-not-bold row.
    static bool HasFontStyle(loglib::LogLevel level) noexcept;

    /// True iff any level in the active theme sets bold or italic.
    /// Cheap top-of-`Qt::FontRole` gate so themes without any font
    /// styling (every shipped theme except the ones tagging
    /// `Fatal: bold`) skip the per-cell `LevelForRow` resolve
    /// entirely. Recomputed once per `ApplyTheme`.
    static bool HasAnyFontStyle() noexcept;

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

    /// Write @p theme to `<UserThemesDir>/<name>.json` and refresh
    /// the in-memory index so the new entry is immediately
    /// discoverable via `AvailableThemes` / `Load` /
    /// `SetActiveSelection`. The on-disk `name` field is the
    /// stable identity used by `DiscoverThemes` to key the index;
    /// renaming the file on disk leaves the in-app name pinned to
    /// @p name. Throws `std::runtime_error` when @p name fails
    /// `SanitiseThemeName` (path separators, `..`, reserved Win32
    /// device names, control characters, trailing dot/space) or on
    /// serialise / write failure. The write is atomic
    /// (`QSaveFile`). Callers that want the freshly-saved theme to
    /// become active still need to follow up with
    /// `SetActiveSelection(name)`.
    static void SaveUserTheme(const QString &name, loglib::Theme theme);

    /// True iff @p color has an ITU-R BT.601 luma below the
    /// app-wide "dark surface" threshold. Single source of truth
    /// for the dark/light heuristic used by the auto theme switch
    /// (`QPalette::Window`) and by per-widget validation feedback
    /// in `FilterEditor` (`QPalette::Base`), so every dark-mode
    /// decision lands on the same threshold.
    [[nodiscard]] static bool IsDarkColor(const QColor &color) noexcept;

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

    /// Test-only: fake the OS-reported colour scheme. CI runners
    /// have unpredictable platform-theme state -- `QStyleHints::
    /// colorScheme()` may return `Unknown` on a minimal Linux
    /// image, and `QStyleHints::setColorScheme()` would trigger
    /// our Force-mode bookkeeping. This setter writes
    /// `mOsColorScheme` directly AND latches `mOsColorSchemeLocked`
    /// so production code paths that would normally refresh the
    /// cache from `hints->colorScheme()` (Force engagement, Force
    /// -> Auto release) skip those reads and trust the cached
    /// value instead. Pass `Qt::ColorScheme::Unknown` to clear
    /// the lock and revert to platform reporting.
    static void SetOsColorSchemeForTest(Qt::ColorScheme scheme) noexcept;
#endif

signals:
    /// Emitted when the resolved active theme changes (selection
    /// flipped, auto-switch tripped, or `ReloadAll` brought in a
    /// new file). Receivers should refresh per-cell brushes and
    /// re-apply table chrome.
    void themeChanged();

private:
    ThemeControl();

    /// Slot wired to `QStyleHints::colorSchemeChanged`. Caches the
    /// OS-reported scheme into `mOsColorScheme` so the Auto picker
    /// can recover it later even after we've installed a Force-mode
    /// override (Qt suppresses OS-driven scheme updates while a
    /// `setColorScheme()` override is active). Skipped when
    /// `mApplyingTheme` is true (the change is one we just pushed
    /// ourselves) or when we currently hold a Force override (Qt's
    /// reported value is our own forced value, not the OS).
    void OnPlatformColorSchemeChanged(Qt::ColorScheme scheme);

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
    /// `QStyleHints::colorScheme` pinned to `Light` and OS scheme
    /// flips would no longer drive `colorSchemeChanged`.
    QString mAppliedSelection;

    /// Pre-built brushes / fonts / flags indexed by the `LogLevel`
    /// enum (`Unknown=0 .. Fatal=6`). Sized to `Fatal + 1` so the
    /// enum can index directly without a switch. `mFonts` is
    /// rebuilt off `qApp->font()` every `ApplyTheme` so a theme
    /// that pins a font family / size flows into per-cell paints
    /// without a per-cell `qApp->font()` copy.
    static constexpr size_t LEVEL_SLOTS = 7;
    std::array<QBrush, LEVEL_SLOTS> mForeground;
    std::array<QBrush, LEVEL_SLOTS> mBackground;
    std::array<QFont, LEVEL_SLOTS> mFonts;
    std::array<bool, LEVEL_SLOTS> mBold{};
    std::array<bool, LEVEL_SLOTS> mItalic{};

    /// OR-fold of `mBold` and `mItalic` across all level slots,
    /// refreshed by `BuildStyleCache`. Powers the
    /// `HasAnyFontStyle()` fast-path so `LogModel::data` can
    /// short-circuit the `Qt::FontRole` branch without resolving
    /// a level when the active theme leaves every level at
    /// regular weight.
    bool mHasAnyFontStyle = false;

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

    /// One-shot snapshot of `qApp->style()->name()` and
    /// `qApp->font()` taken at the very first `LoadConfiguration`,
    /// before any theme has been applied. Restored by `ApplyTheme`
    /// when the active theme omits `app.qtStyle` /
    /// `app.fontFamily` / `app.fontSize`, so switching from a
    /// theme that defines those fields back to one that doesn't
    /// reverts to the process defaults instead of inheriting the
    /// previous theme's values.
    bool mStartupCaptured = false;
    QString mStartupStyleName;
    QFont mStartupFont;

#ifdef LOGAPP_BUILD_TESTING
    /// Test-only lock: when true, `ResolveAndApplyActive` and
    /// `ApplyColorSchemeHint` skip the "refresh `mOsColorScheme`
    /// from `hints->colorScheme()`" steps and trust the cached
    /// value. Set by `SetOsColorSchemeForTest` so a faked OS
    /// scheme isn't immediately clobbered by Qt's platform-
    /// reported value during a Force engagement or release.
    /// Cleared when the test passes `Qt::ColorScheme::Unknown`.
    bool mOsColorSchemeLocked = false;
#endif

    /// Best-effort cache of the OS-reported `Qt::ColorScheme`.
    /// Seeded at the first `LoadConfiguration` (before any
    /// `setColorScheme` override has been pushed) and refreshed in
    /// two places: (1) `OnPlatformColorSchemeChanged` whenever Qt
    /// fires `colorSchemeChanged` from outside our apply path AND
    /// no Force override is held, and (2) right after we call
    /// `unsetColorScheme()` in `ResolveAndApplyActive`'s Force ->
    /// Auto path, since Qt suppresses OS-driven scheme updates
    /// while we hold an override (so the cache can be stale by
    /// many minutes when the user has been in Force mode through
    /// an OS theme flip). The Auto picker reads this cache --
    /// never `qApp->palette()` -- so a Force-mode palette / colour
    /// scheme we pushed earlier cannot mislead the next Auto
    /// resolution into picking the wrong kind.
    Qt::ColorScheme mOsColorScheme = Qt::ColorScheme::Unknown;
};
