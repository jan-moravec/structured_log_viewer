#pragma once

#include "qstring_path.hpp"

#include <loglib/core/path_key.hpp>

#include <QDir>
#include <QFileInfo>
#include <QString>
#include <QUuid>

#include <string>

#ifdef Q_OS_WIN
#include <Qt>
#endif

namespace logapp
{

/// Strict UUID-shape check. Per-session JSON files live at
/// `sessionsDir/<uuid>.json`, so any consumer composing a uuid into
/// a filesystem path must gate on this first to prevent a malformed
/// value (e.g. `"../etc/passwd"` from a corrupted profile) from
/// escaping the sessions directory.
[[nodiscard]] inline bool LooksLikeUuid(const QString &candidate) noexcept
{
    return !QUuid::fromString(candidate).isNull();
}

/// Display path for a file locator: absolute, forward-slashed on
/// Windows, but with the user's original case preserved. This is
/// the form persisted on `SourceLocator::path`, shown in tooltips,
/// and passed to `QFile::open`. Its key is `PathKeyOf`; add both to a
/// source with `loglib::AppendLocator`.
[[nodiscard]] inline QString CanonicalDisplayPath(const QString &locator)
{
    if (locator.isEmpty())
    {
        return locator;
    }
    QString absolute = QFileInfo(locator).absoluteFilePath();
#ifdef Q_OS_WIN
    absolute.replace(QLatin1Char('\\'), QLatin1Char('/'));
#endif
    return absolute;
}

/// The loglib path key of a file path that came from the user interface. Anchors and source locators refer to a
/// file by this key, so use it, and not a form of your own, to compare files.
[[nodiscard]] inline std::string PathKeyOf(const QString &path)
{
    return loglib::MakePathKey(QStringToFsPath(path));
}

} // namespace logapp
