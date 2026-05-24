#pragma once

#include <QDir>
#include <QFileInfo>
#include <QString>
#include <QUuid>

#if defined(Q_OS_WIN)
#include <Qt>
#endif

namespace logapp
{

/// Strict UUID-shape check for filename stems used by the recents
/// subsystem. The recents store keys per-session JSON files by uuid
/// stem (`<sessionsDir>/<uuid>.json`); any consumer that takes a
/// caller-supplied uuid and composes it into a filesystem path MUST
/// gate on this helper first.
///
/// Why this matters: `SessionHistoryManager::PathForUuid` is naive
/// string concatenation; without the gate, a uuid value of
/// `"../../etc/passwd"` (planted via a buggy upgrade, an unrelated
/// tool writing into QSettings on Linux, or a hand-edited profile)
/// makes `Remove(uuid)` / capacity eviction unlink files outside the
/// sessions directory. The orphan-sweeper (`CleanupOrphanFiles`) and
/// `MainWindow::RestoreLastSessionFromPath` already validate uuids
/// via `QUuid::fromString(...).isNull()`; this helper makes the
/// check explicit so every consumer can apply the same gate
/// uniformly.
[[nodiscard]] inline bool LooksLikeUuid(const QString &candidate) noexcept
{
    return !QUuid::fromString(candidate).isNull();
}

/// Normalise a file-system locator before storing it on a `Source`
/// descriptor. The locator is round-tripped through QFileInfo so we
/// get an absolute path; on Windows we additionally normalise the
/// directory separators (`\` -> `/`) and lower-case the drive letter
/// and path so the dedup performed by the multi-file open paths
/// works regardless of how the user typed (or how a previous build
/// recorded) the path.
///
/// Non-empty input that fails canonicalisation (file does not exist
/// on disk, or path is malformed) is returned as `absoluteFilePath`
/// without further mutation: locators are stored at session save
/// time and may legitimately refer to files that have moved or been
/// renamed by the time we read them back, so refusing to normalise
/// them would be hostile.
///
/// Empty input passes through unchanged so callers can dedup without
/// special-casing the "no source yet" sentinel.
[[nodiscard]] inline QString CanonicalLocator(const QString &locator)
{
    if (locator.isEmpty())
    {
        return locator;
    }
    QString absolute = QFileInfo(locator).absoluteFilePath();
#if defined(Q_OS_WIN)
    // Windows treats `\` and `/` as equivalent and is case-insensitive
    // for paths; the dedup check we run on `Source::locators` is a
    // byte compare, so normalise to a canonical form here.
    absolute.replace(QLatin1Char('\\'), QLatin1Char('/'));
    absolute = absolute.toLower();
#endif
    return absolute;
}

} // namespace logapp
