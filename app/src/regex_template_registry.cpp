#include "regex_template_registry.hpp"

#include <loglib/regex_templates.hpp>

#include <QByteArray>
#include <QChar>
#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QLatin1Char>
#include <QSaveFile>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <algorithm>
#include <array>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{

/// UTF-8 read helper, mirroring `theme_control.cpp`. Returns
/// nullopt on open failure so the caller can downgrade to a warn.
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

/// Parse @p path into a `RegexTemplate`. Swallows parse errors at
/// scan time (with a `qWarning`) so one broken JSON cannot wedge
/// the picker / library injection for the rest of the catalog.
std::optional<loglib::RegexTemplate> ParseFileToTemplate(const QString &path)
{
    const auto bytes = ReadFileUtf8(path);
    if (!bytes.has_value())
    {
        qWarning("Failed to read user regex template %s; skipping.", qUtf8Printable(path));
        return std::nullopt;
    }
    try
    {
        return loglib::ParseRegexTemplate(*bytes);
    }
    catch (const std::exception &ex)
    {
        qWarning("Failed to parse user regex template %s: %s", qUtf8Printable(path), ex.what());
        return std::nullopt;
    }
}

bool IsReservedWin32DeviceName(const QString &name)
{
    // Same list as `ThemeControl::SanitiseThemeName`; lifting it
    // verbatim keeps the two registries in lockstep without
    // either depending on the other.
    static constexpr std::array<const char *, 22> RESERVED = {"CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2",
                                                              "COM3", "COM4", "COM5", "COM6", "COM7", "COM8",
                                                              "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5",
                                                              "LPT6", "LPT7", "LPT8", "LPT9"};
    const QString upper = name.toUpper();
    return std::ranges::any_of(RESERVED, [&upper](const char *reserved) {
        return upper == QString::fromLatin1(reserved);
    });
}

} // namespace

RegexTemplateRegistry::RegexTemplateRegistry(QObject *parent)
    : QObject(parent)
{
    Discover();
}

QList<RegexTemplateRegistry::Listing> RegexTemplateRegistry::Available() const
{
    QList<Listing> out;
    out.reserve(static_cast<int>(mIndex.size()));
    for (const auto &[name, entry] : mIndex)
    {
        out.append(Listing{
            .name = name,
            .fromUser = entry.fromUser,
            .priority = entry.tmpl.priority,
            .autoDetect = entry.tmpl.autoDetect,
        });
    }
    return out;
}

std::optional<loglib::RegexTemplate> RegexTemplateRegistry::Load(const QString &name) const
{
    const auto it = mIndex.find(name);
    if (it == mIndex.end())
    {
        return std::nullopt;
    }
    return it->second.tmpl;
}

QDir RegexTemplateRegistry::UserTemplatesDir()
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (base.isEmpty())
    {
        base = QDir::tempPath() + QStringLiteral("/StructuredLogViewer");
    }
    const QString templatesPath = QDir(base).filePath(QStringLiteral("regex_templates"));
    const QDir dir(templatesPath);
    if (!dir.exists())
    {
        dir.mkpath(QStringLiteral("."));
    }
    return dir;
}

bool RegexTemplateRegistry::RevealUserTemplatesDir()
{
    const QDir dir = UserTemplatesDir();
    if (!dir.exists())
    {
        return false;
    }
    return QDesktopServices::openUrl(QUrl::fromLocalFile(dir.absolutePath()));
}

QString RegexTemplateRegistry::SanitiseTemplateName(const QString &name)
{
    if (name.isEmpty())
    {
        throw std::runtime_error("Regex template name must not be empty");
    }
    if (name != name.trimmed())
    {
        throw std::runtime_error("Regex template name must not have leading or trailing whitespace");
    }
    if (name == QStringLiteral(".") || name == QStringLiteral(".."))
    {
        throw std::runtime_error(R"(Regex template name must not be "." or "..")");
    }
    if (name.contains(QStringLiteral("..")))
    {
        throw std::runtime_error("Regex template name must not contain \"..\"");
    }
    constexpr char16_t FIRST_PRINTABLE_ASCII = 0x20U;
    for (const QChar ch : name)
    {
        if (ch == QLatin1Char('/') || ch == QLatin1Char('\\') || ch == QLatin1Char(':') || ch == QLatin1Char('<') ||
            ch == QLatin1Char('>') || ch == QLatin1Char('"') || ch == QLatin1Char('|') || ch == QLatin1Char('?') ||
            ch == QLatin1Char('*') || ch.unicode() < FIRST_PRINTABLE_ASCII)
        {
            throw std::runtime_error("Regex template name contains invalid character");
        }
    }
    if (IsReservedWin32DeviceName(name))
    {
        throw std::runtime_error("Regex template name matches a reserved Win32 device name");
    }
    if (name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' ')))
    {
        throw std::runtime_error("Regex template name must not end with a dot or space");
    }
    return name;
}

void RegexTemplateRegistry::SaveUserTemplate(const QString &name, loglib::RegexTemplate tmpl)
{
    const QString sanitised = SanitiseTemplateName(name);

    // Pin on-disk `name` so the index key matches even after the
    // file is renamed on disk.
    tmpl.name = sanitised.toStdString();
    const std::string json = loglib::SerializeRegexTemplate(tmpl);

    const QDir dir = UserTemplatesDir();
    const QString path = dir.filePath(sanitised + QStringLiteral(".json"));

    // `QSaveFile` writes atomically (temp file + rename on commit)
    // and handles UTF-8 paths on Windows, unlike `std::ofstream`.
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        throw std::runtime_error(
            "Failed to open user regex template file: " + path.toStdString() + " (" + file.errorString().toStdString() +
            ")"
        );
    }
    const QByteArray bytes = QByteArray::fromStdString(json);
    if (file.write(bytes) != bytes.size())
    {
        throw std::runtime_error(
            "Failed to write user regex template file: " + path.toStdString() + " (" +
            file.errorString().toStdString() + ")"
        );
    }
    if (!file.commit())
    {
        throw std::runtime_error(
            "Failed to commit user regex template file: " + path.toStdString() + " (" +
            file.errorString().toStdString() + ")"
        );
    }

    // Refresh the index so the new entry is immediately visible
    // and the library's auto-detect probe picks it up.
    Discover();
    emit templatesChanged();
}

void RegexTemplateRegistry::Reload()
{
    Discover();
    emit templatesChanged();
}

void RegexTemplateRegistry::DeleteUserTemplate(const QString &name)
{
    const auto it = mIndex.find(name);
    if (it == mIndex.end())
    {
        throw std::runtime_error("Regex template not found: " + name.toStdString());
    }
    if (!it->second.fromUser)
    {
        throw std::runtime_error(
            "Cannot delete built-in regex template \"" + name.toStdString() +
            "\". Built-ins live in the binary; create a same-named user template "
            "to shadow it instead."
        );
    }

    // Same path-derivation logic as `SaveUserTemplate`: sanitise
    // first so a callsite passing an unsanitised string can't
    // unlink something unexpected, then resolve relative to the
    // user templates dir.
    const QString sanitised = SanitiseTemplateName(name);
    const QDir dir = UserTemplatesDir();
    const QString path = dir.filePath(sanitised + QStringLiteral(".json"));

    QFile file(path);
    if (!file.exists())
    {
        // The index thought this was a user template but the file
        // is gone. Refresh anyway so the GUI catches up; no
        // exception (the desired end state is achieved).
        Discover();
        emit templatesChanged();
        return;
    }
    if (!file.remove())
    {
        throw std::runtime_error(
            "Failed to delete user regex template file: " + path.toStdString() + " (" +
            file.errorString().toStdString() + ")"
        );
    }

    Discover();
    emit templatesChanged();
}

bool RegexTemplateRegistry::IsUserTemplate(const QString &name) const
{
    const auto it = mIndex.find(name);
    return it != mIndex.end() && it->second.fromUser;
}

void RegexTemplateRegistry::Discover()
{
    mIndex.clear();

    // Built-ins first (so a same-named user file overrides them).
    for (const loglib::RegexTemplate &t : loglib::BuiltinRegexTemplates())
    {
        const QString name = QString::fromStdString(t.name);
        if (name.isEmpty())
        {
            // Shouldn't happen for shipped templates — the build
            // -side test sweep would fail first — but defend
            // against it so a future malformed JSON doesn't crash
            // the registry walk.
            continue;
        }
        mIndex[name] = IndexEntry{.tmpl = t, .fromUser = false};
    }

    // User entries. Sort alphabetically so the tie-breaker for
    // `name` collisions across user files is deterministic across
    // platforms.
    const QDir userDir = UserTemplatesDir();
    const QStringList userFiles = userDir.entryList(QStringList{QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    for (const QString &file : userFiles)
    {
        const QString path = userDir.filePath(file);
        auto tmpl = ParseFileToTemplate(path);
        if (!tmpl.has_value())
        {
            continue;
        }
        QString name = QString::fromStdString(tmpl->name);
        if (name.isEmpty())
        {
            name = QFileInfo(path).completeBaseName();
            qWarning(
                "User regex template %s has no `name` field; using file basename %s. "
                "Add a `name` entry to suppress this warning.",
                qUtf8Printable(path),
                qUtf8Printable(name)
            );
            tmpl->name = name.toStdString();
        }
        if (const auto existing = mIndex.find(name); existing != mIndex.end() && existing->second.fromUser)
        {
            qWarning(
                "User regex template name %s is already taken by a previously-loaded "
                "file; %s is ignored. Rename one of the two `name` entries to make both "
                "discoverable.",
                qUtf8Printable(name),
                qUtf8Printable(path)
            );
            continue;
        }
        mIndex[name] = IndexEntry{.tmpl = std::move(*tmpl), .fromUser = true};
    }

    InjectExtrasIntoLoglib();
}

void RegexTemplateRegistry::InjectExtrasIntoLoglib()
{
    // Snapshot user templates (built-ins are already in the
    // library's catalog; pushing them again would double-count).
    // Local vector owns the storage for the duration of the call;
    // the library copies into its own slot.
    std::vector<loglib::RegexTemplate> extras;
    extras.reserve(mIndex.size());
    for (const auto &[name, entry] : mIndex)
    {
        if (entry.fromUser)
        {
            extras.push_back(entry.tmpl);
        }
    }
    loglib::SetExtraRegexTemplates(std::span<const loglib::RegexTemplate>(extras));
}
