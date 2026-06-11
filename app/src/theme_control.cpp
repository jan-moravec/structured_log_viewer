#include "theme_control.hpp"

#include "icon_loader.hpp"

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
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>

namespace
{

constexpr char SETTINGS_KEY_ACTIVE[] = "theme/active";

/// Fallback anchor palette used when the active theme's slot is
/// empty. Eight saturated, hue-distinct entries; readable on both
/// light and dark chrome.
constexpr std::array<const char *, loglib::ANCHOR_PALETTE_SIZE> ANCHOR_FALLBACK_PALETTE = {
    "#B91C1C",
    "#C2410C",
    "#A16207",
    "#15803D",
    "#0F766E",
    "#0369A1",
    "#7E22CE",
    "#BE185D",
};

constexpr char BUILTIN_LIGHT_NAME[] = "Light";
constexpr char BUILTIN_DARK_NAME[] = "Dark";

// `Light` and `Dark` are the canonical Auto-mode targets (matched
// by name in `ResolveAndApplyActive`); the rest are presets the
// user can force-select.
constexpr char BUILTIN_LIGHT_PATH[] = ":/themes/light.json";
constexpr char BUILTIN_DARK_PATH[] = ":/themes/dark.json";
constexpr char BUILTIN_GITHUB_DARK_PATH[] = ":/themes/github_dark.json";
constexpr char BUILTIN_GITHUB_LIGHT_PATH[] = ":/themes/github_light.json";
constexpr char BUILTIN_MATERIAL_DARK_PATH[] = ":/themes/material_dark.json";
constexpr char BUILTIN_MATERIAL_LIGHT_PATH[] = ":/themes/material_light.json";
constexpr char BUILTIN_MONOKAI_DARK_PATH[] = ":/themes/monokai_dark.json";
constexpr char BUILTIN_MONOKAI_LIGHT_PATH[] = ":/themes/monokai_light.json";

/// Linear `lerp(@p fg, @p bg, @p t)` in sRGB. Used to dim Disabled
/// text roles toward their surrounding surface.
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

QBrush BrushFromHex(const std::optional<std::string> &hex)
{
    if (!hex.has_value() || hex->empty())
    {
        return QBrush{}; // invalid -> "use palette default"
    }
    const QString hexQ = QString::fromStdString(*hex);
    const QColor color(hexQ);
    if (!color.isValid())
    {
        // A non-empty hex that fails to parse is almost always a
        // typo (e.g. `#XYZ`, missing `#`, wrong length). Silent
        // fallback was previously masking these; surface the
        // offender so users can spot the bad field in their theme
        // JSON. Not throwing -- a malformed colour shouldn't
        // refuse the rest of the theme.
        qWarning("Theme hex colour `%s` did not parse; using palette default.", qUtf8Printable(hexQ));
        return QBrush{};
    }
    return QBrush{color};
}

/// Read a UTF-8 text file via Qt so resource paths (`:/...`) work
/// alongside on-disk paths. Returns nullopt on open failure.
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

using loglib::LogLevel;

constexpr std::array<LogLevel, loglib::CANONICAL_LEVEL_COUNT + 1> ALL_LEVELS = {
    LogLevel::Unknown,
    LogLevel::Trace,
    LogLevel::Debug,
    LogLevel::Info,
    LogLevel::Warn,
    LogLevel::Error,
    LogLevel::Fatal
};

bool IsCanonicalLevelKey(const std::string &key) noexcept
{
    return std::ranges::any_of(ALL_LEVELS, [&key](LogLevel level) {
        return std::string_view(key) == loglib::CanonicalLevelName(level);
    });
}

/// Warn about non-canonical level keys (e.g. typos like `"Worn"`)
/// so users can tell whether their override was ignored. Walks
/// both `theme.levels` and the optional `levelColumnOverride.levels`
/// so a typo in either block surfaces the same way.
void WarnOnUnknownLevelKeys(const QString &source, const loglib::Theme &theme)
{
    for (const auto &[key, value] : theme.levels)
    {
        if (!IsCanonicalLevelKey(key))
        {
            qWarning(
                "Theme %s has unrecognised level key `%s`; expected one of "
                "Trace/Debug/Info/Warn/Error/Fatal/Unknown. The entry is ignored.",
                qUtf8Printable(source),
                key.c_str()
            );
        }
    }
    if (theme.levelColumnOverride.has_value())
    {
        for (const auto &[key, value] : theme.levelColumnOverride->levels)
        {
            if (!IsCanonicalLevelKey(key))
            {
                qWarning(
                    "Theme %s has unrecognised levelColumnOverride.levels key `%s`; "
                    "expected one of Trace/Debug/Info/Warn/Error/Fatal/Unknown. "
                    "The entry is ignored.",
                    qUtf8Printable(source),
                    key.c_str()
                );
            }
        }
    }
}

/// Resolve a level-icon path according to the rules in plan
/// section 3. Returns an empty `QString` (with a `qWarning`) for
/// paths that are rejected (path traversal, escape from base).
///
/// Inputs:
///   - `relativeOrAbsolute`: as written in the theme JSON.
///   - `sourceDir`: the source dir of the theme that referenced
///     the icon (`":/themes"` for built-ins, an absolute path for
///     user themes).
///   - `fromUser`: gates rule 2 (absolute paths) and rule 4
///     (path-traversal rejection); built-in JSON should only ship
///     `:/...` paths.
QString ResolveIconPath(const std::string &relativeOrAbsolute, const QString &sourceDir, bool fromUser)
{
    QString path = QString::fromStdString(relativeOrAbsolute);
    if (path.isEmpty())
    {
        return {};
    }
    // Rule 1: Qt resource path. Allowed for both built-in and user
    // themes; the qrc namespace is shared.
    if (path.startsWith(QLatin1String(":/")))
    {
        return path;
    }
    // Rule 2: absolute paths. Allowed only for user themes; the
    // shipped built-in JSON should never need this.
    const bool isAbsolute = QFileInfo(path).isAbsolute();
    if (isAbsolute)
    {
        if (!fromUser)
        {
            qWarning(
                "Built-in theme references absolute icon path %s; expected a `:/...` "
                "resource path. Icon ignored.",
                qUtf8Printable(path)
            );
            return {};
        }
        return path;
    }
    // Rule 4 (applied before rule 3): reject `..` and any path
    // whose canonical form escapes the resolution base. Not a
    // security boundary -- we only render the SVG -- but it stops
    // a shared theme JSON from confused-deputy reaching files the
    // importer didn't expect.
    if (fromUser)
    {
        // `QDir::cleanPath` collapses `./` and `..` segments; if any
        // remain they were trying to escape.
        const QString cleaned = QDir::cleanPath(path);
        if (cleaned.startsWith(QStringLiteral("../")) || cleaned == QStringLiteral("..") ||
            cleaned.contains(QStringLiteral("/../")))
        {
            qWarning(
                "User theme icon path %s uses parent-directory traversal; "
                "icon ignored.",
                qUtf8Printable(path)
            );
            return {};
        }
    }
    // Rule 3: resolved against the theme file's directory.
    if (sourceDir.isEmpty())
    {
        return path;
    }
    return QDir(sourceDir).filePath(path);
}

bool IsReservedWin32DeviceName(const QString &name)
{
    // Win32 reserves these names (with or without an extension);
    // opening one yields a device handle instead of a file.
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
        // Swallow parse errors at scan time so one broken file
        // cannot wedge the app on startup.
        return std::nullopt;
    }
}

} // namespace

ThemeControl::ThemeControl(QObject *parent)
    : QObject(parent)
{
    LoadConfiguration();
}

QString ThemeControl::PersistedSelection() const
{
    const QSettings settings;
    const QVariant raw = settings.value(QLatin1String(SETTINGS_KEY_ACTIVE));
    return raw.isValid() ? raw.toString() : QString();
}

void ThemeControl::SaveConfiguration() const
{
    QSettings settings;
    settings.setValue(QLatin1String(SETTINGS_KEY_ACTIVE), mActiveSelection);
}

void ThemeControl::Reevaluate()
{
    // Re-entrancy guard: `ApplyTheme` fires palette events that
    // round-trip back here via `MainWindow::event`.
    if (mApplyingTheme)
    {
        return;
    }
    // Force mode honours the user's pick regardless of OS scheme.
    if (!mActiveSelection.isEmpty())
    {
        return;
    }
    ResolveAndApplyActive(/*emitWhenUnchanged=*/false);
}

bool ThemeControl::IsApplyingTheme() const noexcept
{
    return mApplyingTheme;
}

void ThemeControl::OnPlatformColorSchemeChanged(Qt::ColorScheme scheme)
{
    // Skip self-induced changes from `set/unsetColorScheme`.
    if (mApplyingTheme)
    {
        return;
    }
    // While a Force override is held, Qt reports our forced value
    // here, not the OS's -- caching it would defeat the cache.
    if (mColorSchemeForced)
    {
        return;
    }
    mOsColorScheme = scheme;
}

void ThemeControl::ReloadAll()
{
    DiscoverThemes();
    ResolveAndApplyActive(/*emitWhenUnchanged=*/true);
}

const loglib::Theme &ThemeControl::Active() const
{
    return mActive;
}

QBrush ThemeControl::ForegroundFor(loglib::LogLevel level) const noexcept
{
    const size_t idx = LevelIndex(level);
    if (idx >= mForeground.size())
    {
        return {};
    }
    return mForeground[idx];
}

QBrush ThemeControl::BackgroundFor(loglib::LogLevel level) const noexcept
{
    const size_t idx = LevelIndex(level);
    if (idx >= mBackground.size())
    {
        return {};
    }
    return mBackground[idx];
}

QFont ThemeControl::FontFor(loglib::LogLevel level) const noexcept
{
    const size_t idx = LevelIndex(level);
    if (idx >= mFonts.size())
    {
        return qApp->font();
    }
    return mFonts[idx];
}

bool ThemeControl::HasFontStyle(loglib::LogLevel level) const noexcept
{
    const size_t idx = LevelIndex(level);
    if (idx >= mBold.size())
    {
        return false;
    }
    return mBold[idx] || mItalic[idx];
}

bool ThemeControl::HasAnyFontStyle() const noexcept
{
    return mHasAnyFontStyle;
}

QBrush ThemeControl::AnchorBrushFor(std::uint8_t colorIndex, int role) const noexcept
{
    if (colorIndex >= loglib::ANCHOR_PALETTE_SIZE)
    {
        return {};
    }
    switch (role)
    {
    case Qt::BackgroundRole:
        return mAnchorBackground[colorIndex];
    case Qt::ForegroundRole:
        return mAnchorForeground[colorIndex];
    default:
        // Other roles have no anchor meaning; invalid brush lets
        // the caller fall through to its normal handling.
        return {};
    }
}

bool ThemeControl::HasLevelColumnOverride() const noexcept
{
    return mHasLevelColumnOverride;
}

QIcon ThemeControl::IconFor(loglib::LogLevel level) const noexcept
{
    const size_t idx = LevelIndex(level);
    if (idx >= mLevelIcons.size())
    {
        return {};
    }
    return mLevelIcons[idx];
}

QBrush ThemeControl::PillBackgroundFor(loglib::LogLevel level) const noexcept
{
    const size_t idx = LevelIndex(level);
    if (idx >= mPillBackground.size())
    {
        return {};
    }
    return mPillBackground[idx];
}

QBrush ThemeControl::PillForegroundFor(loglib::LogLevel level) const noexcept
{
    const size_t idx = LevelIndex(level);
    if (idx >= mPillForeground.size())
    {
        return {};
    }
    return mPillForeground[idx];
}

std::optional<QString> ThemeControl::LevelColumnHeaderTextOverride() const
{
    return mLevelColumnHeaderText;
}

QIcon ThemeControl::LevelColumnHeaderIcon() const
{
    return mLevelColumnHeaderIcon;
}

QString ThemeControl::ActiveSelection() const
{
    return mActiveSelection;
}

void ThemeControl::SetActiveSelection(const QString &nameOrAuto)
{
    if (nameOrAuto == mActiveSelection)
    {
        return;
    }
    mActiveSelection = nameOrAuto;
    ResolveAndApplyActive(/*emitWhenUnchanged=*/false);
}

QList<ThemeControl::ThemeListing> ThemeControl::AvailableThemes() const
{
    QList<ThemeListing> out;
    out.reserve(static_cast<int>(mIndex.size()));
    for (const auto &[name, entry] : mIndex)
    {
        out.append(ThemeListing{.name = name, .kind = entry.theme.kind, .fromUser = entry.fromUser});
    }
    return out;
}

std::optional<loglib::Theme> ThemeControl::Load(const QString &name) const
{
    const auto it = mIndex.find(name);
    if (it == mIndex.end())
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
    const QDir dir(themesPath);
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
    // ITU-R BT.601 luma (`0.299*R + 0.587*G + 0.114*B`) vs mid-grey.
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
        throw std::runtime_error(R"(Theme name must not be "." or "..")");
    }
    if (name.contains(QStringLiteral("..")))
    {
        throw std::runtime_error("Theme name must not contain \"..\"");
    }
    // Reject path separators, invalid-on-some-platform filename
    // characters, and ASCII control bytes.
    constexpr char16_t FIRST_PRINTABLE_ASCII = 0x20U;
    for (const QChar ch : name)
    {
        if (ch == QLatin1Char('/') || ch == QLatin1Char('\\') || ch == QLatin1Char(':') || ch == QLatin1Char('<') ||
            ch == QLatin1Char('>') || ch == QLatin1Char('"') || ch == QLatin1Char('|') || ch == QLatin1Char('?') ||
            ch == QLatin1Char('*') || ch.unicode() < FIRST_PRINTABLE_ASCII)
        {
            throw std::runtime_error("Theme name contains invalid character");
        }
    }
    if (IsReservedWin32DeviceName(name))
    {
        throw std::runtime_error("Theme name matches a reserved Win32 device name");
    }
    // Win32 strips trailing `.` / ` ` on create, which would let
    // "Dark." silently shadow the built-in "Dark".
    if (name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' ')))
    {
        throw std::runtime_error("Theme name must not end with a dot or space");
    }
    return name;
}

void ThemeControl::SaveUserTheme(const QString &name, loglib::Theme theme)
{
    const QString sanitised = SanitiseThemeName(name);

    // Pin on-disk `name` so the index key matches even after the
    // file is renamed on disk.
    theme.name = sanitised.toStdString();
    const std::string json = loglib::SerializeTheme(theme);

    const QDir dir = UserThemesDir();
    const QString path = dir.filePath(sanitised + QStringLiteral(".json"));

    // `QSaveFile` writes atomically (temp file + rename on commit)
    // and handles UTF-8 paths on Windows, unlike `std::ofstream`.
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

    // Refresh the index so the new entry is immediately visible.
    DiscoverThemes();

    // If the saved theme is the active one, re-resolve so the
    // new bytes propagate via `themeChanged`. Unrelated themes
    // skip the re-apply through the byte-equal fast-path in
    // `ResolveAndApplyActive`.
    if (sanitised == mActiveName)
    {
        ResolveAndApplyActive(/*emitWhenUnchanged=*/true);
    }
}

bool ThemeControl::IsColorSchemeForcedForTest() const noexcept
{
    return mColorSchemeForced;
}

void ThemeControl::SetOsColorSchemeForTest(Qt::ColorScheme scheme) noexcept
{
    mOsColorScheme = scheme;
    // `Unknown` releases the pin and re-enables refreshes from
    // `QStyleHints::colorScheme()`.
    mOsColorSchemeLocked = (scheme != Qt::ColorScheme::Unknown);
}

void ThemeControl::LoadConfiguration()
{
    // Capture style/font/OS-scheme defaults before any apply
    // mutates them, so themes that omit those fields can revert
    // cleanly. Must run before the first `ApplyColorSchemeHint`
    // so `mOsColorScheme` records the unforced OS value.
    if (!mStartupCaptured)
    {
        if (qApp->style() != nullptr)
        {
            mStartupStyleName = qApp->style()->name();
        }
        mStartupFont = qApp->font();
        if (const QStyleHints *hints = QGuiApplication::styleHints(); hints != nullptr)
        {
            mOsColorScheme = hints->colorScheme();
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
    ResolveAndApplyActive(/*emitWhenUnchanged=*/false);
}

void ThemeControl::DiscoverThemes()
{
    mIndex.clear();

    // Shared ingest path. Empty `name` falls back to the file
    // basename (warns); user-dir collisions keep the first hit and
    // warn so the silent overwrite doesn't go unnoticed.
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
        // `QFileInfo::absolutePath()` works for both qrc paths
        // (`":/themes/dark.json"` -> `":/themes"`) and on-disk
        // paths, so the same lookup feeds both built-ins and user
        // files. `BuildStyleCache` consumes this to resolve
        // relative icon paths from the theme JSON.
        const QString sourceDir = QFileInfo(path).absolutePath();
        mIndex[name] = IndexEntry{.theme = std::move(*theme), .fromUser = fromUser, .sourceDir = sourceDir};
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

    // User entries shadow built-ins. Sort alphabetically so the
    // tie-breaker for `name` collisions is deterministic across
    // platforms.
    const QDir userDir = UserThemesDir();
    const QStringList userFiles = userDir.entryList(QStringList{QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    for (const QString &file : userFiles)
    {
        ingest(userDir.filePath(file), /*fromUser=*/true);
    }
}

void ThemeControl::ResolveAndApplyActive(bool emitWhenUnchanged)
{
    loglib::Theme chosen;
    QString chosenSourceDir;
    bool chosenFromUser = false;
    bool found = false;

    if (!mActiveSelection.isEmpty())
    {
        if (const auto it = mIndex.find(mActiveSelection); it != mIndex.end())
        {
            chosen = it->second.theme;
            chosenSourceDir = it->second.sourceDir;
            chosenFromUser = it->second.fromUser;
            found = true;
        }
        else
        {
            // Stale selection (theme deleted, settings copied
            // across machines, etc.): coerce in-memory state to
            // Auto. We don't touch `QSettings` -- only user-driven
            // saves persist.
            qInfo("Theme selection %s no longer exists; reverting to Auto.", qUtf8Printable(mActiveSelection));
            mActiveSelection.clear();
        }
    }
    if (!found)
    {
        // Auto: pick by cached OS scheme. If we still hold a
        // Force-mode override, release it and refresh the cache
        // first -- Qt suppresses OS scheme updates while an
        // override is active, so the cache can be stale.
        if (mColorSchemeForced)
        {
            if (QStyleHints *hints = QGuiApplication::styleHints(); hints != nullptr)
            {
                const bool wasApplying = mApplyingTheme;
                mApplyingTheme = true;
                hints->unsetColorScheme();
                mColorSchemeForced = false;
                if (!mOsColorSchemeLocked)
                {
                    mOsColorScheme = hints->colorScheme();
                }
                mApplyingTheme = wasApplying;
            }
        }
        const bool osDark = (mOsColorScheme == Qt::ColorScheme::Dark);
        const QString desiredName =
            osDark ? QString::fromLatin1(BUILTIN_DARK_NAME) : QString::fromLatin1(BUILTIN_LIGHT_NAME);
        if (const auto it = mIndex.find(desiredName); it != mIndex.end())
        {
            chosen = it->second.theme;
            chosenSourceDir = it->second.sourceDir;
            chosenFromUser = it->second.fromUser;
            found = true;
        }
    }
    if (!found && !mIndex.empty())
    {
        // Last resort -- keep the app usable in a broken install.
        chosen = mIndex.begin()->second.theme;
        chosenSourceDir = mIndex.begin()->second.sourceDir;
        chosenFromUser = mIndex.begin()->second.fromUser;
        found = true;
    }
    if (!found)
    {
        return;
    }

    const QString newName = QString::fromStdString(chosen.name);
    const bool unchanged = (newName == mActiveName);

    // A Force<->Auto flip with the same resolved name still needs
    // `ApplyColorSchemeHint` to pin/unpin `QStyleHints::colorScheme`.
    const bool selectionModeChanged = mActiveSelection.isEmpty() != mAppliedSelection.isEmpty();

    // Skip the re-apply when the resolved name is unchanged --
    // also breaks the `setPalette` -> event -> Reevaluate loop.
    if (unchanged && !emitWhenUnchanged)
    {
        if (selectionModeChanged)
        {
            // Only the colour-scheme hint needs updating; palette
            // and caches are still valid.
            ApplyColorSchemeHint(mActive);
            mAppliedSelection = mActiveSelection;
        }
        return;
    }

    // ReloadAll fast-path: byte-equal theme means nothing to do.
    if (unchanged && emitWhenUnchanged && chosen == mActive && !selectionModeChanged)
    {
        return;
    }

    mActive = std::move(chosen);
    mActiveName = newName;
    mActiveSourceDir = chosenSourceDir;
    mActiveFromUser = chosenFromUser;
    mAppliedSelection = mActiveSelection;
    ApplyTheme(mActive);
    if (!unchanged || emitWhenUnchanged)
    {
        emit themeChanged();
    }
}

void ThemeControl::ApplyTheme(const loglib::Theme &theme)
{
    // Re-entrancy guard for the palette/style events fired below.
    if (mApplyingTheme)
    {
        return;
    }
    mApplyingTheme = true;

    // Set the style first so the standard palette we mutate below
    // is the *new* style's palette. Themes that omit `qtStyle`
    // revert to the startup style.
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

    // Push the colour-scheme hint so platform-native chrome
    // (Windows menus, native dialogs) follows the active theme,
    // and so Qt 6.8+ rebuilds its standard palette in the right
    // scheme before we layer overrides on top.
    ApplyColorSchemeHint(theme);

    ApplyPalette(theme);

    // Font: revert to the startup font when the theme omits the
    // overrides; overlay family/size onto the startup font
    // otherwise.
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

    // Build the cache last so it picks up the just-pushed font.
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

    // Auto mode: drop the override so Qt tracks the OS again.
    // Leaving the scheme unset is what lets later OS flips fire
    // `colorSchemeChanged`.
    if (mActiveSelection.isEmpty())
    {
        if (mColorSchemeForced)
        {
            hints->unsetColorScheme();
            mColorSchemeForced = false;
            if (!mOsColorSchemeLocked)
            {
                mOsColorScheme = hints->colorScheme();
            }
        }
        return;
    }

    // Force mode: snapshot the OS scheme *before* installing the
    // override -- afterwards Qt reports our forced value, so the
    // next Auto resolution would lose the real OS state.
    if (!mColorSchemeForced)
    {
        if (!mOsColorSchemeLocked)
        {
            mOsColorScheme = hints->colorScheme();
        }
    }

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
    // Build a complete palette per theme kind so a Force-Light on
    // a dark OS (or vice versa) has every role matching the
    // theme, not the OS. Otherwise text rendered with the OS
    // `Text` colour on the theme's `Base` can be invisible.
    const bool dark = (theme.kind == loglib::ThemeKind::Dark);

    auto hexOr = [](const std::optional<std::string> &hex, const QColor &fallback) {
        if (!hex.has_value() || hex->empty())
        {
            return fallback;
        }
        const QColor color(QString::fromStdString(*hex));
        return color.isValid() ? color : fallback;
    };

    // Per-kind defaults match the built-in JSON, so a partial
    // theme still produces a self-consistent palette.
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
    // roles (Mid, Shadow, Light, ...) carry through.
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

    // Pin the highlight across Active+Inactive so an unfocused
    // window keeps the same selection colour as a focused one.
    // (Other roles were already broadcast to all groups by the
    // group-less `setColor` calls above.)
    for (const QPalette::ColorGroup group : {QPalette::Active, QPalette::Inactive})
    {
        palette.setColor(group, QPalette::Highlight, highlight);
        palette.setColor(group, QPalette::HighlightedText, highlightedText);
    }

    // Dim text-bearing roles in the Disabled group toward their
    // backing surface so disabled chrome reads as dimmer without
    // vanishing. Surface roles below stay at full strength.
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
        {{.role = QPalette::Text, .foreground = text, .background = base, .mix = DISABLED_TEXT_MIX},
         {.role = QPalette::WindowText, .foreground = windowText, .background = window, .mix = DISABLED_TEXT_MIX},
         {.role = QPalette::ButtonText, .foreground = buttonText, .background = button, .mix = DISABLED_TEXT_MIX},
         // ToolTipText blends toward `toolTipBase` (its surface),
         // not `base`, per the ChromeStyle contract.
         {.role = QPalette::ToolTipText, .foreground = toolTipText, .background = toolTipBase, .mix = DISABLED_TEXT_MIX
         },
         {.role = QPalette::PlaceholderText, .foreground = placeholder, .background = base, .mix = DISABLED_TEXT_MIX},
         {.role = QPalette::Highlight, .foreground = highlight, .background = window, .mix = DISABLED_HIGHLIGHT_MIX},
         {.role = QPalette::HighlightedText,
          .foreground = highlightedText,
          .background = disabledHighlight,
          .mix = DISABLED_TEXT_MIX}}
    };
    for (const DisabledOverride &entry : textOverrides)
    {
        palette.setColor(QPalette::Disabled, entry.role, BlendTowards(entry.foreground, entry.background, entry.mix));
    }
    // Surface roles stay at full strength -- only text dims.
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

    // Snapshot the app font once; per-level entries overlay
    // bold/italic on top.
    const QFont appFont = qApp->font();
    mFonts.fill(appFont);

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

    // Cache anchor brushes: theme override first, then fallback.
    // Foreground is chosen per-slot from background luma so pastel
    // user slots still get legible text.
    for (size_t slot = 0; slot < loglib::ANCHOR_PALETTE_SIZE; ++slot)
    {
        QColor background;
        if (slot < theme.anchorPalette.size() && !theme.anchorPalette[slot].empty())
        {
            background = QColor(QString::fromStdString(theme.anchorPalette[slot]));
        }
        if (!background.isValid())
        {
            background = QColor(QString::fromLatin1(ANCHOR_FALLBACK_PALETTE[slot]));
        }
        mAnchorBackground[slot] = QBrush{background};
        mAnchorForeground[slot] = QBrush{ThemeControl::IsDarkColor(background) ? QColor(Qt::white) : QColor(Qt::black)};
    }

    // Reset the level-column override caches. Anything left over
    // from a previously-active icon theme must clear out so a
    // subsequent theme that omits the block reverts to plain text.
    mHasLevelColumnOverride = false;
    mLevelColumnHeaderText.reset();
    mLevelColumnHeaderIcon = QIcon{};
    mLevelIcons.fill(QIcon{});
    mPillBackground.fill(QBrush{});
    mPillForeground.fill(QBrush{});

    if (!theme.levelColumnOverride.has_value())
    {
        return;
    }
    mHasLevelColumnOverride = true;
    const loglib::LevelColumnOverride &override = *theme.levelColumnOverride;

    // Header chrome: nullopt vs ""-vs-set is preserved verbatim;
    // `LogModel::headerData` uses the three cases to decide
    // whether to fall through to `Column::header`.
    if (override.header.has_value())
    {
        mLevelColumnHeaderText = QString::fromStdString(*override.header);
    }

    // Icon rasterisation params: small-icon size + app DPR. The
    // delegate downsamples further to fit the pill, but starting
    // from a sharp source keeps stroke widths consistent across
    // themes (same recipe `icon_loader::ResolveAnchorIconParams`
    // uses for the toolbar).
    constexpr int FALLBACK_ICON_SIZE_PX = 16;
    int sizePx = FALLBACK_ICON_SIZE_PX;
    if (QStyle *style = qApp->style(); style != nullptr)
    {
        const int metric = style->pixelMetric(QStyle::PM_SmallIconSize);
        if (metric > 0)
        {
            sizePx = metric;
        }
    }
    const qreal dpr = qApp->devicePixelRatio();
    const QColor paletteWindowText = qApp->palette().color(QPalette::Active, QPalette::WindowText);

    // Project the string-keyed override map into a `LogLevel`-indexed
    // local lookup so the per-level loop below stays O(1) per level.
    std::array<const loglib::LevelDisplayOverride *, LEVEL_SLOTS> perLevel{};
    perLevel.fill(nullptr);
    for (const auto &[key, value] : override.levels)
    {
        for (const LogLevel level : ALL_LEVELS)
        {
            if (std::string_view(key) == loglib::CanonicalLevelName(level))
            {
                perLevel[LevelIndex(level)] = &value;
                break;
            }
        }
    }

    // Header identity icon. No compile-default fallback: a theme
    // that wants one sets `headerIcon` explicitly. Tint resolves
    // to the palette's `WindowText` because the header isn't
    // level-specific.
    if (override.headerIcon.has_value() && !override.headerIcon->empty())
    {
        const QString resolved = ResolveIconPath(*override.headerIcon, mActiveSourceDir, mActiveFromUser);
        if (!resolved.isEmpty())
        {
            mLevelColumnHeaderIcon = icon_loader::MakeThemedIcon(resolved, paletteWindowText, sizePx, dpr);
        }
    }

    // Per-level icon + pill caches. Foreground resolution chain:
    //   override.pillForeground -> LevelStyle.foreground -> WindowText
    // Background resolution: just `override.pillBackground` (no
    // fallback -- absent means "no pill", which the delegate
    // honours by skipping the rounded-rect draw).
    for (const LogLevel level : ALL_LEVELS)
    {
        const size_t idx = LevelIndex(level);
        const loglib::LevelDisplayOverride *entry = perLevel[idx];
        if (entry == nullptr)
        {
            continue;
        }

        const QBrush pillBg = BrushFromHex(entry->pillBackground);
        mPillBackground[idx] = pillBg;

        QColor pillFgColor;
        if (entry->pillForeground.has_value() && !entry->pillForeground->empty())
        {
            const QString hexQ = QString::fromStdString(*entry->pillForeground);
            pillFgColor = QColor(hexQ);
            if (!pillFgColor.isValid())
            {
                // Match `BrushFromHex`'s warning surface so a typo
                // in `pillForeground` is just as discoverable as
                // one in `LevelStyle::foreground`. We don't bail
                // -- the chain falls back to `LevelStyle.foreground`
                // then `WindowText`, both of which the user expects.
                qWarning(
                    "Theme pillForeground hex `%s` did not parse; falling back to LevelStyle foreground / palette.",
                    qUtf8Printable(hexQ)
                );
            }
        }
        if (!pillFgColor.isValid())
        {
            const QBrush levelFg = mForeground[idx];
            if (levelFg.style() != Qt::NoBrush)
            {
                pillFgColor = levelFg.color();
            }
        }
        if (!pillFgColor.isValid())
        {
            pillFgColor = paletteWindowText;
        }
        mPillForeground[idx] = QBrush{pillFgColor};

        if (entry->icon.has_value() && !entry->icon->empty())
        {
            const QString resolved = ResolveIconPath(*entry->icon, mActiveSourceDir, mActiveFromUser);
            if (!resolved.isEmpty())
            {
                mLevelIcons[idx] = icon_loader::MakeThemedIcon(resolved, pillFgColor, sizePx, dpr);
            }
        }
    }
}
