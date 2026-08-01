#pragma once

#include <QString>

#include <filesystem>

namespace logapp
{

/// Convert a `QString` (always UTF-16 internally) into a
/// `std::filesystem::path` without going through the native narrow
/// encoding. On Windows this matters: `QString::toStdString()`
/// returns UTF-8, but the `std::filesystem::path(const std::string&)`
/// constructor on MSVC interprets its argument as the ACP (Active
/// Code Page), which is *not* UTF-8 by default. Non-ASCII filenames
/// (e.g. Cyrillic, CJK) fed through the naive round-trip come out
/// mangled or trigger `ERROR_FILE_NOT_FOUND` at open time.
///
/// This helper picks the wide overload on Windows (Qt's UTF-16
/// buffer maps to `std::wstring` losslessly) and stays on the narrow
/// overload elsewhere, where `std::string` *is* UTF-8 and the
/// filesystem library follows suit. Same reasoning as
/// `QFile::encodeName` / `QFile::decodeName`, but yielding a
/// `std::filesystem::path` instead of a `QByteArray`.
///
/// Use this at every boundary where a user-supplied `QString` path
/// hands off to a `<filesystem>` API.
[[nodiscard]] inline std::filesystem::path QStringToFsPath(const QString &path)
{
#ifdef Q_OS_WIN
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::path(path.toStdString());
#endif
}

} // namespace logapp
