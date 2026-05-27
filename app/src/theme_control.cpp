#include "theme_control.hpp"

#include <loglib/log_level.hpp>
#include <loglib/theme.hpp>

#include <QApplication>
#include <QColor>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLatin1String>
#include <QPalette>
#include <QSettings>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QStyle>
#include <QStyleFactory>
#include <QTextStream>
#include <QUrl>
#include <QVariant>

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
    BuildStyleCache(theme);

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
