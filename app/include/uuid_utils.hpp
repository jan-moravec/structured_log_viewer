#pragma once

#include <loglib/log_configuration.hpp>

#include <QDir>
#include <QFileInfo>
#include <QString>
#include <QUuid>

#include <optional>

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

/// Canonical dedup key for a file locator: absolute path, with
/// directory separators and case normalised on Windows so two
/// references to the same file collapse via byte-equality. Used
/// internally as the dedup key; never shown to the user (see
/// `CanonicalDisplayPath` for the case-preserving sibling).
///
/// Stale / non-existent paths return their absolute form without
/// further mutation -- locators are stored at save time and may
/// legitimately refer to files that have since moved.
[[nodiscard]] inline QString CanonicalLocator(const QString &locator)
{
    if (locator.isEmpty())
    {
        return locator;
    }
    QString absolute = QFileInfo(locator).absoluteFilePath();
#ifdef Q_OS_WIN
    // Windows is case-insensitive and treats `\` and `/` as equivalent.
    absolute.replace(QLatin1Char('\\'), QLatin1Char('/'));
    absolute = absolute.toLower();
#endif
    return absolute;
}

/// Display path for a file locator: absolute, forward-slashed on
/// Windows, but with the user's original case preserved. This is
/// the form persisted on `Source::locators`, shown in tooltips,
/// and passed to `QFile::open`. Pair every `CanonicalLocator`
/// call with a matching `CanonicalDisplayPath` (push both via
/// `loglib::AppendLocator` to keep the parallel arrays in step).
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

/// Ensure `Source::locatorDedupKeys` has exactly one entry per
/// `Source::locators`. Idempotent (no-op when the arrays already
/// match). Used after loading a session JSON predating the
/// schema split, and by test fixtures that build `Source` via
/// designated initialisers without setting the dedup-keys vector.
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

/// Optional overload. No-op when unset.
inline void BackfillLocatorDedupKeys(std::optional<loglib::LogConfiguration::Source> &source)
{
    if (source.has_value())
    {
        BackfillLocatorDedupKeys(*source);
    }
}

} // namespace logapp
