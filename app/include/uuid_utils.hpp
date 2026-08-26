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
/// `loglib::AppendLocator`).
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

/// Ensure every locator has a dedup key. Idempotent when keys are already
/// populated. Used after loading session JSON that omitted
/// `locatorDedupKeys`, and by test fixtures that set only display paths.
inline void BackfillLocatorDedupKeys(loglib::Source &source)
{
    for (loglib::SourceLocator &locator : source.locators)
    {
        if (!locator.dedupKey.empty())
        {
            continue;
        }
        locator.dedupKey = CanonicalLocator(QString::fromStdString(locator.displayPath)).toStdString();
    }
}

/// Optional overload. No-op when unset.
inline void BackfillLocatorDedupKeys(std::optional<loglib::Source> &source)
{
    if (source.has_value())
    {
        BackfillLocatorDedupKeys(*source);
    }
}

} // namespace logapp
