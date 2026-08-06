#pragma once

#include <QString>

#include <filesystem>

namespace logapp
{

/// Convert a `QString` to a filesystem path without narrowing it
/// through the Windows active code page.
[[nodiscard]] inline std::filesystem::path QStringToFsPath(const QString &path)
{
#ifdef Q_OS_WIN
    return {path.toStdWString()};
#else
    return {path.toStdString()};
#endif
}

/// Convert a filesystem path to `QString` without losing non-ASCII
/// characters on Windows.
[[nodiscard]] inline QString FsPathToQString(const std::filesystem::path &path)
{
#ifdef Q_OS_WIN
    return QString::fromStdWString(path.wstring());
#else
    return QString::fromStdString(path.string());
#endif
}

} // namespace logapp
