#include "theme_control.hpp"

#include <loglib/log_level.hpp>
#include <loglib/theme.hpp>

#include <QApplication>
#include <QColor>
#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLatin1String>
#include <QPalette>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>
#include <QTextStream>
#include <QUrl>
#include <QVariant>
#include <QtGlobal>

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>
#include <string_view>

namespace
{

constexpr char SETTINGS_KEY_ACTIVE[] = "theme/active";

constexpr char BUILTIN_LIGHT_NAME[] = "Light";
constexpr char BUILTIN_DARK_NAME[] = "Dark";

// `Light` and `Dark` are the canonical Auto-mode targets (see
// `ResolveAndApplyActive`); the rest are popular VS Code-flavoured
// presets users can Force-select. Adding more dark-kind built-ins
// does NOT change Auto resolution because the Auto picker keys off
// the literal `Light` / `Dark` names, not `theme.kind`.
constexpr char BUILTIN_LIGHT_PATH[] = ":/themes/light.json";
constexpr char BUILTIN_DARK_PATH[] = ":/themes/dark.json";
constexpr char BUILTIN_GITHUB_DARK_PATH[] = ":/themes/github_dark.json";
constexpr char BUILTIN_GITHUB_LIGHT_PATH[] = ":/themes/github_light.json";
constexpr char BUILTIN_MATERIAL_DARK_PATH[] = ":/themes/material_dark.json";
constexpr char BUILTIN_MATERIAL_LIGHT_PATH[] = ":/themes/material_light.json";
constexpr char BUILTIN_MONOKAI_DARK_PATH[] = ":/themes/monokai_dark.json";
constexpr char BUILTIN_MONOKAI_LIGHT_PATH[] = ":/themes/monokai_light.json";

/// Linear `lerp(@p fg, @p bg, @p t)` in sRGB. Used by `ApplyPalette`
/// to dim the Disabled colour group's text-bearing roles toward
/// their surrounding surface so disabled chrome reads as visibly
/// dimmer without disappearing. Linear (rather than perceptual /
/// HSL) interpolation is good enough at the 0.40-0.55 mix factors
/// the call sites use; switching to HSL would not produce a
/// perceptibly better result for "obviously disabled" feedback
/// and would cost a colour-space round-trip per role.
QColor BlendTowards(const QColor &fg, const QColor &bg, float t)
{
    const float clamped = std::clamp(t, 0.0F, 1.0F);
    return QColor::fromRgbF(
        (fg.redF() * (1.0F - clamped)) + (bg.redF() * clamped),
        (fg.greenF() * (1.0F - clamped)) + (bg.greenF() * clamped),
        (fg.blueF() * (1.0F - clamped)) + (bg.blueF() * clamped),
        1.0F
    );
}

size_t LevelIndex(loglib::LogLevel level) noexcept
{
    return static_cast<size_t>(level);
}

QBrush BrushFromHex(const std::optional<std::string> &hex) noexcept
{
    if (!hex.has_value() || hex->empty())
    {
        return QBrush{}; // invalid -> "use palette default"
    }
    const QColor color(QString::fromStdString(*hex));
    if (!color.isValid())
    {
        return QBrush{};
    }
    return QBrush(color);
}

/// Read a UTF-8 text file via Qt so the resource scheme (`:/...`)
/// works alongside on-disk paths. Returns nullopt on open failure.
std::optional<std::string> ReadFileUtf8(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        return std::nullopt;
    }
    const QByteArray bytes = file.readAll();
    return std::string(bytes.constData(), static_cast<size_t>(bytes.size()));
}

/// Emit a `qWarning` for every level key in @p theme that doesn't
/// spell a canonical name (`Trace`..`Fatal`, or the literal
/// `Unknown` sentinel). Hand-edited theme JSON typoes (`"Worn"`,
/// `"warning"`) silently render nothing without this hint --
/// users can't tell whether their override is unused because the
/// theme didn't load or because the key didn't match.
void WarnOnUnknownLevelKeys(const QString &source, const loglib::Theme &theme)
{
    using loglib::LogLevel;
    static constexpr std::array<LogLevel, loglib::CANONICAL_LEVEL_COUNT + 1> ALL_LEVELS = {
        LogLevel::Unknown, LogLevel::Trace, LogLevel::Debug, LogLevel::Info,
        LogLevel::Warn,    LogLevel::Error, LogLevel::Fatal
    };
    for (const auto &[key, value] : theme.levels)
    {
        const bool matchesCanonical = std::ranges::any_of(ALL_LEVELS, [&key](LogLevel level) {
            return std::string_view(key) == loglib::CanonicalLevelName(level);
        });
        if (!matchesCanonical)
        {
            qWarning(
                "Theme %s has unrecognised level key `%s`; expected one of "
                "Trace/Debug/Info/Warn/Error/Fatal/Unknown. The entry is ignored.",
                qUtf8Printable(source),
                key.c_str()
            );
        }
    }
}

bool IsReservedWin32DeviceName(const QString &name)
{
    // Case-insensitive match against the Win32 reserved device set.
    // Same names are reserved with or without an extension on
    // Windows, and creating one of them under any path produces a
    // device handle instead of a file. Reject up front so a user
    // theme called "CON" doesn't silently swallow the write.
    static constexpr std::array<const char *, 22> RESERVED = {"CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2",
                                                              "COM3", "COM4", "COM5", "COM6", "COM7", "COM8",
                                                              "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5",
                                                              "LPT6", "LPT7", "LPT8", "LPT9"};
    const QString upper = name.toUpper();
    return std::ranges::any_of(RESERVED, [&upper](const char *reserved) {
        return upper == QString::fromLatin1(reserved);
    });
}

std::optional<loglib::Theme> ParseFileToTheme(const QString &path)
{
    const auto bytes = ReadFileUtf8(path);
    if (!bytes.has_value())
    {
        return std::nullopt;
    }
    try
    {
        return loglib::ParseTheme(*bytes);
    }
    catch (const std::exception &)
    {
        // Corrupt user file: skip and let the next discovery surface
        // the fallback (built-in). Parse errors are intentionally
        // swallowed at scan time so one broken file cannot wedge the
        // app on startup.
        return std::nullopt;
    }
}

} // namespace

ThemeControl &ThemeControl::Instance()
{
    static ThemeControl instance;
    return instance;
}

ThemeControl::ThemeControl() = default;

void ThemeControl::LoadConfiguration()
{
    Instance().DoLoadConfiguration();
}

QString ThemeControl::PersistedSelection()
{
    QSettings settings;
    const QVariant raw = settings.value(QLatin1String(SETTINGS_KEY_ACTIVE));
    return raw.isValid() ? raw.toString() : QString();
}

void ThemeControl::SaveConfiguration()
{
    QSettings settings;
    settings.setValue(QLatin1String(SETTINGS_KEY_ACTIVE), Instance().mActiveSelection);
}

void ThemeControl::Reevaluate()
{
    Instance().DoReevaluate();
}

bool ThemeControl::IsApplyingTheme() noexcept
{
    return Instance().mApplyingTheme;
}

void ThemeControl::OnPlatformColorSchemeChanged(Qt::ColorScheme scheme)
{
    // Skip self-induced changes: `setColorScheme` /
    // `unsetColorScheme` from inside `ApplyColorSchemeHint` /
    // `ResolveAndApplyActive` both fire this signal. The Force ->
    // Auto path inside `ResolveAndApplyActive` reads `colorScheme()`
    // explicitly right after unsetting, so we don't need this slot
    // for that case either.
    if (mApplyingTheme)
    {
        return;
    }
    // When we hold a Force-mode override, the value Qt reports here
    // is the value WE forced (Qt suppresses OS-driven updates to
    // `colorScheme()` while the override is active). Recording it
    // would defeat the entire reason this cache exists.
    if (mColorSchemeForced)
    {
        return;
    }
    mOsColorScheme = scheme;
}

void ThemeControl::ReloadAll()
{
    Instance().DoReloadAll();
}

const loglib::Theme &ThemeControl::Active()
{
    return Instance().mActive;
}

QBrush ThemeControl::ForegroundFor(loglib::LogLevel level) noexcept
{
    auto &self = Instance();
    const size_t idx = LevelIndex(level);
    if (idx >= self.mForeground.size())
    {
        return {};
    }
    return self.mForeground[idx];
}

QBrush ThemeControl::BackgroundFor(loglib::LogLevel level) noexcept
{
    auto &self = Instance();
    const size_t idx = LevelIndex(level);
    if (idx >= self.mBackground.size())
    {
        return {};
    }
    return self.mBackground[idx];
}

QFont ThemeControl::FontFor(loglib::LogLevel level) noexcept
{
    auto &self = Instance();
    const size_t idx = LevelIndex(level);
    if (idx >= self.mFonts.size())
    {
        return qApp->font();
    }
    return self.mFonts[idx];
}

bool ThemeControl::HasFontStyle(loglib::LogLevel level) noexcept
{
    auto &self = Instance();
    const size_t idx = LevelIndex(level);
    if (idx >= self.mBold.size())
    {
        return false;
    }
    return self.mBold[idx] || self.mItalic[idx];
}

bool ThemeControl::HasAnyFontStyle() noexcept
{
    return Instance().mHasAnyFontStyle;
}

QString ThemeControl::ActiveSelection()
{
    return Instance().mActiveSelection;
}

void ThemeControl::SetActiveSelection(const QString &nameOrAuto)
{
    Instance().DoSetActiveSelection(nameOrAuto);
}

QList<ThemeControl::ThemeListing> ThemeControl::AvailableThemes()
{
    QList<ThemeListing> out;
    out.reserve(static_cast<int>(Instance().mIndex.size()));
    for (const auto &[name, entry] : Instance().mIndex)
    {
        out.append(ThemeListing{.name = name, .kind = entry.theme.kind, .fromUser = entry.fromUser});
    }
    return out;
}

std::optional<loglib::Theme> ThemeControl::Load(const QString &name)
{
    const auto it = Instance().mIndex.find(name);
    if (it == Instance().mIndex.end())
    {
        return std::nullopt;
    }
    return it->second.theme;
}

QDir ThemeControl::UserThemesDir()
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (base.isEmpty())
    {
        base = QDir::tempPath() + QStringLiteral("/StructuredLogViewer");
    }
    const QString themesPath = QDir(base).filePath(QStringLiteral("themes"));
    QDir dir(themesPath);
    if (!dir.exists())
    {
        dir.mkpath(QStringLiteral("."));
    }
    return dir;
}

bool ThemeControl::RevealUserThemesDir()
{
    const QDir dir = UserThemesDir();
    if (!dir.exists())
    {
        return false;
    }
    return QDesktopServices::openUrl(QUrl::fromLocalFile(dir.absolutePath()));
}

bool ThemeControl::IsDarkColor(const QColor &color) noexcept
{
    // ITU-R BT.601 luma weights (`0.299*R + 0.587*G + 0.114*B`)
    // applied to the 8-bit channels and compared against a
    // mid-gray threshold. Kept as a single tunable so a future
    // contrast policy can adjust without chasing duplicate
    // constants in `filter_editor.cpp` etc.
    constexpr int RED_WEIGHT = 299;
    constexpr int GREEN_WEIGHT = 587;
    constexpr int BLUE_WEIGHT = 114;
    constexpr int WEIGHT_DENOMINATOR = 1000;
    constexpr int MID_GRAY_BRIGHTNESS = 128;
    const int brightness =
        ((color.red() * RED_WEIGHT) + (color.green() * GREEN_WEIGHT) + (color.blue() * BLUE_WEIGHT)) /
        WEIGHT_DENOMINATOR;
    return brightness < MID_GRAY_BRIGHTNESS;
}

QString ThemeControl::SanitiseThemeName(const QString &name)
{
    if (name.isEmpty())
    {
        throw std::runtime_error("Theme name must not be empty");
    }
    if (name != name.trimmed())
    {
        throw std::runtime_error("Theme name must not have leading or trailing whitespace");
    }
    if (name == QStringLiteral(".") || name == QStringLiteral(".."))
    {
        throw std::runtime_error("Theme name must not be \".\" or \"..\"");
    }
    if (name.contains(QStringLiteral("..")))
    {
        throw std::runtime_error("Theme name must not contain \"..\"");
    }
    // Reject every byte the OS treats as a path separator or that
    // produces an invalid filename on at least one supported
    // platform. ASCII control characters (`<0x20`) are rejected
    // too: even though POSIX allows them, they break the user's
    // shell and Win32 fails on them outright.
    constexpr char16_t FIRST_PRINTABLE_ASCII = 0x20U;
    for (const QChar ch : name)
    {
        if (ch == QLatin1Char('/') || ch == QLatin1Char('\\') || ch == QLatin1Char(':') ||
            ch == QLatin1Char('<') || ch == QLatin1Char('>') || ch == QLatin1Char('"') ||
            ch == QLatin1Char('|') || ch == QLatin1Char('?') || ch == QLatin1Char('*') ||
            ch.unicode() < FIRST_PRINTABLE_ASCII)
        {
            throw std::runtime_error("Theme name contains invalid character");
        }
    }
    if (IsReservedWin32DeviceName(name))
    {
        throw std::runtime_error("Theme name matches a reserved Win32 device name");
    }
    // Win32 silently strips a trailing dot or space when creating a
    // file. Rejecting those names up front keeps the saved on-disk
    // basename byte-equal to the input, so a user theme called
    // "Dark." can't surreptitiously land at "Dark.json" and shadow
    // the built-in "Dark" entry on the next discovery scan.
    if (name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' ')))
    {
        throw std::runtime_error("Theme name must not end with a dot or space");
    }
    return name;
}

void ThemeControl::SaveUserTheme(const QString &name, loglib::Theme theme)
{
    const QString sanitised = SanitiseThemeName(name);

    // Pin the on-disk `name` to the sanitised input. `DiscoverThemes`
    // keys the index by this field, so renaming the file on disk
    // later leaves the in-app name pinned to whatever was saved here.
    theme.name = sanitised.toStdString();
    const std::string json = loglib::SerializeTheme(theme);

    const QDir dir = UserThemesDir();
    const QString path = dir.filePath(sanitised + QStringLiteral(".json"));

    // `QSaveFile` writes to a sibling temp file and renames into
    // place on `commit()`, so a crash mid-write can't leave a
    // truncated theme JSON behind. The QFile API also honours
    // Qt's UTF-8 path handling on Windows, where `std::ofstream`
    // with `toStdString()` silently fails on non-ASCII profile
    // paths (e.g. `C:\Users\Lukasz\AppData\...`).
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        throw std::runtime_error(
            "Failed to open user theme file: " + path.toStdString() + " (" + file.errorString().toStdString() + ")"
        );
    }
    const QByteArray bytes = QByteArray::fromStdString(json);
    if (file.write(bytes) != bytes.size())
    {
        throw std::runtime_error(
            "Failed to write user theme file: " + path.toStdString() + " (" + file.errorString().toStdString() + ")"
        );
    }
    if (!file.commit())
    {
        throw std::runtime_error(
            "Failed to commit user theme file: " + path.toStdString() + " (" + file.errorString().toStdString() + ")"
        );
    }

    // Refresh the in-memory index so the just-saved theme is
    // immediately discoverable via `AvailableThemes` / `Load` /
    // `SetActiveSelection`. Without this, callers had to follow up
    // with a manual `ReloadAll` -- a footgun that's now centralised
    // here.
    Instance().DiscoverThemes();

    // When the just-saved theme is also the currently-resolved one
    // (either because the user is on Force-`sanitised` or because
    // Auto picked it by kind), the cached `mActive` now holds the
    // pre-write copy and `Active()` would lie until the next
    // `ReloadAll`. Re-resolve so the new bytes flow through
    // `ApplyTheme` + `themeChanged`. Inactive themes still skip
    // the re-apply: `ResolveAndApplyActive`'s byte-equal fast-path
    // (resolved name + theme contents both unchanged) returns
    // without emitting, so saving an unrelated user theme stays
    // surprise-free.
    if (sanitised == Instance().mActiveName)
    {
        Instance().ResolveAndApplyActive(/*emitWhenUnchanged=*/true);
    }
}

#ifdef LOGAPP_BUILD_TESTING
bool ThemeControl::IsColorSchemeForcedForTest() noexcept
{
    return Instance().mColorSchemeForced;
}

void ThemeControl::SetOsColorSchemeForTest(Qt::ColorScheme scheme) noexcept
{
    ThemeControl &self = Instance();
    self.mOsColorScheme = scheme;
    // Lock the override only when the caller passed a concrete
    // value. `Unknown` is the "reset" sentinel -- it both clears
    // the cached value and unblocks production-side refreshes
    // from `hints->colorScheme()` so the next Auto resolution
    // can fall back to real platform reporting.
    self.mOsColorSchemeLocked = (scheme != Qt::ColorScheme::Unknown);
}
#endif

void ThemeControl::DoLoadConfiguration()
{
    // One-shot capture of the process-default style + font BEFORE
    // any `ApplyTheme` mutates them. Restored later when the active
    // theme has no `app.qtStyle` / `app.font*` overrides, so a
    // switch from a font-defining theme back to one that omits the
    // field reverts to the startup defaults instead of inheriting
    // the previous theme's font.
    //
    // Also seeds `mOsColorScheme` from `QStyleHints::colorScheme()`
    // and wires the `colorSchemeChanged` signal so OS-driven scheme
    // changes (dark/light flip from the system settings panel) get
    // captured into the cache. Capture has to happen before the first
    // `ApplyColorSchemeHint` so the snapshot is the unforced OS value.
    if (!mStartupCaptured)
    {
        if (qApp->style() != nullptr)
        {
            mStartupStyleName = qApp->style()->name();
        }
        mStartupFont = qApp->font();
        if (QStyleHints *hints = QGuiApplication::styleHints(); hints != nullptr)
        {
            mOsColorScheme = hints->colorScheme();
            // The signal carries the freshly-resolved scheme, so we
            // can just funnel it into the slot. `Qt::UniqueConnection`
            // is defensive against accidental double-wires from tests
            // re-running `LoadConfiguration` (though `mStartupCaptured`
            // also gates that today).
            connect(
                hints,
                &QStyleHints::colorSchemeChanged,
                this,
                &ThemeControl::OnPlatformColorSchemeChanged,
                Qt::UniqueConnection
            );
        }
        mStartupCaptured = true;
    }

    DiscoverThemes();

    mActiveSelection = PersistedSelection();

    // First apply -- always emit so any downstream listener that
    // connected before LoadConfiguration sees the initial state.
    // Existing receivers (set up after LoadConfiguration) are
    // unaffected; the signal coalesces into a single repaint.
    ResolveAndApplyActive(/*emitWhenUnchanged=*/false);
}

void ThemeControl::DoReloadAll()
{
    DiscoverThemes();
    ResolveAndApplyActive(/*emitWhenUnchanged=*/true);
}

void ThemeControl::DoReevaluate()
{
    // Re-entrancy guard. `ApplyTheme` synchronously fires
    // `ApplicationPaletteChange` (via `qApp->setPalette`), which
    // `MainWindow::event` routes back here. At that moment
    // `qApp->palette()` already holds the theme palette we just
    // pushed (not the OS palette), so sampling it would feed the
    // auto-picker our own output. Bail before the sample so
    // re-entry can never affect resolution. The outer
    // `ResolveAndApplyActive` early-out and the `ApplyTheme`
    // guard further down both happen to catch this today, but
    // pinning the contract here keeps it robust to future
    // refactors of either path.
    if (mApplyingTheme)
    {
        return;
    }
    // In Force mode the user explicitly picked a theme; an OS
    // dark/light flip must not override that choice. Skip
    // entirely so we also avoid re-running `ApplyTheme` (which
    // would re-push the palette and re-trigger the very event
    // that brought us here).
    if (!mActiveSelection.isEmpty())
    {
        return;
    }
    ResolveAndApplyActive(/*emitWhenUnchanged=*/false);
}

void ThemeControl::DoSetActiveSelection(const QString &nameOrAuto)
{
    if (nameOrAuto == mActiveSelection)
    {
        return;
    }
    mActiveSelection = nameOrAuto;
    ResolveAndApplyActive(/*emitWhenUnchanged=*/false);
}

void ThemeControl::DiscoverThemes()
{
    mIndex.clear();

    // Single ingest helper shared between the built-in resource
    // pass and the user-dir pass. When the JSON's `name` field is
    // empty we fall back to the file basename and emit a
    // `qWarning`, regardless of source: a corrupted built-in
    // resource and a hand-edited user file have the same failure
    // mode, and the basename fallback at least keeps the entry
    // discoverable instead of silently dropping it.
    //
    // Within the user-dir pass we also warn when a second file
    // lands on an already-taken `name` so the silent overwrite
    // doesn't go unnoticed; we then skip the overwrite so the
    // first file (alphabetical order, see below) wins
    // deterministically.
    auto ingest = [&](const QString &path, bool fromUser) {
        auto theme = ParseFileToTheme(path);
        if (!theme.has_value())
        {
            return;
        }
        QString name = QString::fromStdString(theme->name);
        if (name.isEmpty())
        {
            name = QFileInfo(path).completeBaseName();
            qWarning(
                "%s theme %s has no `name` field; using file basename %s. Add a "
                "`name` entry to suppress this warning.",
                fromUser ? "User" : "Built-in",
                qUtf8Printable(path),
                qUtf8Printable(name)
            );
            theme->name = name.toStdString();
        }
        if (fromUser)
        {
            if (const auto existing = mIndex.find(name); existing != mIndex.end() && existing->second.fromUser)
            {
                qWarning(
                    "User theme name %s is already taken by a previously-loaded file; the "
                    "second file %s is ignored. Rename one of the two `name` entries to "
                    "make both discoverable.",
                    qUtf8Printable(name),
                    qUtf8Printable(path)
                );
                return;
            }
        }
        WarnOnUnknownLevelKeys(path, *theme);
        mIndex[name] = IndexEntry{.theme = std::move(*theme), .fromUser = fromUser};
    };

    // Built-ins first, so a same-named user file overrides them.
    constexpr std::array<const char *, 8> BUILTIN_PATHS = {
        BUILTIN_LIGHT_PATH,
        BUILTIN_DARK_PATH,
        BUILTIN_GITHUB_DARK_PATH,
        BUILTIN_GITHUB_LIGHT_PATH,
        BUILTIN_MATERIAL_DARK_PATH,
        BUILTIN_MATERIAL_LIGHT_PATH,
        BUILTIN_MONOKAI_DARK_PATH,
        BUILTIN_MONOKAI_LIGHT_PATH
    };
    for (const char *path : BUILTIN_PATHS)
    {
        ingest(QString::fromLatin1(path), /*fromUser=*/false);
    }

    // User dir entries shadow built-ins by name. Sort the listing
    // by basename first so the "first file in alphabetical order
    // wins" tie-breaker for `name` collisions is deterministic
    // across platforms (`QDir::entryList` order is filesystem-
    // dependent without an explicit sort flag).
    const QDir userDir = UserThemesDir();
    const QStringList userFiles =
        userDir.entryList(QStringList{QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    for (const QString &file : userFiles)
    {
        ingest(userDir.filePath(file), /*fromUser=*/true);
    }
}

void ThemeControl::ResolveAndApplyActive(bool emitWhenUnchanged)
{
    loglib::Theme chosen;
    bool found = false;

    if (!mActiveSelection.isEmpty())
    {
        if (const auto it = mIndex.find(mActiveSelection); it != mIndex.end())
        {
            chosen = it->second.theme;
            found = true;
        }
        else
        {
            // Stale selection (theme JSON deleted, settings copied
            // from another machine, ...): coerce in-memory state
            // back to Auto so callers asking `ActiveSelection()`
            // see the same value they just saw resolve. We do NOT
            // call `SaveConfiguration` here -- only user-driven
            // saves should write `QSettings`, so the on-disk value
            // stays as-is until the next Ok in the Preferences
            // dialog. This matches what `RepopulateThemeCombo`
            // used to do at the UI layer (now redundant).
            qInfo("Theme selection %s no longer exists; reverting to Auto.", qUtf8Printable(mActiveSelection));
            mActiveSelection.clear();
        }
    }
    if (!found)
    {
        // Auto (or selection was just coerced above): pick by the
        // cached `Qt::ColorScheme`. Before sampling, if we currently
        // hold a Force-mode `setColorScheme()` override, release it
        // and re-read so the cache reflects the actual OS state
        // (Qt suppresses OS-driven `colorScheme()` updates while we
        // hold an override -- so the cache could be many minutes
        // stale if the user toggled the OS theme during a Force
        // session). We gate the unset with `mApplyingTheme` so the
        // resulting palette-change events don't recurse into us via
        // `MainWindow::event` -> `Reevaluate` -> here.
        if (mColorSchemeForced)
        {
            if (QStyleHints *hints = QGuiApplication::styleHints(); hints != nullptr)
            {
                const bool wasApplying = mApplyingTheme;
                mApplyingTheme = true;
                hints->unsetColorScheme();
                mColorSchemeForced = false;
#ifdef LOGAPP_BUILD_TESTING
                // Tests can pre-pin `mOsColorScheme` via the test
                // helper; honour the lock so the production-side
                // refresh below doesn't clobber the fake with
                // whatever the test host's real platform reports.
                if (!mOsColorSchemeLocked)
#endif
                {
                    mOsColorScheme = hints->colorScheme();
                }
                mApplyingTheme = wasApplying;
            }
        }
        const bool osDark = (mOsColorScheme == Qt::ColorScheme::Dark);
        const QString desiredName = osDark ? QString::fromLatin1(BUILTIN_DARK_NAME)
                                           : QString::fromLatin1(BUILTIN_LIGHT_NAME);
        if (const auto it = mIndex.find(desiredName); it != mIndex.end())
        {
            chosen = it->second.theme;
            found = true;
        }
    }
    if (!found && !mIndex.empty())
    {
        // Last-resort: take whatever's in the index. Keeps the app
        // usable when resources are missing in a corrupted install.
        chosen = mIndex.begin()->second.theme;
        found = true;
    }
    if (!found)
    {
        // Nothing discoverable; leave caches empty so the model
        // falls back to palette defaults.
        return;
    }

    const QString newName = QString::fromStdString(chosen.name);
    const bool unchanged = (newName == mActiveName);

    // Selection-mode flip (Force <-> Auto) requires re-running
    // `ApplyColorSchemeHint` even when the resolved theme is the
    // same: in Force mode it pins `QStyleHints::colorScheme`, in
    // Auto mode it unpins. Without this, switching from Force
    // "Light" to Auto on a light system would leave Qt's color
    // scheme pinned and the OS dark/light flip would no longer
    // drive `ApplicationPaletteChange`.
    const bool selectionModeChanged = mActiveSelection.isEmpty() != mAppliedSelection.isEmpty();

    // Skip the full re-apply when the resolved theme name hasn't
    // changed and the caller doesn't insist. This is the second
    // line of defence against the palette-change feedback loop:
    // `qApp->setPalette` fires `ApplicationPaletteChange` ->
    // `MainWindow::event` -> `Reevaluate` -> here.
    if (unchanged && !emitWhenUnchanged)
    {
        if (selectionModeChanged)
        {
            // Resolved theme stayed the same but the selection mode
            // flipped; re-run just the colour-scheme hint so Qt's
            // `QStyleHints` state matches the new mode. Palette /
            // brush cache are unaffected so we skip them.
            ApplyColorSchemeHint(mActive);
            mAppliedSelection = mActiveSelection;
        }
        return;
    }

    // `emitWhenUnchanged` path (ReloadAll / explicit re-apply):
    // when both the resolved name *and* every field of the new
    // theme matches the active one, skip the expensive
    // `ApplyTheme` (style + palette + cache rebuild) and the
    // `themeChanged` fan-out. Reload-from-disk with no edits is
    // a no-op; users who edited a file see the signal as expected.
    if (unchanged && emitWhenUnchanged && chosen == mActive && !selectionModeChanged)
    {
        return;
    }

    mActive = std::move(chosen);
    mActiveName = newName;
    mAppliedSelection = mActiveSelection;
    ApplyTheme(mActive);
    if (!unchanged || emitWhenUnchanged)
    {
        emit themeChanged();
    }
}

void ThemeControl::ApplyTheme(const loglib::Theme &theme)
{
    // First-line re-entrancy guard. `qApp->setStyle` /
    // `qApp->setPalette` fire `QEvent::StyleChange` /
    // `ApplicationPaletteChange` which `MainWindow::event` routes
    // back to `ThemeControl::Reevaluate`. The `unchanged` check in
    // `ResolveAndApplyActive` already prevents re-entry from
    // doing real work, but bailing early here also avoids
    // rebuilding caches on the bounce-back.
    if (mApplyingTheme)
    {
        return;
    }
    mApplyingTheme = true;

    // Order matters: set the style FIRST so the standard palette
    // we then mutate is the new style's standard palette, not the
    // previous style's. Without this, switching from windows11 ->
    // fusion on a dark-mode Windows system would briefly leave the
    // dark windows11 palette in place even after we wrote the
    // theme-derived light palette over it.
    //
    // When the active theme omits `app.qtStyle` (or names a style
    // `QStyleFactory::create` can't resolve), fall back to the
    // startup snapshot taken in `DoLoadConfiguration`. Without
    // this, switching from a theme that pinned `qtStyle: "fusion"`
    // back to one that omits it would leave Fusion in place
    // forever, contradicting the docstring on `AppStyle::qtStyle`.
    QString targetStyleName;
    if (theme.app.qtStyle.has_value() && !theme.app.qtStyle->empty())
    {
        targetStyleName = QString::fromStdString(*theme.app.qtStyle);
    }
    else
    {
        targetStyleName = mStartupStyleName;
    }
    if (!targetStyleName.isEmpty() && qApp->style() != nullptr &&
        qApp->style()->name().compare(targetStyleName, Qt::CaseInsensitive) != 0)
    {
        if (QStyle *newStyle = QStyleFactory::create(targetStyleName); newStyle != nullptr)
        {
            qApp->setStyle(newStyle);
        }
    }

    // Then push the colour-scheme hint so platform-native widgets
    // (Windows menu bar, native dialogs, anything that consults
    // `QStyleHints::colorScheme` rather than just `QPalette`)
    // follow the active theme. The explicit `setColorScheme` call
    // also makes Qt 6.8+ rebuild its default palette in the right
    // colour scheme BEFORE we layer our overrides; without it,
    // forcing Light on a dark-mode Windows system left platform
    // chrome painted dark. In Auto mode we restore the OS default
    // via `unsetColorScheme` so a system dark/light flip
    // continues to drive `ApplicationPaletteChange` (which our
    // event handler routes back into `Reevaluate`).
    ApplyColorSchemeHint(theme);

    ApplyPalette(theme);

    // Font: same contract as `qtStyle` -- when the theme omits the
    // override, restore the startup font instead of letting the
    // previous theme's font carry through. The `app.fontFamily` /
    // `app.fontSize` paths overlay onto the startup font so a
    // theme that customises only the size keeps the startup family.
    const bool hasFamily = theme.app.fontFamily.has_value() && !theme.app.fontFamily->empty();
    const bool hasSize = theme.app.fontSize.has_value() && *theme.app.fontSize > 0;
    if (hasFamily || hasSize)
    {
        QFont font = mStartupFont;
        if (hasFamily)
        {
            font.setFamily(QString::fromStdString(*theme.app.fontFamily));
        }
        if (hasSize)
        {
            font.setPointSize(*theme.app.fontSize);
        }
        if (qApp->font() != font)
        {
            qApp->setFont(font);
        }
    }
    else if (qApp->font() != mStartupFont)
    {
        qApp->setFont(mStartupFont);
    }

    // Build the brush + font cache LAST so it sees the
    // freshly-pushed `qApp->font()`. `LogModel::data` reads from
    // these arrays per cell and they must agree with the
    // application font in effect at paint time.
    BuildStyleCache(theme);

    mApplyingTheme = false;
}

void ThemeControl::ApplyColorSchemeHint([[maybe_unused]] const loglib::Theme &theme)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    QStyleHints *hints = QGuiApplication::styleHints();
    if (hints == nullptr)
    {
        return;
    }

    // Auto mode: drop any prior Force-mode override so Qt tracks
    // the system setting again. The auto-resolved theme's `kind`
    // already matches the current OS scheme (we picked it from
    // `mOsColorScheme` in `ResolveAndApplyActive`), so we don't
    // need to set anything here. Crucially, leaving the scheme
    // unset is what lets a later system dark/light flip fire
    // `colorSchemeChanged` through to `OnPlatformColorSchemeChanged`.
    //
    // We track `mColorSchemeForced` ourselves because after
    // `unsetColorScheme()` Qt reports `colorScheme()` as the
    // system's current value, not `Unknown`, so we can't recover
    // "is this currently a forced override?" from Qt alone.
    if (mActiveSelection.isEmpty())
    {
        if (mColorSchemeForced)
        {
            hints->unsetColorScheme();
            mColorSchemeForced = false;
#ifdef LOGAPP_BUILD_TESTING
            if (!mOsColorSchemeLocked)
#endif
            {
                // Refresh the cache from the now-unforced OS value
                // so a subsequent Auto re-resolution doesn't drift
                // back through a stale `mOsColorScheme`. The Auto
                // picker already does this preemptively, but
                // mirroring it here keeps the cache invariant tight.
                mOsColorScheme = hints->colorScheme();
            }
        }
        return;
    }

    // Force mode: snapshot the current OS scheme into the cache
    // BEFORE we install the override. Once `setColorScheme()` runs,
    // Qt suppresses OS-driven scheme updates and `colorScheme()`
    // returns our forced value -- so if we wait until "after",
    // we lose the OS state and the next Auto resolution would
    // pick the wrong kind. Skipped when we already hold an
    // override (the cache is whatever it was when we entered
    // Force mode the first time).
    if (!mColorSchemeForced)
    {
#ifdef LOGAPP_BUILD_TESTING
        if (!mOsColorSchemeLocked)
#endif
        {
            mOsColorScheme = hints->colorScheme();
        }
    }

    // Then explicit override so Qt's standard palette and
    // platform-native chrome line up with the user's choice
    // regardless of the OS setting. Per QStyleHints docs this
    // also makes Qt ignore later system colour-scheme changes,
    // which is exactly the contract Force mode promises.
    const Qt::ColorScheme target =
        (theme.kind == loglib::ThemeKind::Dark) ? Qt::ColorScheme::Dark : Qt::ColorScheme::Light;
    if (!mColorSchemeForced || hints->colorScheme() != target)
    {
        hints->setColorScheme(target);
        mColorSchemeForced = true;
    }
#endif
}

void ThemeControl::ApplyPalette(const loglib::Theme &theme)
{
    // Build a fully-formed palette per theme kind so that on a
    // dark-mode OS with a forced Light theme (and vice versa) the
    // QPalette agrees with `theme.table.background`. Without this
    // the table renders Info-level cells with the OS palette's
    // `QPalette::Text` (e.g. white on Windows dark mode) against
    // the theme's `#FFFFFF` Base, producing invisible rows -- and
    // worse, Qt does extra work reconciling the conflicting QSS
    // and palette on every paint, which shows up as a parsing
    // slowdown on multi-million-row tables.
    const bool dark = (theme.kind == loglib::ThemeKind::Dark);

    auto hexOr = [](const std::optional<std::string> &hex, const QColor &fallback) {
        if (!hex.has_value() || hex->empty())
        {
            return fallback;
        }
        const QColor color(QString::fromStdString(*hex));
        return color.isValid() ? color : fallback;
    };

    // Defaults derived from the theme kind. These match the
    // built-in Light/Dark JSON values so themes that only define a
    // subset still produce a self-consistent palette. Each role is
    // overridable through `theme.chrome` / `theme.table`; absent
    // fields fall back to the corresponding kind-specific default
    // below.
    const QColor base = hexOr(theme.table.background, dark ? QColor(0x22, 0x22, 0x22) : QColor(0xFF, 0xFF, 0xFF));
    const QColor altBase =
        hexOr(theme.table.alternateRowBackground, dark ? QColor(0x2A, 0x2A, 0x2A) : QColor(0xF3, 0xF4, 0xF6));
    const QColor text = hexOr(theme.chrome.text, dark ? QColor(0xE5, 0xE7, 0xEB) : QColor(0x0F, 0x17, 0x2A));
    const QColor window = hexOr(theme.chrome.window, dark ? QColor(0x2A, 0x2A, 0x2A) : QColor(0xF7, 0xF7, 0xF7));
    const QColor windowText =
        hexOr(theme.chrome.windowText, dark ? QColor(0xE5, 0xE7, 0xEB) : QColor(0x0F, 0x17, 0x2A));
    const QColor button = hexOr(theme.chrome.button, dark ? QColor(0x37, 0x41, 0x51) : QColor(0xF3, 0xF4, 0xF6));
    const QColor buttonText =
        hexOr(theme.chrome.buttonText, dark ? QColor(0xE5, 0xE7, 0xEB) : QColor(0x0F, 0x17, 0x2A));
    const QColor placeholder =
        hexOr(theme.chrome.placeholderText, dark ? QColor(0x9C, 0xA3, 0xAF) : QColor(0x6B, 0x72, 0x80));
    const QColor toolTipBase = hexOr(theme.chrome.toolTipBase, window);
    const QColor toolTipText = hexOr(theme.chrome.toolTipText, text);
    const QColor highlight =
        hexOr(theme.table.selectionBackground, dark ? QColor(0x00, 0x51, 0x8F) : QColor(0xAD, 0xD4, 0xFF));
    const QColor highlightedText =
        hexOr(theme.table.selectionForeground, dark ? QColor(0xFF, 0xFF, 0xFF) : QColor(0x00, 0x00, 0x00));

    // Start from the style's standard palette so style-specific
    // bits (Mid, Shadow, Light, etc.) carry through, then override
    // the roles we actually care about.
    QPalette palette = qApp->style()->standardPalette();

    palette.setColor(QPalette::Base, base);
    palette.setColor(QPalette::AlternateBase, altBase);
    palette.setColor(QPalette::Text, text);
    palette.setColor(QPalette::Window, window);
    palette.setColor(QPalette::WindowText, windowText);
    palette.setColor(QPalette::Button, button);
    palette.setColor(QPalette::ButtonText, buttonText);
    palette.setColor(QPalette::ToolTipBase, toolTipBase);
    palette.setColor(QPalette::ToolTipText, toolTipText);
    palette.setColor(QPalette::PlaceholderText, placeholder);

    // Apply highlight to both the Active and Inactive colour
    // groups so a focused-elsewhere window keeps the same
    // selection colour the user sees while interacting. Without
    // this Qt grays the selection out when the window loses
    // focus, which previously required QSS to override.
    // Other roles (Base, AlternateBase, Text, Window, WindowText)
    // are already written across all three groups by the
    // group-less `setColor(role, c)` calls above (that overload
    // is shorthand for "all groups"), so re-writing them per
    // group would be redundant.
    for (QPalette::ColorGroup group : {QPalette::Active, QPalette::Inactive})
    {
        palette.setColor(group, QPalette::Highlight, highlight);
        palette.setColor(group, QPalette::HighlightedText, highlightedText);
    }

    // Disabled colour group: every group-less `setColor(role, c)`
    // call above also wrote `c` into the Disabled group (Qt's API
    // shorthand for "all groups"), which made disabled menu items,
    // buttons, etc. read identically to enabled ones. Override the
    // text-bearing roles here with values blended toward the
    // surrounding surface so disabled chrome dims visibly without
    // disappearing.
    //
    // We hand-roll the blend instead of reaching for
    // `QPalette::setColorGroup(QPalette::Disabled, ...)`: that
    // overload demands a positional `QBrush` per role (17 of
    // them, including roles we want to leave at full strength),
    // which would produce a less readable call site than the
    // small loop below. `QColor::fromRgbF` linear interpolation
    // is good enough at the 0.55 / 0.40 mix factors we use --
    // perceptual (HSL / Lab) interpolation isn't worth the
    // extra cost for "obviously disabled" feedback.
    constexpr float DISABLED_TEXT_MIX = 0.55F;
    constexpr float DISABLED_HIGHLIGHT_MIX = 0.40F;
    const QColor disabledHighlight = BlendTowards(highlight, window, DISABLED_HIGHLIGHT_MIX);

    struct DisabledOverride
    {
        QPalette::ColorRole role{QPalette::NoRole};
        QColor foreground;
        QColor background;
        float mix{0.0F};
    };
    const std::array<DisabledOverride, 7> textOverrides = {
        {{QPalette::Text, text, base, DISABLED_TEXT_MIX},
         {QPalette::WindowText, windowText, window, DISABLED_TEXT_MIX},
         {QPalette::ButtonText, buttonText, button, DISABLED_TEXT_MIX},
         // ToolTipText must blend toward `toolTipBase`, NOT `base`:
         // the `ChromeStyle` docstring in `theme.hpp` pins every
         // text role to its own backing surface for the Disabled
         // group, so a theme that customises `chrome.toolTipBase`
         // (or `chrome.toolTipText`) to values distinct from
         // `text`/`base` still gets a self-consistent disabled
         // tooltip rather than dimming the body-text foreground
         // toward the table-body surface.
         {QPalette::ToolTipText, toolTipText, toolTipBase, DISABLED_TEXT_MIX},
         {QPalette::PlaceholderText, placeholder, base, DISABLED_TEXT_MIX},
         {QPalette::Highlight, highlight, window, DISABLED_HIGHLIGHT_MIX},
         {QPalette::HighlightedText, highlightedText, disabledHighlight, DISABLED_TEXT_MIX}}
    };
    for (const DisabledOverride &entry : textOverrides)
    {
        palette.setColor(QPalette::Disabled, entry.role, BlendTowards(entry.foreground, entry.background, entry.mix));
    }
    // Surface-bearing roles stay at full strength so the chrome
    // itself remains recognisable; only the text-on-top dims.
    palette.setColor(QPalette::Disabled, QPalette::Base, base);
    palette.setColor(QPalette::Disabled, QPalette::AlternateBase, altBase);
    palette.setColor(QPalette::Disabled, QPalette::Window, window);
    palette.setColor(QPalette::Disabled, QPalette::Button, button);
    palette.setColor(QPalette::Disabled, QPalette::ToolTipBase, toolTipBase);

    qApp->setPalette(palette);
}

void ThemeControl::BuildStyleCache(const loglib::Theme &theme)
{
    mForeground.fill(QBrush{});
    mBackground.fill(QBrush{});
    mBold.fill(false);
    mItalic.fill(false);
    mHasAnyFontStyle = false;

    // Snapshot `qApp->font()` once -- `ApplyTheme` calls us after
    // `qApp->setFont` lands, so this picks up the theme-applied
    // family / size. Per-level fonts overlay bold / italic on top.
    const QFont appFont = qApp->font();
    mFonts.fill(appFont);

    using loglib::LogLevel;
    // Iterate every enum value the model can produce, including the
    // `Unknown` sentinel. Themes that want to tint rows whose level
    // string didn't resolve via `ResolveLevel` can add a
    // `"Unknown"` entry to their `levels` map; absent ones leave
    // the brushes invalid and the model falls back to palette
    // defaults.
    constexpr std::array<LogLevel, loglib::CANONICAL_LEVEL_COUNT + 1> ALL_LEVELS = {
        LogLevel::Unknown, LogLevel::Trace, LogLevel::Debug, LogLevel::Info,
        LogLevel::Warn,    LogLevel::Error, LogLevel::Fatal
    };
    for (const LogLevel level : ALL_LEVELS)
    {
        const loglib::LevelStyle style = loglib::StyleForLevel(theme, level);
        const size_t idx = LevelIndex(level);
        mForeground[idx] = BrushFromHex(style.foreground);
        mBackground[idx] = BrushFromHex(style.background);
        mBold[idx] = style.bold;
        mItalic[idx] = style.italic;
        if (style.bold)
        {
            mFonts[idx].setBold(true);
        }
        if (style.italic)
        {
            mFonts[idx].setItalic(true);
        }
        mHasAnyFontStyle = mHasAnyFontStyle || style.bold || style.italic;
    }
}
