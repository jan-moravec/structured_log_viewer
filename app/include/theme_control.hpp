#pragma once

#include <loglib/log_level.hpp>
#include <loglib/theme.hpp>

#include <QBrush>
#include <QDir>
#include <QFont>
#include <QIcon>
#include <QList>
#include <QObject>
#include <QString>
#include <Qt>

#include <array>
#include <map>
#include <optional>

class QColor;

/// Owns the active theme, exposes per-`LogLevel` brushes and
/// fonts to the table model, and auto-switches between Light and
/// Dark based on the OS palette.
///
/// One instance per process, constructed by `main()` after
/// `QApplication` and passed by pointer through `MainWindow` to
/// the widgets that need it (`LogModel`, `FilterEditor`,
/// `PreferencesEditor`). Connecting to `themeChanged()` keeps
/// dependent views in sync.
///
/// Persistence: a single `theme/active` `QSettings` key. Empty
/// means "Auto" (follow the OS); any other value forces that
/// theme. Theme files live in `resources/themes/*.json` (read-only
/// built-ins) and `<AppDataLocation>/themes/*.json` (user files,
/// which shadow built-ins by `name`).
class ThemeControl : public QObject
{
    Q_OBJECT

public:
    /// `theme/active` value meaning "Auto: follow the OS scheme".
    static constexpr const char *AUTO_TOKEN = "";

    /// Discover themes, resolve the active selection, and push it
    /// onto `qApp`. Construct after `QApplication` so the
    /// startup-style/font snapshot reads the real defaults.
    explicit ThemeControl(QObject *parent = nullptr);

    /// Active selection as persisted on disk (empty = Auto). May
    /// differ from `ActiveSelection()` during a Preferences
    /// preview or when the persisted name no longer exists.
    [[nodiscard]] QString PersistedSelection() const;

    /// Persist the in-memory active selection to `QSettings`.
    void SaveConfiguration() const;

    /// Re-run the auto-switch (e.g. after an OS dark-mode toggle).
    /// No-op when the resolved theme is unchanged.
    void Reevaluate();

    /// True while an `ApplyTheme` is in flight. `MainWindow::event`
    /// uses this to skip the table-QSS re-apply on our own
    /// self-induced `StyleChange` / `ApplicationPaletteChange`
    /// events.
    [[nodiscard]] bool IsApplyingTheme() const noexcept;

    /// Re-scan disk and re-resolve. Surfaced by the Preferences
    /// "Reload themes from disk" button.
    void ReloadAll();

    /// Currently resolved theme.
    [[nodiscard]] const loglib::Theme &Active() const;

    /// Cached brushes / fonts for `LogModel::data()`. Returns an
    /// invalid brush when the theme doesn't style @p level, which
    /// the model converts to "use palette default".
    [[nodiscard]] QBrush ForegroundFor(loglib::LogLevel level) const noexcept;
    [[nodiscard]] QBrush BackgroundFor(loglib::LogLevel level) const noexcept;

    /// `QFont` for @p level, derived from `qApp->font()` at apply
    /// time with bold/italic applied per the theme. Returns
    /// `qApp->font()` unchanged when the level isn't styled, so
    /// callers in the paint hot path should gate on
    /// `HasFontStyle(level)` first.
    [[nodiscard]] QFont FontFor(loglib::LogLevel level) const noexcept;

    /// True iff the active theme sets bold or italic for @p level.
    [[nodiscard]] bool HasFontStyle(loglib::LogLevel level) const noexcept;

    /// True iff *any* level sets bold or italic. Lets the model
    /// skip the per-cell `FontRole` resolve when no theme entry
    /// changes the font.
    [[nodiscard]] bool HasAnyFontStyle() const noexcept;

    /// Cached anchor brush for @p colorIndex. @p role must be
    /// `Qt::BackgroundRole` or `Qt::ForegroundRole`; foregrounds
    /// are picked per slot for legible contrast. Out-of-range
    /// returns an invalid brush so callers fall through to default
    /// styling.
    [[nodiscard]] QBrush AnchorBrushFor(std::uint8_t colorIndex, int role) const noexcept;

    /// True iff the active theme ships a `levelColumnOverride`.
    /// The single bool every consumer reads to decide "icon mode?".
    [[nodiscard]] bool HasLevelColumnOverride() const noexcept;

    /// True iff the active theme ships a non-empty
    /// `levelsHighContrast` map. The Preferences dialog greys out
    /// the high-contrast checkbox when this is false.
    [[nodiscard]] bool HasLevelsHighContrast() const noexcept;

    /// Current `ui/highContrastLevels` value as last applied.
    [[nodiscard]] bool IsHighContrast() const noexcept;

    /// Switch between subtle (false, default) and loud (true) row
    /// styling. No-op on same value; otherwise rebuilds the style
    /// cache and emits `themeChanged()`.
    void SetHighContrast(bool on);

    /// Cached icon for @p level. Null when the theme has no icon
    /// for it or when icon mode is off.
    [[nodiscard]] QIcon IconFor(loglib::LogLevel level) const noexcept;

    /// Pill background for @p level. Invalid brush when the theme
    /// omits `pillBackground` for this level (the delegate then
    /// skips the pill and paints the icon over the row fill).
    [[nodiscard]] QBrush PillBackgroundFor(loglib::LogLevel level) const noexcept;

    /// Pill foreground for @p level (tints the icon). Resolution
    /// chain: `override.pillForeground` -> `LevelStyle::foreground`
    /// -> `QPalette::WindowText`. Always valid in icon mode.
    [[nodiscard]] QBrush PillForegroundFor(loglib::LogLevel level) const noexcept;

    /// Theme's header-text override (`Column::header` fallback when
    /// nullopt). Empty `QString` means "render no header text".
    /// When a `headerIcon` is also set and this is nullopt,
    /// `LogModel::headerData` suppresses the text so the icon
    /// stands alone.
    [[nodiscard]] std::optional<QString> LevelColumnHeaderTextOverride() const;

    /// Theme's header-icon override. Null when not set; callers
    /// fall back to the existing warning > funnel > nothing
    /// `DecorationRole` priority.
    [[nodiscard]] QIcon LevelColumnHeaderIcon() const;

    /// In-memory active selection (empty = Auto).
    [[nodiscard]] QString ActiveSelection() const;

    /// Update the in-memory selection (does not persist). Triggers
    /// `themeChanged()` when the resolved theme changes.
    void SetActiveSelection(const QString &nameOrAuto);

    /// One discovered theme entry. `fromUser` lets the UI tag
    /// user themes.
    struct ThemeListing
    {
        QString name;
        loglib::ThemeKind kind = loglib::ThemeKind::Light;
        bool fromUser = false;
    };

    /// Sorted alphabetically by name. Built-ins shadowed by a user
    /// file appear once, marked `fromUser=true`.
    [[nodiscard]] QList<ThemeListing> AvailableThemes() const;

    /// Theme by name, or nullopt if no such entry. User files
    /// shadow built-ins.
    [[nodiscard]] std::optional<loglib::Theme> Load(const QString &name) const;

    /// `<AppDataLocation>/themes`, created on demand. Falls back
    /// to `<temp>/StructuredLogViewer/themes` if AppData is empty.
    static QDir UserThemesDir();

    /// Open the user themes folder in the OS file manager.
    /// Returns false on failure.
    static bool RevealUserThemesDir();

    /// Atomically write @p theme to `<UserThemesDir>/<name>.json`
    /// (the on-disk `name` field is set to @p name) and refresh
    /// the index. Throws `std::runtime_error` on invalid @p name
    /// (see `SanitiseThemeName`) or write failure. Does not change
    /// the active selection.
    void SaveUserTheme(const QString &name, loglib::Theme theme);

    /// True iff @p color has an ITU-R BT.601 luma below mid-grey.
    /// Single source of truth for "is this surface dark?".
    [[nodiscard]] static bool IsDarkColor(const QColor &color) noexcept;

    /// Validate @p name as a safe filename basename. Rejects path
    /// separators, `..`, control characters, leading/trailing
    /// whitespace, trailing dot or space, and reserved Win32
    /// device names. Returns @p name unchanged on success, throws
    /// `std::runtime_error` on rejection.
    [[nodiscard]] static QString SanitiseThemeName(const QString &name);

    /// True iff a Force-mode `QStyleHints::colorScheme` override
    /// is currently held. Exposed as a public instance method so
    /// tests can exercise the Force/Auto state machine without
    /// build-flag gating.
    [[nodiscard]] bool IsColorSchemeForcedForTest() const noexcept;

    /// Pin the cached OS colour scheme. Pass
    /// `Qt::ColorScheme::Unknown` to release the pin. Test hook
    /// for the Auto-mode resolver; production code lets
    /// `QStyleHints::colorSchemeChanged` drive the cache.
    void SetOsColorSchemeForTest(Qt::ColorScheme scheme) noexcept;

signals:
    /// Emitted when the resolved active theme changes. Receivers
    /// should refresh per-cell brushes and table chrome.
    void themeChanged();

private:
    /// Slot for `QStyleHints::colorSchemeChanged`. Caches the
    /// OS-reported scheme. Skipped during our own apply path and
    /// while a Force override is held (Qt then reports our forced
    /// value, not the OS).
    void OnPlatformColorSchemeChanged(Qt::ColorScheme scheme);

    void LoadConfiguration();
    void DiscoverThemes();
    void ResolveAndApplyActive(bool emitWhenUnchanged);
    void ApplyTheme(const loglib::Theme &theme);
    void ApplyColorSchemeHint(const loglib::Theme &theme);
    void ApplyPalette(const loglib::Theme &theme);
    void BuildStyleCache(const loglib::Theme &theme);

    /// Per-name index. User-dir entries shadow built-ins.
    struct IndexEntry
    {
        loglib::Theme theme;
        bool fromUser = false;
        /// Directory the theme JSON was loaded from (`":/themes"`
        /// for built-ins, an absolute path for user themes). Used
        /// by `BuildStyleCache` to resolve relative icon paths.
        QString sourceDir;
    };
    std::map<QString, IndexEntry> mIndex;

    loglib::Theme mActive;
    QString mActiveName;
    /// Source directory of the active theme, cached for icon-path
    /// resolution in `BuildStyleCache`.
    QString mActiveSourceDir;
    /// True iff the active theme came from `UserThemesDir()`.
    /// Gates the user-only path rules (absolute paths allowed,
    /// parent-traversal rejected).
    bool mActiveFromUser = false;

    /// `theme/active` value (empty = Auto).
    QString mActiveSelection;

    /// Selection in effect during the last `ApplyTheme`. Compared
    /// against `mActiveSelection` so a Force<->Auto flip that
    /// keeps the same resolved theme still re-runs the
    /// colour-scheme hint.
    QString mAppliedSelection;

    /// Per-`LogLevel` style cache, indexed by enum value
    /// (`Unknown=0 .. Fatal=6`).
    static constexpr size_t LEVEL_SLOTS = 7;
    std::array<QBrush, LEVEL_SLOTS> mForeground;
    std::array<QBrush, LEVEL_SLOTS> mBackground;
    std::array<QFont, LEVEL_SLOTS> mFonts;
    std::array<bool, LEVEL_SLOTS> mBold{};
    std::array<bool, LEVEL_SLOTS> mItalic{};

    /// Per-anchor-slot brush cache. Built by `BuildStyleCache` from
    /// `theme.anchorPalette`, with the built-in palette filling gaps.
    std::array<QBrush, loglib::ANCHOR_PALETTE_SIZE> mAnchorBackground;
    std::array<QBrush, loglib::ANCHOR_PALETTE_SIZE> mAnchorForeground;

    /// Mirrors `theme.levelColumnOverride.has_value()`.
    bool mHasLevelColumnOverride = false;

    /// Mirrors `!theme.levelsHighContrast.empty()`. Refreshed by
    /// `BuildStyleCache`.
    bool mHasLevelsHighContrast = false;

    /// In-memory mirror of `ui/highContrastLevels`. Drives the
    /// per-level resolution in `BuildStyleCache`.
    bool mHighContrast = false;

    /// Header-text override; `nullopt` falls back to
    /// `Column::header`. Empty string is preserved (= "no text").
    std::optional<QString> mLevelColumnHeaderText;

    /// Header identity icon; null when the theme didn't set
    /// `headerIcon`.
    QIcon mLevelColumnHeaderIcon;

    /// Per-`LogLevel` icon cache. Null = no icon for this level.
    std::array<QIcon, LEVEL_SLOTS> mLevelIcons;

    /// Per-`LogLevel` pill brushes. Invalid `mPillBackground` = no
    /// pill (icon paints over the row tint).
    std::array<QBrush, LEVEL_SLOTS> mPillBackground;
    std::array<QBrush, LEVEL_SLOTS> mPillForeground;

    /// Any-level bold-or-italic flag for the `HasAnyFontStyle`
    /// fast-path. Refreshed by `BuildStyleCache`.
    bool mHasAnyFontStyle = false;

    /// Re-entrancy guard for `ApplyTheme`. `qApp->setPalette` /
    /// `qApp->setStyle` fire events that round-trip back through
    /// `MainWindow::event` -> `Reevaluate`.
    bool mApplyingTheme = false;

    /// True while we hold a Force-mode `QStyleHints::colorScheme`
    /// override. Qt has no way to query this, so we track it
    /// ourselves.
    bool mColorSchemeForced = false;

    /// Process style/font captured at the first `LoadConfiguration`,
    /// before any theme runs. Restored when a theme omits the
    /// corresponding `app.*` field, so switching themes reverts
    /// cleanly to the startup defaults.
    bool mStartupCaptured = false;
    QString mStartupStyleName;
    QFont mStartupFont;

    /// Test hook: when true, production code skips refreshing
    /// `mOsColorScheme` from `QStyleHints`, trusting the test's
    /// faked value instead. Always false in production paths
    /// because `SetOsColorSchemeForTest` is the only setter.
    bool mOsColorSchemeLocked = false;

    /// Cached OS-reported colour scheme. Seeded at startup,
    /// refreshed by `colorSchemeChanged` (outside Force mode) and
    /// after `unsetColorScheme()`. The Auto picker reads this --
    /// never `qApp->palette()` -- so a Force-mode palette we
    /// pushed can't mislead the next Auto resolution.
    Qt::ColorScheme mOsColorScheme = Qt::ColorScheme::Unknown;
};
