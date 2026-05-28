#include "theme_control.hpp"

#include <loglib/log_level.hpp>
#include <loglib/theme.hpp>

#include <QApplication>
#include <QColor>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLatin1String>
#include <QPalette>
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
#include <fstream>
#include <stdexcept>
#include <string>

namespace
{

constexpr char SETTINGS_KEY_ACTIVE[] = "theme/active";

constexpr char BUILTIN_LIGHT_NAME[] = "Light";
constexpr char BUILTIN_DARK_NAME[] = "Dark";

constexpr char BUILTIN_LIGHT_PATH[] = ":/themes/light.json";
constexpr char BUILTIN_DARK_PATH[] = ":/themes/dark.json";

/// Same heuristic AppearanceControl used: a light system palette has
/// a Window background brighter than this. Kept as a tunable so a
/// future contrast policy can adjust without changing call sites.
constexpr int K_MID_GRAY_BRIGHTNESS = 128;

bool IsDarkPalette()
{
    const QColor bgColor = qApp->palette().color(QPalette::Window);
    const int brightness = ((bgColor.red() * 299) + (bgColor.green() * 587) + (bgColor.blue() * 114)) / 1000;
    return brightness < K_MID_GRAY_BRIGHTNESS;
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

void ThemeControl::SaveConfiguration()
{
    QSettings settings;
    settings.setValue(QLatin1String(SETTINGS_KEY_ACTIVE), Instance().mActiveSelection);
}

void ThemeControl::Reevaluate()
{
    Instance().DoReevaluate();
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
    const size_t idx = LevelIndex(level);
    if (idx >= Instance().mForeground.size())
    {
        return {};
    }
    return Instance().mForeground[idx];
}

QBrush ThemeControl::BackgroundFor(loglib::LogLevel level) noexcept
{
    const size_t idx = LevelIndex(level);
    if (idx >= Instance().mBackground.size())
    {
        return {};
    }
    return Instance().mBackground[idx];
}

QFont ThemeControl::FontFor(loglib::LogLevel level, const QFont &base) noexcept
{
    const size_t idx = LevelIndex(level);
    if (idx >= Instance().mBold.size())
    {
        return base;
    }
    QFont font = base;
    if (Instance().mBold[idx])
    {
        font.setBold(true);
    }
    if (Instance().mItalic[idx])
    {
        font.setItalic(true);
    }
    return font;
}

bool ThemeControl::HasStyle(loglib::LogLevel level) noexcept
{
    const size_t idx = LevelIndex(level);
    if (idx >= Instance().mHasAnyStyle.size())
    {
        return false;
    }
    return Instance().mHasAnyStyle[idx];
}

bool ThemeControl::HasFontStyle(loglib::LogLevel level) noexcept
{
    const size_t idx = LevelIndex(level);
    if (idx >= Instance().mBold.size())
    {
        return false;
    }
    return Instance().mBold[idx] || Instance().mItalic[idx];
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

void ThemeControl::SaveUserTheme(const QString &name, loglib::Theme theme)
{
    // Pin the on-disk `name` to the file basename so a later
    // rename of the file keeps the index consistent with the
    // declared name.
    theme.name = name.toStdString();
    const std::string json = loglib::SerializeTheme(theme);

    const QDir dir = UserThemesDir();
    const QString path = dir.filePath(name + QStringLiteral(".json"));
    std::ofstream out(path.toStdString());
    if (!out.is_open())
    {
        throw std::runtime_error("Failed to open user theme file: " + path.toStdString());
    }
    out << json;
    if (!out.good())
    {
        throw std::runtime_error("Failed to write user theme file: " + path.toStdString());
    }
}

void ThemeControl::DoLoadConfiguration()
{
    DiscoverThemes();

    QSettings settings;
    const QVariant raw = settings.value(QLatin1String(SETTINGS_KEY_ACTIVE));
    mActiveSelection = raw.isValid() ? raw.toString() : QString();

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

    // Built-ins first, so a same-named user file overrides them.
    constexpr std::array<const char *, 2> BUILTIN_PATHS = {BUILTIN_LIGHT_PATH, BUILTIN_DARK_PATH};
    for (const char *path : BUILTIN_PATHS)
    {
        if (auto theme = ParseFileToTheme(QString::fromLatin1(path)); theme.has_value())
        {
            const QString name = QString::fromStdString(theme->name);
            if (!name.isEmpty())
            {
                mIndex[name] = IndexEntry{.theme = std::move(*theme), .fromUser = false};
            }
        }
    }

    // User dir entries shadow built-ins by name.
    const QDir userDir = UserThemesDir();
    const QStringList userFiles = userDir.entryList(QStringList{QStringLiteral("*.json")}, QDir::Files);
    for (const QString &file : userFiles)
    {
        const QString fullPath = userDir.filePath(file);
        auto theme = ParseFileToTheme(fullPath);
        if (!theme.has_value())
        {
            continue;
        }
        // If a user file omits or empties `name`, fall back to the
        // file basename. That keeps the index keyed by something
        // stable and matches the listing the user sees in the
        // file manager.
        QString name = QString::fromStdString(theme->name);
        if (name.isEmpty())
        {
            name = QFileInfo(file).completeBaseName();
            theme->name = name.toStdString();
        }
        mIndex[name] = IndexEntry{.theme = std::move(*theme), .fromUser = true};
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
    }
    if (!found)
    {
        // Auto (or selection is stale / unknown): pick by palette.
        const QString desiredName =
            IsDarkPalette() ? QString::fromLatin1(BUILTIN_DARK_NAME) : QString::fromLatin1(BUILTIN_LIGHT_NAME);
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

    // Skip the full re-apply when the resolved theme name hasn't
    // changed and the caller doesn't insist. This is the second
    // line of defence against the palette-change feedback loop:
    // `qApp->setPalette` fires `ApplicationPaletteChange` ->
    // `MainWindow::event` -> `Reevaluate` -> here.
    if (unchanged && !emitWhenUnchanged)
    {
        return;
    }

    mActive = std::move(chosen);
    mActiveName = newName;
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

    BuildStyleCache(theme);

    // Order matters: set the style FIRST so the standard palette
    // we then mutate is the new style's standard palette, not the
    // previous style's. Without this, switching from windows11 ->
    // fusion on a dark-mode Windows system would briefly leave the
    // dark windows11 palette in place even after we wrote the
    // theme-derived light palette over it.
    if (theme.app.qtStyle.has_value())
    {
        const QString styleName = QString::fromStdString(*theme.app.qtStyle);
        if (!styleName.isEmpty() && qApp->style()->name().compare(styleName, Qt::CaseInsensitive) != 0)
        {
            if (QStyle *newStyle = QStyleFactory::create(styleName); newStyle != nullptr)
            {
                qApp->setStyle(newStyle);
            }
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

    if (theme.app.fontFamily.has_value() || theme.app.fontSize.has_value())
    {
        QFont font = qApp->font();
        if (theme.app.fontFamily.has_value() && !theme.app.fontFamily->empty())
        {
            font.setFamily(QString::fromStdString(*theme.app.fontFamily));
        }
        if (theme.app.fontSize.has_value() && *theme.app.fontSize > 0)
        {
            font.setPointSize(*theme.app.fontSize);
        }
        qApp->setFont(font);
    }

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
    // already matches the current system brightness (we picked it
    // by sampling the palette), so we don't need to set anything
    // here. Crucially, leaving the scheme unset is what lets a
    // later system dark/light flip fire
    // `ApplicationPaletteChange` through to `MainWindow::event`
    // -> `Reevaluate`.
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
        }
        return;
    }

    // Force mode: explicit override so Qt's standard palette and
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
    // subset still produce a self-consistent palette.
    const QColor base = hexOr(theme.table.background, dark ? QColor(0x22, 0x22, 0x22) : QColor(0xFF, 0xFF, 0xFF));
    const QColor altBase =
        hexOr(theme.table.alternateRowBackground, dark ? QColor(0x2A, 0x2A, 0x2A) : QColor(0xF3, 0xF4, 0xF6));
    const QColor text = dark ? QColor(0xE5, 0xE7, 0xEB) : QColor(0x0F, 0x17, 0x2A);
    const QColor window = dark ? QColor(0x2A, 0x2A, 0x2A) : QColor(0xF7, 0xF7, 0xF7);
    const QColor windowText = dark ? QColor(0xE5, 0xE7, 0xEB) : QColor(0x0F, 0x17, 0x2A);
    const QColor button = dark ? QColor(0x37, 0x41, 0x51) : QColor(0xF3, 0xF4, 0xF6);
    const QColor buttonText = dark ? QColor(0xE5, 0xE7, 0xEB) : QColor(0x0F, 0x17, 0x2A);
    const QColor placeholder = dark ? QColor(0x9C, 0xA3, 0xAF) : QColor(0x6B, 0x72, 0x80);
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
    palette.setColor(QPalette::ToolTipBase, window);
    palette.setColor(QPalette::ToolTipText, text);
    palette.setColor(QPalette::PlaceholderText, placeholder);

    // Apply highlight to both the Active and Inactive colour
    // groups so a focused-elsewhere window keeps the same
    // selection colour the user sees while interacting. Without
    // this Qt grays the selection out when the window loses
    // focus, which previously required QSS to override.
    for (QPalette::ColorGroup group : {QPalette::Active, QPalette::Inactive})
    {
        palette.setColor(group, QPalette::Highlight, highlight);
        palette.setColor(group, QPalette::HighlightedText, highlightedText);
        palette.setColor(group, QPalette::Base, base);
        palette.setColor(group, QPalette::AlternateBase, altBase);
        palette.setColor(group, QPalette::Text, text);
        palette.setColor(group, QPalette::Window, window);
        palette.setColor(group, QPalette::WindowText, windowText);
    }

    // Disabled colour group: every group-less `setColor(role, c)`
    // call above also wrote `c` into the Disabled group (Qt's API
    // shorthand for "all groups"), which made disabled menu items,
    // buttons, etc. read identically to enabled ones. Override the
    // text-bearing roles here with values blended toward the
    // surrounding surface so disabled chrome dims visibly without
    // disappearing.
    auto blend = [](const QColor &fg, const QColor &bg, float towardsBg) {
        const float t = std::clamp(towardsBg, 0.0F, 1.0F);
        return QColor::fromRgbF(
            (fg.redF() * (1.0F - t)) + (bg.redF() * t),
            (fg.greenF() * (1.0F - t)) + (bg.greenF() * t),
            (fg.blueF() * (1.0F - t)) + (bg.blueF() * t),
            1.0F
        );
    };
    // 0.55 leaves enough contrast to read but lands comfortably in
    // "obviously disabled" territory across both Light and Dark.
    constexpr float DISABLED_TEXT_MIX = 0.55F;
    constexpr float DISABLED_HIGHLIGHT_MIX = 0.40F;
    const QColor disabledText = blend(text, base, DISABLED_TEXT_MIX);
    const QColor disabledWindowText = blend(windowText, window, DISABLED_TEXT_MIX);
    const QColor disabledButtonText = blend(buttonText, button, DISABLED_TEXT_MIX);
    const QColor disabledPlaceholder = blend(placeholder, base, DISABLED_TEXT_MIX);
    const QColor disabledHighlight = blend(highlight, window, DISABLED_HIGHLIGHT_MIX);
    const QColor disabledHighlightedText = blend(highlightedText, disabledHighlight, DISABLED_TEXT_MIX);

    palette.setColor(QPalette::Disabled, QPalette::Text, disabledText);
    palette.setColor(QPalette::Disabled, QPalette::WindowText, disabledWindowText);
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, disabledButtonText);
    palette.setColor(QPalette::Disabled, QPalette::ToolTipText, disabledText);
    palette.setColor(QPalette::Disabled, QPalette::PlaceholderText, disabledPlaceholder);
    palette.setColor(QPalette::Disabled, QPalette::Highlight, disabledHighlight);
    palette.setColor(QPalette::Disabled, QPalette::HighlightedText, disabledHighlightedText);
    // Surface-bearing roles stay at full strength so the chrome
    // itself remains recognisable; only the text-on-top dims.
    palette.setColor(QPalette::Disabled, QPalette::Base, base);
    palette.setColor(QPalette::Disabled, QPalette::AlternateBase, altBase);
    palette.setColor(QPalette::Disabled, QPalette::Window, window);
    palette.setColor(QPalette::Disabled, QPalette::Button, button);
    palette.setColor(QPalette::Disabled, QPalette::ToolTipBase, window);

    qApp->setPalette(palette);
}

void ThemeControl::BuildStyleCache(const loglib::Theme &theme)
{
    mForeground.fill(QBrush{});
    mBackground.fill(QBrush{});
    mBold.fill(false);
    mItalic.fill(false);
    mHasAnyStyle.fill(false);

    using loglib::LogLevel;
    constexpr std::array<LogLevel, loglib::CANONICAL_LEVEL_COUNT> CANONICAL = {
        LogLevel::Trace, LogLevel::Debug, LogLevel::Info, LogLevel::Warn, LogLevel::Error, LogLevel::Fatal
    };
    for (const LogLevel level : CANONICAL)
    {
        const loglib::LevelStyle style = loglib::StyleForLevel(theme, level);
        const size_t idx = LevelIndex(level);
        mForeground[idx] = BrushFromHex(style.foreground);
        mBackground[idx] = BrushFromHex(style.background);
        mBold[idx] = style.bold;
        mItalic[idx] = style.italic;
        mHasAnyStyle[idx] = style.foreground.has_value() || style.background.has_value() || style.bold || style.italic;
    }
}
