#pragma once

#include <QString>

#include <filesystem>

namespace logapp
{

/// Convert a `QString` to a `std::filesystem::path` without going
/// through the native narrow encoding.
///
/// On Windows this matters: `QString::toStdString()` returns
/// UTF-8, but MSVC's `path(const std::string&)` interprets its
/// argument as the ACP (Active Code Page), which is usually not
/// UTF-8. Non-ASCII filenames (Cyrillic, CJK, ...) round-trip
/// mangled or fail to open. This helper takes the wide overload
/// on Windows (Qt's UTF-16 maps losslessly to `std::wstring`) and
/// stays on the narrow overload elsewhere, where `std::string`
/// *is* UTF-8. Use at every `QString` -> `<filesystem>` boundary.
[[nodiscard]] inline std::filesystem::path QStringToFsPath(const QString &path)
{
#ifdef Q_OS_WIN
    return {path.toStdWString()};
#else
    return {path.toStdString()};
#endif
}

/// Inverse of `QStringToFsPath`: render a `std::filesystem::path`
/// as a `QString` without dropping non-ASCII bytes. Windows routes
/// via `path::wstring()` (lossless UTF-16); POSIX uses the native
/// UTF-8 string. Use at every `<filesystem>` -> `QString` boundary
/// because the tempting `QString::fromStdString(path.string())`
/// silently mojibakes non-ASCII names on Windows.
[[nodiscard]] inline QString FsPathToQString(const std::filesystem::path &path)
{
#ifdef Q_OS_WIN
    return QString::fromStdWString(path.wstring());
#else
    return QString::fromStdString(path.string());
#endif
}

} // namespace logapp
