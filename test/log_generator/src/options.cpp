#include <log_generator/options.hpp>

#include <CLI/CLI.hpp>
#include <date/date.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace log_generator
{

namespace
{

// Line counts use SI multipliers: `10k` is 10,000 lines.
CLI::AsNumberWithUnit CountWithSuffix()
{
    return CLI::AsNumberWithUnit(
        std::map<std::string, std::uint64_t>{{"k", 1'000ULL}, {"m", 1'000'000ULL}, {"g", 1'000'000'000ULL}},
        CLI::AsNumberWithUnit::CASE_INSENSITIVE,
        "K|M|G"
    );
}

// Sizes use binary multipliers for K, KB, and KiB alike, matching `slp`.
CLI::AsSizeValue SizeWithSuffix()
{
    return CLI::AsSizeValue(false);
}

// Rejects zero and negative values with a readable message; runs after any unit transform.
CLI::Validator Positive()
{
    return {
        [](std::string &value) {
            const bool positive = !value.starts_with('-') && value.find_first_not_of("0.") != std::string::npos;
            return positive ? std::string() : std::string("must be greater than 0");
        },
        ""
    };
}

// Option callback that maps a choice, ignoring case, onto an enum value and
// rejects anything else with the list of valid choices.
template <typename Enum>
std::function<void(const std::string &)> SetChoice(
    Enum &destination, std::string optionName, std::map<std::string, Enum> choices
)
{
    // CLI11 reports a rejected value through the exception thrown here.
    // NOLINTNEXTLINE(bugprone-exception-escape)
    return [&destination, optionName = std::move(optionName), choices = std::move(choices)](const std::string &value) {
        std::string lower = value;
        std::ranges::transform(lower, lower.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        const auto it = choices.find(lower);
        if (it == choices.end())
        {
            std::string names;
            for (const auto &choice : choices)
            {
                names += (names.empty() ? "" : ", ") + choice.first;
            }
            throw CLI::ValidationError(optionName, "unknown value '" + value + "' (expected " + names + ")");
        }
        destination = it->second;
    };
}

CLI::AsNumberWithUnit DurationInMilliseconds()
{
    return CLI::AsNumberWithUnit(
        std::map<std::string, std::int64_t>{
            {"ms", 1}, {"s", 1'000}, {"m", 60'000}, {"h", 3'600'000}, {"d", 86'400'000}
        },
        static_cast<CLI::AsNumberWithUnit::Options>(
            CLI::AsNumberWithUnit::CASE_INSENSITIVE | CLI::AsNumberWithUnit::UNIT_REQUIRED
        ),
        "ms|s|m|h|d"
    );
}

void AddOutputOptions(CLI::App &app, GeneratorOptions &options, CLI::Option *&outputOpt)
{
    auto *group = app.add_option_group("Output", "What is written and where");
    group
        ->add_option_function<std::string>(
            "-f,--format",
            [&options](const std::string &value) {
                options.format = FindFormat(value);
                if (options.format == nullptr)
                {
                    throw CLI::ValidationError("--format", "unknown format '" + value + "'; run --list-formats");
                }
            },
            "Line format (default: json): json, logfmt, csv, syslog, apache-combined, apache-common, "
            "apache-error, or java"
        )
        ->option_text("FORMAT");
    outputOpt = group
                    ->add_option_function<std::string>(
                        "-o,--output",
                        [&options](const std::string &value) {
                            try
                            {
                                options.target = ParseTarget(value);
                            }
                            catch (const std::invalid_argument &err)
                            {
                                throw CLI::ValidationError("--output", err.what());
                            }
                        },
                        "File path, '-' for stdout, or tcp://HOST:PORT, tcp+tls://HOST:PORT, udp://HOST:PORT. "
                        "Defaults to generated.<ext> (generated_<format>.log for regex-template formats)"
                    )
                    ->option_text("DEST");
    group->add_flag("--append", options.append, "Append to an existing file instead of overwriting it");
}

void AddLimitOptions(CLI::App &app, GeneratorOptions &options)
{
    auto *group = app.add_option_group(
        "Limits", "Generation stops at the first limit reached; with no limit given, it stops after 10MiB"
    );
    auto *linesOpt =
        group->add_option("-n,--lines", options.limits.lines, "Stop after this many lines (K/M/G = 10^3/6/9)")
            ->transform(CountWithSuffix())
            ->check(Positive())
            ->option_text("COUNT");
    auto *sizeOpt =
        group->add_option("-s,--size", options.limits.bytes, "Stop after this many bytes (K/M/G = 2^10/20/30)")
            ->transform(SizeWithSuffix())
            ->check(Positive())
            ->option_text("SIZE");
    auto *durationOpt =
        group
            ->add_option_function<std::int64_t>(
                "-d,--duration",
                [&options](std::int64_t millis) { options.limits.duration = std::chrono::milliseconds(millis); },
                "Stop once this much time is covered: wall-clock time when live, virtual time with --start-time"
            )
            ->transform(DurationInMilliseconds())
            ->check(Positive())
            ->option_text("SPAN");
    group->add_flag("--forever", options.limits.forever, "Run until interrupted")
        ->excludes(linesOpt)
        ->excludes(sizeOpt)
        ->excludes(durationOpt);
}

CLI::Option *AddTimingOptions(CLI::App &app, GeneratorOptions &options)
{
    auto *group = app.add_option_group(
        "Timing",
        "Live runs stamp lines with the wall clock. --rate paces them like real traffic; without it they are written "
        "as fast as possible. --start-time switches to a virtual clock that never sleeps."
    );
    group->add_option("--rate", options.timing.rate, "Average lines per second (simulated default: 20)")
        ->check(Positive())
        ->option_text("N");
    auto *profileOpt =
        group
            ->add_option_function<std::string>(
                "--profile",
                SetChoice(
                    options.timing.profile,
                    "--profile",
                    {{"daily", test_common::TrafficProfile::Daily}, {"flat", test_common::TrafficProfile::Flat}}
                ),
                "Rate shape (default: daily): 'daily' varies 0.2x-1.8x over 24h, peaking at 14:00 UTC; 'flat' keeps "
                "it constant"
            )
            ->option_text("daily|flat");
    group->add_flag(
        "--bursts", options.timing.bursts, "Add 5-45s incident bursts at 10x rate, mostly warning and error records"
    );
    group
        ->add_option_function<std::string>(
            "--start-time",
            [&options](const std::string &value) {
                try
                {
                    options.timing.startTime = ParseStartTime(value);
                }
                catch (const std::invalid_argument &err)
                {
                    throw CLI::ValidationError("--start-time", err.what());
                }
            },
            "Simulate time from this UTC instant ('now' or YYYY-MM-DD[THH:MM:SS[.mmm]]) without sleeping"
        )
        ->option_text("TIME");
    return profileOpt;
}

void AddRotationOptions(CLI::App &app, GeneratorOptions &options)
{
    auto *group = app.add_option_group("Rotation", "Rotate the output file while writing (file output only)");
    group->add_option("--roll-size", options.rotation.maxBytes, "Rotate once the file reaches this size")
        ->transform(SizeWithSuffix())
        ->check(Positive())
        ->option_text("SIZE");
    group->add_option("--roll-lines", options.rotation.maxLines, "Rotate once the file has this many lines")
        ->transform(CountWithSuffix())
        ->check(Positive())
        ->option_text("COUNT");
    group
        ->add_option_function<std::string>(
            "--roll-strategy",
            SetChoice(
                options.rotation.strategy,
                "--roll-strategy",
                {{"rename", RollStrategy::Rename},
                 {"copytruncate", RollStrategy::CopyTruncate},
                 {"truncate", RollStrategy::Truncate}}
            ),
            "How to rotate (default: rename): 'rename' moves the file to .1 and recreates it, 'copytruncate' "
            "copies it to .1 and truncates it, 'truncate' truncates it without a backup"
        )
        ->option_text("STRATEGY");
    group
        ->add_option(
            "--roll-keep",
            options.rotation.keep,
            "Backups to keep as <output>.1 .. <output>.N (default: 5; 0 keeps none)"
        )
        ->option_text("N");
}

std::vector<CLI::Option *> AddTlsOptions(CLI::App &app, GeneratorOptions &options)
{
    auto *group = app.add_option_group("TLS", "Settings for tcp+tls:// output");
    const auto setPath = [](std::filesystem::path &destination) {
        return [&destination](const std::string &value) { destination = value; };
    };
    auto *caOpt = group->add_option_function<std::string>(
        "--tls-ca", setPath(options.tls.caBundle), "PEM CA bundle that verifies the server certificate"
    );
    caOpt->option_text("FILE");
    auto *certOpt = group->add_option_function<std::string>(
        "--tls-cert", setPath(options.tls.clientCert), "Client certificate PEM for mutual TLS"
    );
    auto *keyOpt = group->add_option_function<std::string>(
        "--tls-key", setPath(options.tls.clientKey), "Client private key PEM for mutual TLS"
    );
    certOpt->needs(keyOpt)->option_text("FILE");
    keyOpt->needs(certOpt)->option_text("FILE");
    auto *sniOpt = group->add_option(
        "--tls-sni", options.tls.serverNameIndication, "SNI host name for the handshake (default: the output host)"
    );
    sniOpt->option_text("HOST");
    auto *skipOpt = group->add_flag(
        "--tls-skip-verify", options.tls.insecureSkipVerify, "Accept any server certificate, e.g. self-signed dev certs"
    );
    return {caOpt, certOpt, keyOpt, sniOpt, skipOpt};
}

} // namespace

void AddOptions(CLI::App &app, GeneratorOptions &options)
{
    app.add_flag_callback(
        "--list-formats",
        []() {
            PrintFormatList(std::cout);
            throw CLI::Success();
        },
        "Print the supported formats and exit"
    );
    app.add_option("--seed", options.seed, "RNG seed for reproducible output (default: random)")->option_text("N");
    app.add_flag("-q,--quiet", options.quiet, "Print no progress messages; errors still go to stderr");

    CLI::Option *outputOpt = nullptr;
    AddOutputOptions(app, options, outputOpt);
    AddLimitOptions(app, options);
    const CLI::Option *profileOpt = AddTimingOptions(app, options);
    AddRotationOptions(app, options);
    const std::vector<CLI::Option *> tlsOpts = AddTlsOptions(app, options);

    // CLI11 reports a failed check through the exception thrown here.
    // NOLINTNEXTLINE(bugprone-exception-escape)
    app.callback([&options, outputOpt, profileOpt, tlsOpts]() {
        if (outputOpt->count() == 0)
        {
            options.target = Target{.kind = TargetKind::File, .path = DefaultFileName(*options.format)};
        }
        const bool fileTarget = options.target.kind == TargetKind::File;
        if (!fileTarget && options.rotation.Enabled())
        {
            throw CLI::ValidationError("--roll-size/--roll-lines", "rotation needs a file --output");
        }
        if (!fileTarget && options.append)
        {
            throw CLI::ValidationError("--append", "appending needs a file --output");
        }
        const bool tlsGiven =
            std::any_of(tlsOpts.begin(), tlsOpts.end(), [](const CLI::Option *opt) { return opt->count() > 0; });
        if (tlsGiven && options.target.kind != TargetKind::TcpTls)
        {
            throw CLI::ValidationError("--tls-*", "TLS options need a tcp+tls:// --output");
        }
        const bool paced = options.timing.rate.has_value() || options.timing.Simulated();
        if (!paced && (profileOpt->count() > 0 || options.timing.bursts))
        {
            throw CLI::ValidationError("--profile/--bursts", "traffic shaping needs --rate or --start-time");
        }
        const Limits &limits = options.limits;
        if (!limits.forever && !limits.lines && !limits.bytes && !limits.duration)
        {
            options.limits.bytes = DEFAULT_SIZE_LIMIT;
        }
    });
}

std::chrono::system_clock::time_point ParseStartTime(const std::string &text)
{
    if (text == "now")
    {
        return std::chrono::system_clock::now();
    }
    std::string trimmed = text;
    if (!trimmed.empty() && (trimmed.back() == 'Z' || trimmed.back() == 'z'))
    {
        trimmed.pop_back();
    }
    for (const char *fmt : {"%FT%T", "%F"})
    {
        std::istringstream in(trimmed);
        std::chrono::sys_time<std::chrono::milliseconds> tp;
        // Qualified call: `date::parse` makes `from_stream` ambiguous with `std::chrono::from_stream` on libc++.
        date::from_stream(in, fmt, tp);
        if (!in.fail() && in.peek() == std::istringstream::traits_type::eof())
        {
            return tp;
        }
    }
    throw std::invalid_argument("'" + text + "' must be 'now' or YYYY-MM-DD[THH:MM:SS[.mmm]] (UTC)");
}

} // namespace log_generator
