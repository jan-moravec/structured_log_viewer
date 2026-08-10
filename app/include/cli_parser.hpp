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
/// spawn many windows). `readStdin` is true when either `-` (as a
/// positional) or `--stdin` was passed; `main()` opens a live-tail
/// session over the process's `stdin` in that case, after any
/// positional file paths were opened. `disableRotationHistory`
/// disables rotated-sibling expansion for this launch.
struct ParsedCli
{
    QStringList files;
    bool allowNewInstance = false;
    bool readStdin = false;
    bool disableRotationHistory = false;
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
        QStringLiteral(
            "Run a fresh process without coordinating with the canonical primary. "
            "Useful for side-by-side debugging or running an alternate session "
            "without disturbing the running primary."
        )
    );
    parser.addOption(newInstanceOption);
    const QCommandLineOption stdinOption(
        QStringLiteral("stdin"), QStringLiteral("Read log lines from standard input (equivalent to passing `-`).")
    );
    parser.addOption(stdinOption);
    const QCommandLineOption noRotationHistoryOption(
        QStringLiteral("no-rotation-history"),
        QStringLiteral(
            "Do not auto-detect and load rotated companion files (`app.log.1`, "
            "`app.log-2025-04-28.gz`, ...) alongside the opened primary. Overrides "
            "the global preference for this launch."
        )
    );
    parser.addOption(noRotationHistoryOption);
    parser.addPositionalArgument(
        QStringLiteral("files"),
        QStringLiteral("Optional list of log files (or session JSONs) to open on launch."),
        QStringLiteral("[files...]")
    );

    // `-` alone is the longstanding UNIX convention for "read
    // stdin". Pre-strip it so `QCommandLineParser` (which we run
    // in `ParseAsLongOptions` mode) doesn't reject the bare dash
    // as an unknown short option. `--stdin` flows through the
    // parser normally so `--help` documents it and typos surface
    // as unknown-option warnings.
    //
    // `args[0]` is the program name -- `QCommandLineParser` and
    // Qt in general expect it in-place, and a hypothetical exe
    // renamed to just `-` should not be misinterpreted as the
    // stdin sentinel. Skip index 0 in the pre-strip.
    QStringList filteredArgs;
    filteredArgs.reserve(args.size());
    if (!args.isEmpty())
    {
        filteredArgs.append(args.first());
    }
    for (qsizetype i = 1; i < args.size(); ++i)
    {
        const QString &arg = args.at(i);
        if (arg == QStringLiteral("-"))
        {
            result.readStdin = true;
            continue;
        }
        filteredArgs.append(arg);
    }

    // `parse()` does not exit on error; surface the diagnostic but
    // still keep the parsed positionals and flags.
    if (!parser.parse(filteredArgs))
    {
        ::logapp::LogWarning() << "Unrecognised CLI argument:" << parser.errorText();
    }

    result.allowNewInstance = parser.isSet(newInstanceOption);
    if (parser.isSet(stdinOption))
    {
        result.readStdin = true;
    }
    if (parser.isSet(noRotationHistoryOption))
    {
        result.disableRotationHistory = true;
    }

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
