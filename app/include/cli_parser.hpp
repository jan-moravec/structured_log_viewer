#pragma once

#include "log_warning.hpp"
#include "uuid_utils.hpp"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>
#include <Qt>

namespace logapp
{

/// Parsed CLI state shared between `main()` and the test harness.
/// `files` is the list of positional arguments, canonicalised
/// against the caller's CWD via `CanonicalLocator`. `allowNewInstance`
/// reflects both the `--new-instance` flag and the
/// `LOGAPP_NEW_INSTANCE` env override (set the env var to `1` or
/// `true` to opt every launch out of single-instance coordination,
/// useful for CI runs that spawn many windows).
struct ParsedCli
{
    QStringList files;
    bool allowNewInstance = false;
};

/// Single declarative source of truth for CLI parsing. Replaces the
/// pre-fix hand-rolled `CollectCliFiles` + `ShouldAllowNewInstance`
/// pair, which duplicated the flag table and silently dropped
/// unknown long-form flags. `QCommandLineParser` rejects unknown
/// flags via `parse`'s error path; we surface the error to stderr
/// for shell-script consumers but still proceed with the parsed
/// positionals so an unknown flag in the middle of an open-files
/// gesture does not fail the entire launch.
///
/// The `env` argument exists so tests can drive the parser with a
/// hermetic env -- production callers pass
/// `QProcessEnvironment::systemEnvironment()`.
[[nodiscard]] inline ParsedCli ParseCli(const QStringList &args, const QProcessEnvironment &env)
{
    ParsedCli result;

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Structured Log Viewer"));
    parser.setSingleDashWordOptionMode(QCommandLineParser::ParseAsLongOptions);

    QCommandLineOption newInstanceOption(
        QStringLiteral("new-instance"),
        QStringLiteral(
            "Run a fresh process without coordinating with the canonical primary. "
            "Useful for side-by-side debugging or running an alternate session "
            "without disturbing the running primary."
        )
    );
    parser.addOption(newInstanceOption);
    parser.addPositionalArgument(
        QStringLiteral("files"),
        QStringLiteral("Optional list of log files (or session JSONs) to open on launch."),
        QStringLiteral("[files...]")
    );

    // `parse()` (as opposed to `process()`) does not call `exit()` on
    // error; we surface the diagnostic and still salvage the
    // positional arguments + flags so the user sees a window.
    if (!parser.parse(args))
    {
        LOGAPP_WARN() << "Unrecognised CLI argument:" << parser.errorText();
    }

    result.allowNewInstance = parser.isSet(newInstanceOption);

    const QString envOverride = env.value(QStringLiteral("LOGAPP_NEW_INSTANCE"));
    if (envOverride == QStringLiteral("1")
        || envOverride.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0)
    {
        result.allowNewInstance = true;
    }

    // `absoluteFilePath` (inside `CanonicalLocator`) resolves
    // against the caller's CWD without requiring the file to exist
    // (unlike `canonicalFilePath`, which would silently drop a user
    // typo). The locator then has slashes + case normalised on
    // Windows so dedup works across mixed-style paths.
    for (const QString &positional : parser.positionalArguments())
    {
        if (positional.isEmpty())
        {
            continue;
        }
        result.files.append(CanonicalLocator(positional));
    }
    return result;
}

} // namespace logapp
