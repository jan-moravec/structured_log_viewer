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

/// Parsed CLI state. `files` holds the positional arguments
/// canonicalised against the caller's CWD. `allowNewInstance` is
/// true when `--new-instance` is set or `LOGAPP_NEW_INSTANCE` is
/// `1` / `true` (the env override is useful for CI runs that
/// spawn many windows).
struct ParsedCli
{
    QStringList files;
    bool allowNewInstance = false;
};

/// Parse CLI arguments. Unknown flags are reported to stderr but
/// the positional arguments still flow through, so a typo in the
/// middle of an open-files gesture does not fail the entire launch.
/// `env` lets tests drive the parser with a hermetic environment;
/// production passes `QProcessEnvironment::systemEnvironment()`.
[[nodiscard]] inline ParsedCli ParseCli(const QStringList &args, const QProcessEnvironment &env)
{
    ParsedCli result;

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Structured Log Viewer"));
    parser.setSingleDashWordOptionMode(QCommandLineParser::ParseAsLongOptions);

    const QCommandLineOption newInstanceOption(
        QStringLiteral("new-instance"),
        QStringLiteral("Run a fresh process without coordinating with the canonical primary. "
                       "Useful for side-by-side debugging or running an alternate session "
                       "without disturbing the running primary.")
    );
    parser.addOption(newInstanceOption);
    parser.addPositionalArgument(
        QStringLiteral("files"),
        QStringLiteral("Optional list of log files (or session JSONs) to open on launch."),
        QStringLiteral("[files...]")
    );

    // `parse()` does not exit on error; surface the diagnostic but
    // still keep the parsed positionals and flags.
    if (!parser.parse(args))
    {
        ::logapp::LogWarning() << "Unrecognised CLI argument:" << parser.errorText();
    }

    result.allowNewInstance = parser.isSet(newInstanceOption);

    const QString envOverride = env.value(QStringLiteral("LOGAPP_NEW_INSTANCE"));
    if (envOverride == QStringLiteral("1") || envOverride.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0)
    {
        result.allowNewInstance = true;
    }

    // Store display-form paths (case preserved on Windows); the dedup
    // key is computed later by `StreamNextPendingFile` when the path
    // lands on a `Source`.
    for (const QString &positional : parser.positionalArguments())
    {
        if (positional.isEmpty())
        {
            continue;
        }
        result.files.append(CanonicalDisplayPath(positional));
    }
    return result;
}

} // namespace logapp
