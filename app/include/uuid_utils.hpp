#pragma once

#include <loglib/log_configuration.hpp>

#include <QDir>
#include <QFileInfo>
#include <QString>
#include <QUuid>

#include <optional>

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

/// Compute the canonical *dedup key* for a file-system locator. The
/// locator is round-tripped through QFileInfo so we get an absolute
/// path; on Windows we additionally normalise the directory
/// separators (`\` -> `/`) and lower-case the drive letter and path
/// so a byte-equality compare across two callsites (one that got the
/// path from a Finder drop with title-case, another from a
/// configuration JSON saved with lowercase) collapses to "same
/// file".
///
/// The output is intentionally NOT user-facing: lowercasing the
/// drive letter on Windows makes the recents tooltip read
/// "c:/users/jane/server.log" when the user typed
/// "C:/Users/Jane/Server.log". The display path -- the one shown to
/// the user and the one fed to `QFile::open` -- comes from
/// `CanonicalDisplayPath` below. Persisted on `Source::locatorDedupKeys`.
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
    // for paths; the dedup check we run on `Source::locatorDedupKeys`
    // is a byte compare, so normalise to a canonical form here.
    // Case is intentionally lowered here -- this is the dedup key,
    // not the display path. See `CanonicalDisplayPath` for the
    // sibling that preserves case.
    absolute.replace(QLatin1Char('\\'), QLatin1Char('/'));
    absolute = absolute.toLower();
#endif
    return absolute;
}

/// Compute the *display path* for a file-system locator: absolute,
/// forward-slashed on Windows (consistent with how the rest of the
/// app renders paths), but with the user's original case preserved.
/// This is the form persisted on `Source::locators` and shown in
/// the Recent Sessions tooltip; it is also the form passed to
/// `QFile::open` so a future case-sensitive filesystem (WSL with
/// case sensitivity enabled, a session JSON shared with a Linux
/// teammate) still resolves to the right file. Pair every
/// `CanonicalLocator` call with a matching `CanonicalDisplayPath`
/// call and push both via `loglib::AppendLocator` so the parallel
/// arrays stay in lockstep.
///
/// Empty input passes through unchanged; non-existent or malformed
/// inputs return the absolute path without further mutation, same
/// "be tolerant of stale locators" rationale as `CanonicalLocator`.
[[nodiscard]] inline QString CanonicalDisplayPath(const QString &locator)
{
    if (locator.isEmpty())
    {
        return locator;
    }
    QString absolute = QFileInfo(locator).absoluteFilePath();
#if defined(Q_OS_WIN)
    // Slash normalisation but NOT case normalisation: this is the
    // string the user will see in tooltips / status bar / Recent
    // Sessions menu, and the case they typed matters.
    absolute.replace(QLatin1Char('\\'), QLatin1Char('/'));
#endif
    return absolute;
}

/// Repair the `Source::locatorDedupKeys` parallel array so it has
/// exactly one entry per `Source::locators` entry. Used after
/// loading a session JSON whose schema may pre-date the
/// `locatorDedupKeys` field (the array is then default-empty),
/// or after constructing a `Source` in a code path that pre-dates
/// the helper adoption (the test fixtures still use designated
/// initialisers like `.locators = {"C:/x.json"}` without setting
/// the dedup-keys vector). Without the backfill, downstream dedup
/// loops in `MainWindow::StreamNextPendingFile` and
/// `MirrorSessionStateToConfiguration` would silently treat such
/// a source as having zero dedup keys -- two paths that differ
/// only by Windows case would slip through as duplicates.
///
/// The backfill is idempotent: when the arrays already have the
/// same length it returns without re-scanning, so it is cheap to
/// call after every load / mirror.
inline void BackfillLocatorDedupKeys(loglib::LogConfiguration::Source &source)
{
    if (source.locators.size() == source.locatorDedupKeys.size())
    {
        return;
    }
    source.locatorDedupKeys.clear();
    source.locatorDedupKeys.reserve(source.locators.size());
    for (const std::string &display : source.locators)
    {
        source.locatorDedupKeys.push_back(CanonicalLocator(QString::fromStdString(display)).toStdString());
    }
}

/// Overload for the `std::optional<Source>` shape we store on
/// `MainWindow::mCurrentSource` and friends. No-op when the
/// optional is unset, so callers can sprinkle this in immediately
/// after `mCurrentSource = ...` without an additional has_value
/// check.
inline void BackfillLocatorDedupKeys(std::optional<loglib::LogConfiguration::Source> &source)
{
    if (source.has_value())
    {
        BackfillLocatorDedupKeys(*source);
    }
}

} // namespace logapp
