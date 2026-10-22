#include "options.hpp"

#include "diagnostics.hpp"
#include "version.hpp"

#include <loglib/line_source.hpp>
#include <loglib/session_bundle.hpp>
#include <loglib/version.hpp>

#include <argparse/argparse.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <format>
#include <limits>
#include <string>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace slp
{

namespace
{

constexpr int SECONDS_PER_MINUTE = 60;
constexpr int MINUTES_PER_HOUR = 60;
constexpr std::uint64_t KIBIBYTE = 1024;
constexpr std::uint64_t MEBIBYTE = KIBIBYTE * KIBIBYTE;
constexpr std::uint64_t GIBIBYTE = MEBIBYTE * KIBIBYTE;
constexpr int MIN_TCP_PORT = 1;
constexpr int MAX_TCP_PORT = 65535;
constexpr double MIN_ERROR_RATE = 0.0;
constexpr double MAX_ERROR_RATE = 100.0;

constexpr std::string_view ADVANCED_HELP = "\nAdvanced options:\n"
                                           "  --batch-size <bytes>  Parser batch size. Accepts suffixes such as 1MiB "
                                           "(powers of 1024). The default is the library batch size.\n";

struct HelpRequest : std::exception
{
    std::string text;
    bool advanced = false;

    explicit HelpRequest(std::string helpText, bool showAdvanced)
        : text(std::move(helpText)), advanced(showAdvanced)
    {
    }

    [[nodiscard]] const char *what() const noexcept override
    {
        return "help";
    }
};

struct VersionRequest : std::exception
{
    [[nodiscard]] const char *what() const noexcept override
    {
        return "version";
    }
};

[[nodiscard]] bool EqualsIgnoreCase(std::string_view lhs, std::string_view rhs) noexcept
{
    if (lhs.size() != rhs.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i)
    {
        const auto left = static_cast<unsigned char>(lhs[i]);
        const auto right = static_cast<unsigned char>(rhs[i]);
        if (std::tolower(left) != std::tolower(right))
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool StartsWithIgnoreCase(std::string_view text, std::string_view prefix) noexcept
{
    return text.size() >= prefix.size() && EqualsIgnoreCase(text.substr(0, prefix.size()), prefix);
}

void AddSwitch(argparse::ArgumentParser &parser, std::string_view name, std::string help)
{
    parser.add_argument(name).default_value(false).implicit_value(true).nargs(0).help(std::move(help));
}

void AddSwitch(
    argparse::ArgumentParser &parser, std::string_view shortName, std::string_view longName, std::string help
)
{
    parser.add_argument(shortName, longName).default_value(false).implicit_value(true).nargs(0).help(std::move(help));
}

void AddHelpFlag(argparse::ArgumentParser &parser)
{
    parser.add_argument("-h", "--help")
        .action([&parser](const std::string &) { throw HelpRequest(parser.help().str(), false); })
        .default_value(false)
        .implicit_value(true)
        .nargs(0)
        .help("Show this help and exit 0.");
}

void AddAdvancedHelpFlag(argparse::ArgumentParser &parser)
{
    parser.add_argument("--help-advanced")
        .action([&parser](const std::string &) { throw HelpRequest(parser.help().str(), true); })
        .default_value(false)
        .implicit_value(true)
        .nargs(0)
        .help("Show flags omitted from the default help and exit 0.");
}

void AddGlobalFlags(argparse::ArgumentParser &parser, int &verboseCount)
{
    AddSwitch(
        parser, "-q", "--quiet", "Suppress warnings and progress. Errors still print. The default is to print warnings."
    );
    parser.add_argument("-v", "--verbose")
        .action([&verboseCount](const std::string &) { ++verboseCount; })
        .append()
        .default_value(false)
        .implicit_value(true)
        .nargs(0)
        .help("Print per-stage timing and row counts to stderr. Repeat for parser-level detail. The default is off.");
    parser.add_argument("--color")
        .default_value(std::string{"auto"})
        .nargs(1)
        .choices("auto", "always", "never")
        .help(
            "Colorize human-readable text. One of auto, always, or never. The default is auto, which colors only "
            "when stderr is a terminal and NO_COLOR is unset."
        );
    parser.add_argument("--threads")
        .default_value(std::string{"0"})
        .nargs(1)
        .metavar("N")
        .help("Number of parser threads forwarded to the library. 0 selects the library default. The default is 0.");
    parser.add_argument("--tz")
        .default_value(std::string{"local"})
        .nargs(1)
        .metavar("ZONE")
        .help(
            "Time zone for naive timestamps and human-readable times. An IANA name, local, or UTC. The default is "
            "local."
        );
    AddSwitch(
        parser,
        "--no-rotation-history",
        "Skip rotated-sibling discovery. The default is to load numbered and dated siblings, oldest first."
    );
    parser.add_argument("--template-dir")
        .metavar("DIR")
        .help(
            "Directory of user regex templates. Overrides SLV_TEMPLATE_DIR and the default user directory. The "
            "default is the platform user-template directory."
        );
}

void AddParserFlags(argparse::ArgumentParser &parser)
{
    parser.add_argument("-f", "--format")
        .default_value(std::string{"auto"})
        .nargs(1)
        .choices("auto", "json", "logfmt", "csv", "regex")
        .help(
            "Parser to use. One of auto, json, logfmt, csv, or regex. The default is auto. --regex and "
            "--regex-template imply regex."
        );
    parser.add_argument("--regex").metavar("PATTERN").help(
        "PCRE2 pattern with named captures. Implies --format regex. Mutually exclusive with --regex-template."
    );
    parser.add_argument("--regex-template")
        .metavar("SLUG")
        .help(
            "Built-in or user regex template slug, as printed by `slp formats`. Implies --format regex. Mutually "
            "exclusive with --regex."
        );
    AddSwitch(
        parser, "--no-multiline-logfmt", "Parse each logfmt line on its own. The default is to join continuations."
    );
    parser.add_argument("--batch-size")
        .metavar("BYTES")
        .hidden()
        .help("Parser batch size, for example 1MiB. The default is the library batch size.");
}

void AddFilterFlags(argparse::ArgumentParser &parser, bool includeUntil)
{
    parser.add_argument("--filter")
        .append()
        .metavar("QUERY")
        .help(
            "Filter query in the GUI syntax. Repeat to combine with AND in the order given. The default is to keep "
            "every row."
        );
    parser.add_argument("--since").metavar("TIME").help(
        "Keep rows at or after this time on the first time column. Accepts the column's formats, ISO-8601, now, "
        "and relative values such as -30m. The default is no lower bound."
    );
    if (includeUntil)
    {
        parser.add_argument("--until").metavar("TIME").help(
            "Keep rows at or before this time on the first time column. The same forms as --since are accepted. "
            "The default is no upper bound."
        );
    }
    parser.add_argument("--min-level")
        .choices("trace", "debug", "info", "warn", "error", "fatal")
        .metavar("LEVEL")
        .help(
            "Keep rows at or above this canonical level. One of trace, debug, info, warn, error, or fatal. The "
            "default is to keep every level."
        );
    AddSwitch(
        parser, "-x", "--invert", "Keep rows the filter rejects. The default is to keep rows the filter accepts."
    );
}

void AddSources(argparse::ArgumentParser &parser, std::string help)
{
    parser.add_argument("source").nargs(argparse::nargs_pattern::any).metavar("source").help(std::move(help));
}

[[nodiscard]] bool Flag(const argparse::ArgumentParser &parser, std::string_view name)
{
    return parser.is_used(name);
}

[[nodiscard]] bool FlagEither(
    const argparse::ArgumentParser &parent, const argparse::ArgumentParser &child, std::string_view name
)
{
    return parent.is_used(name) || child.is_used(name);
}

[[nodiscard]] std::optional<std::string> OptionalString(const argparse::ArgumentParser &parser, std::string_view name)
{
    if (!parser.is_used(name))
    {
        return std::nullopt;
    }
    return parser.get<std::string>(name);
}

[[nodiscard]] std::string PickString(
    const argparse::ArgumentParser &parent, const argparse::ArgumentParser &child, std::string_view name
)
{
    if (child.is_used(name))
    {
        return child.get<std::string>(name);
    }
    if (parent.is_used(name))
    {
        return parent.get<std::string>(name);
    }
    return child.get<std::string>(name);
}

[[nodiscard]] std::vector<std::string> Repeated(const argparse::ArgumentParser &parser, std::string_view name)
{
    if (!parser.is_used(name))
    {
        return {};
    }
    return parser.get<std::vector<std::string>>(name);
}

[[nodiscard]] std::expected<std::uint64_t, std::string> ParseUint(std::string_view text, std::string_view flag)
{
    if (text.empty() || text.front() == '-' || text.front() == '+')
    {
        return std::unexpected(std::format("Flag {} requires a non-negative integer, not '{}'.", flag, text));
    }
    std::uint64_t value = 0;
    const char *const begin = text.data();
    const char *const end = text.data() + text.size();
    const std::from_chars_result parsed = std::from_chars(begin, end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end)
    {
        return std::unexpected(std::format("Flag {} requires a non-negative integer, not '{}'.", flag, text));
    }
    return value;
}

[[nodiscard]] std::expected<int, std::string> ParseInt(std::string_view text, std::string_view flag)
{
    int value = 0;
    const char *const begin = text.data();
    const char *const end = text.data() + text.size();
    const std::from_chars_result parsed = std::from_chars(begin, end, value);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != end)
    {
        return std::unexpected(std::format("Flag {} requires an integer, not '{}'.", flag, text));
    }
    return value;
}

[[nodiscard]] ColorMode ParseColor(std::string_view text)
{
    if (text == "always")
    {
        return ColorMode::Always;
    }
    if (text == "never")
    {
        return ColorMode::Never;
    }
    return ColorMode::Auto;
}

[[nodiscard]] InputFormat ParseInputFormat(std::string_view text)
{
    if (text == "json")
    {
        return InputFormat::Json;
    }
    if (text == "logfmt")
    {
        return InputFormat::Logfmt;
    }
    if (text == "csv")
    {
        return InputFormat::Csv;
    }
    if (text == "regex")
    {
        return InputFormat::Regex;
    }
    return InputFormat::Auto;
}

[[nodiscard]] bool EnvIsSet(const char *name)
{
#ifdef _WIN32
    const DWORD needed = GetEnvironmentVariableA(name, nullptr, 0);
    return needed != 0 || GetLastError() != ERROR_ENVVAR_NOT_FOUND;
#else
    return std::getenv(name) != nullptr;
#endif
}

[[nodiscard]] std::optional<std::string> EnvironmentString(const char *name)
{
#ifdef _WIN32
    const DWORD needed = GetEnvironmentVariableA(name, nullptr, 0);
    if (needed == 0)
    {
        return std::nullopt;
    }
    std::string value(static_cast<std::size_t>(needed), '\0');
    const DWORD written = GetEnvironmentVariableA(name, value.data(), needed);
    if (written == 0)
    {
        return std::nullopt;
    }
    value.resize(written);
    return value;
#else
    const char *value = std::getenv(name);
    if (value == nullptr || value[0] == '\0')
    {
        return std::nullopt;
    }
    return std::string(value);
#endif
}

[[nodiscard]] std::string TranslateArgparse(std::string_view what)
{
    std::string text(what);
    for (char &ch : text)
    {
        if (ch == '\n' || ch == '\r')
        {
            ch = ' ';
        }
    }
    constexpr std::string_view UNKNOWN = "Unknown argument: ";
    if (text.starts_with(UNKNOWN))
    {
        return std::format("Unknown flag '{}'.", text.substr(UNKNOWN.size()));
    }
    constexpr std::string_view TOO_FEW = "Too few arguments for '";
    if (text.starts_with(TOO_FEW))
    {
        std::string_view name = std::string_view(text).substr(TOO_FEW.size());
        if (!name.empty() && name.back() == '.')
        {
            name.remove_suffix(1);
        }
        if (!name.empty() && name.back() == '\'')
        {
            name.remove_suffix(1);
        }
        return std::format("Flag '{}' requires a value.", name);
    }
    if (!text.empty() && text.back() != '.')
    {
        text.push_back('.');
    }
    return text;
}

[[nodiscard]] std::string CommandToken(int argc, char **argv)
{
    static constexpr std::array<std::string_view, 7> kCommands = {
        "query",
        "stats",
        "schema",
        "validate",
        "bundle",
        "formats",
        "tail",
    };
    static constexpr std::array<std::string_view, 10> kValueFlags = {
        "--color",
        "--threads",
        "--tz",
        "--template-dir",
        "-f",
        "--format",
        "-t",
        "--output-format",
        "-o",
        "--output",
    };
    for (int index = 1; index < argc; ++index)
    {
        if (argv[index] == nullptr)
        {
            break;
        }
        const std::string_view arg{argv[index]};
        if (arg == "--")
        {
            break;
        }
        if (arg.starts_with('-'))
        {
            const auto equals = arg.find('=');
            const std::string_view name = equals == std::string_view::npos ? arg : arg.substr(0, equals);
            const bool takesValue = equals == std::string_view::npos &&
                                    std::find(kValueFlags.begin(), kValueFlags.end(), name) != kValueFlags.end();
            if (takesValue)
            {
                ++index;
            }
            continue;
        }
        if (std::find(kCommands.begin(), kCommands.end(), arg) != kCommands.end())
        {
            return std::string(arg);
        }
    }
    return {};
}

[[nodiscard]] const char *CommandName(Command command) noexcept
{
    switch (command)
    {
    case Command::Query:
        return "query";
    case Command::Stats:
        return "stats";
    case Command::Schema:
        return "schema";
    case Command::Validate:
        return "validate";
    case Command::Bundle:
        return "bundle";
    case Command::Formats:
        return "formats";
    case Command::Tail:
        return "tail";
    case Command::None:
        return "";
    }
    return "";
}

[[nodiscard]] std::expected<std::uint16_t, std::string> ParsePort(std::string_view text, std::string_view original)
{
    const auto parsed = ParseUint(text, "port");
    if (!parsed)
    {
        return std::unexpected(std::format("Network source '{}' has an invalid port.", original));
    }
    if (*parsed < static_cast<std::uint64_t>(MIN_TCP_PORT) || *parsed > static_cast<std::uint64_t>(MAX_TCP_PORT))
    {
        return std::unexpected(
            std::format(
                "Network source '{}' has port {}, which is outside {}-{}.",
                original,
                *parsed,
                MIN_TCP_PORT,
                MAX_TCP_PORT
            )
        );
    }
    return static_cast<std::uint16_t>(*parsed);
}

[[nodiscard]] std::expected<SourceSpec, std::string> ClassifyNetwork(std::string_view text, SourceKind kind)
{
    constexpr std::string_view kScheme = "://";
    const auto scheme = text.find(kScheme);
    if (scheme == std::string_view::npos)
    {
        return std::unexpected(std::format("Network source '{}' is missing '://'.", text));
    }
    const std::string_view rest = text.substr(scheme + kScheme.size());
    std::string_view host;
    std::string_view portText;
    if (rest.starts_with('['))
    {
        const auto end = rest.find(']');
        if (end == std::string_view::npos || end + 1 >= rest.size() || rest[end + 1] != ':')
        {
            return std::unexpected(std::format("Network source '{}' must look like tcp://[host]:port.", text));
        }
        host = rest.substr(1, end - 1);
        portText = rest.substr(end + 2);
    }
    else
    {
        const auto colon = rest.rfind(':');
        if (colon == std::string_view::npos)
        {
            return std::unexpected(std::format("Network source '{}' must include a port.", text));
        }
        host = rest.substr(0, colon);
        portText = rest.substr(colon + 1);
    }
    auto port = ParsePort(portText, text);
    if (!port)
    {
        return std::unexpected(std::move(port.error()));
    }
    SourceSpec spec;
    spec.kind = kind;
    spec.text = std::string(text);
    spec.host = std::string(host);
    spec.port = *port;
    return spec;
}

struct UsageMap
{
    std::string top;
    std::string query;
    std::string stats;
    std::string schema;
    std::string validate;
    std::string bundle;
    std::string formats;
    std::string tail;

    [[nodiscard]] const std::string &For(std::string_view command) const
    {
        if (command == "query")
        {
            return query;
        }
        if (command == "stats")
        {
            return stats;
        }
        if (command == "schema")
        {
            return schema;
        }
        if (command == "validate")
        {
            return validate;
        }
        if (command == "bundle")
        {
            return bundle;
        }
        if (command == "formats")
        {
            return formats;
        }
        if (command == "tail")
        {
            return tail;
        }
        return top;
    }
};

[[nodiscard]] std::expected<std::uint64_t, UsageError> RequireUint(
    const argparse::ArgumentParser &parser, std::string_view flag, std::string_view command, const std::string &usage
)
{
    const auto parsed = ParseUint(parser.get<std::string>(flag), flag);
    if (!parsed)
    {
        return std::unexpected(UsageError{std::string(command), parsed.error(), usage});
    }
    return *parsed;
}

void ApplyColor(Options &options)
{
    if (EnvIsSet("NO_COLOR"))
    {
        options.color = ColorMode::Never;
    }
}

void ApplyTemplateDir(Options &options, bool flagSet, std::string flagValue)
{
    if (flagSet)
    {
        options.templateDir = std::filesystem::path(std::move(flagValue));
        return;
    }
    if (const std::optional<std::string> env = EnvironmentString("SLV_TEMPLATE_DIR"))
    {
        options.templateDir = std::filesystem::path(*env);
    }
}

[[nodiscard]] std::expected<Options, UsageError> ValidateSemantics(Options options, const std::string &usage)
{
    const std::string command = CommandName(options.command);
    auto fail = [&](std::string message) -> std::unexpected<UsageError> {
        return std::unexpected(UsageError{command, std::move(message), usage});
    };

    int stdinCount = 0;
    bool network = false;
    for (const SourceSpec &source : options.sources)
    {
        if (source.kind == SourceKind::Stdin)
        {
            ++stdinCount;
        }
        if (source.kind == SourceKind::Tcp || source.kind == SourceKind::Udp)
        {
            network = true;
            if (options.command != Command::Tail)
            {
                return fail(std::format("Network source '{}' is only valid for `slp tail`.", source.text));
            }
        }
    }
    if (stdinCount > 1)
    {
        return fail("Stdin '-' may appear only once.");
    }
    if (stdinCount > 0 && network)
    {
        return fail("Stdin '-' cannot be combined with a network source.");
    }
    if (options.regex.has_value() && options.regexTemplate.has_value())
    {
        return fail("'--regex' cannot be combined with '--regex-template'.");
    }
    if ((options.regex.has_value() || options.regexTemplate.has_value()) && options.format != InputFormat::Auto &&
        options.format != InputFormat::Regex)
    {
        return fail(
            "'--regex' and '--regex-template' imply --format regex and cannot be combined with another format."
        );
    }
    if (options.regex.has_value() || options.regexTemplate.has_value())
    {
        options.format = InputFormat::Regex;
    }
    if (options.last.has_value() && options.offset.has_value())
    {
        return fail("'--last' cannot be combined with '--offset'.");
    }
    if (options.command == Command::Stats && options.statsBy == "column" && !options.column.has_value())
    {
        return fail("'--by column' requires '--column'.");
    }
    if (options.command == Command::Validate && options.strict)
    {
        return fail("'--strict' is not valid for validate; use '--max-error-rate'.");
    }
    if (options.command != Command::Validate && options.maxErrorRate.has_value())
    {
        return fail("'--max-error-rate' is only valid for validate; use '--strict' on row-producing commands.");
    }
    if (options.command == Command::Bundle && !options.outputExplicit)
    {
        return fail("'--output' is required for bundle.");
    }
    if (options.retention.has_value() && *options.retention != 0 &&
        (*options.retention < loglib::MIN_RETENTION_LINES || *options.retention > loglib::MAX_RETENTION_LINES))
    {
        return fail(
            std::format(
                "'--retention' must be 0 (unbounded) or from {} to {}.",
                loglib::MIN_RETENTION_LINES,
                loglib::MAX_RETENTION_LINES
            )
        );
    }
    if (options.maxErrorRate.has_value() &&
        (*options.maxErrorRate < MIN_ERROR_RATE || *options.maxErrorRate > MAX_ERROR_RATE))
    {
        return fail("'--max-error-rate' must be a percentage from 0 to 100.");
    }
    return options;
}

} // namespace

std::expected<std::uint64_t, std::string> ParseSizeSuffix(std::string_view text)
{
    if (text.empty())
    {
        return std::unexpected("A size must be an integer with an optional suffix such as 1MiB.");
    }
    std::size_t suffixAt = text.size();
    while (suffixAt > 0 && std::isalpha(static_cast<unsigned char>(text[suffixAt - 1])) != 0)
    {
        --suffixAt;
    }
    if (suffixAt == 0)
    {
        return std::unexpected(
            std::format("'{}' is not a size. Use an integer and an optional suffix such as 1MiB.", text)
        );
    }
    const auto number = ParseUint(text.substr(0, suffixAt), "size");
    if (!number)
    {
        return std::unexpected(number.error());
    }
    const std::string_view suffix = text.substr(suffixAt);
    std::uint64_t multiplier = 1;
    if (suffix.empty() || EqualsIgnoreCase(suffix, "B"))
    {
        multiplier = 1;
    }
    else if (EqualsIgnoreCase(suffix, "K") || EqualsIgnoreCase(suffix, "KB") || EqualsIgnoreCase(suffix, "KiB"))
    {
        multiplier = KIBIBYTE;
    }
    else if (EqualsIgnoreCase(suffix, "M") || EqualsIgnoreCase(suffix, "MB") || EqualsIgnoreCase(suffix, "MiB"))
    {
        multiplier = MEBIBYTE;
    }
    else if (EqualsIgnoreCase(suffix, "G") || EqualsIgnoreCase(suffix, "GB") || EqualsIgnoreCase(suffix, "GiB"))
    {
        multiplier = GIBIBYTE;
    }
    else
    {
        return std::unexpected(
            std::format("Unknown size suffix '{}'. Use B, K, KB, KiB, M, MB, MiB, G, GB, or GiB.", suffix)
        );
    }
    if (*number > std::numeric_limits<std::uint64_t>::max() / multiplier)
    {
        return std::unexpected(std::format("'{}' overflows a 64-bit byte count.", text));
    }
    return *number * multiplier;
}

std::expected<std::chrono::milliseconds, std::string> ParseDurationSuffix(std::string_view text)
{
    if (text.empty() || std::isalpha(static_cast<unsigned char>(text.back())) == 0)
    {
        return std::unexpected(
            std::format("'{}' is not a duration. Use a suffix of s, m, or h, for example 30s.", text)
        );
    }
    const char unit = static_cast<char>(std::tolower(static_cast<unsigned char>(text.back())));
    const auto number = ParseUint(text.substr(0, text.size() - 1), "duration");
    if (!number)
    {
        return std::unexpected(
            std::format("'{}' is not a duration. Use a suffix of s, m, or h, for example 30s.", text)
        );
    }
    std::uint64_t seconds = 0;
    if (unit == 's')
    {
        seconds = *number;
    }
    else if (unit == 'm')
    {
        seconds = *number * static_cast<std::uint64_t>(SECONDS_PER_MINUTE);
    }
    else if (unit == 'h')
    {
        seconds =
            *number * static_cast<std::uint64_t>(SECONDS_PER_MINUTE) * static_cast<std::uint64_t>(MINUTES_PER_HOUR);
    }
    else
    {
        return std::unexpected(std::format("Unknown duration suffix '{}'. Use s, m, or h.", unit));
    }
    constexpr std::uint64_t kMillisPerSecond = 1000;
    if (seconds > std::numeric_limits<std::uint64_t>::max() / kMillisPerSecond)
    {
        return std::unexpected(std::format("'{}' overflows a duration.", text));
    }
    return std::chrono::milliseconds(seconds * kMillisPerSecond);
}

std::expected<SourceSpec, std::string> ClassifySource(std::string_view text)
{
    if (text.empty())
    {
        return std::unexpected("A source path must not be empty.");
    }
    if (text == "-")
    {
        SourceSpec spec;
        spec.kind = SourceKind::Stdin;
        spec.text = "-";
        return spec;
    }
    if (StartsWithIgnoreCase(text, "tcp://"))
    {
        return ClassifyNetwork(text, SourceKind::Tcp);
    }
    if (StartsWithIgnoreCase(text, "udp://"))
    {
        return ClassifyNetwork(text, SourceKind::Udp);
    }
    if (StartsWithIgnoreCase(text, "tcp+tls://"))
    {
        return std::unexpected(
            std::format(
                "Network source '{}' uses TLS, which this version does not support. Use tcp:// for plaintext.", text
            )
        );
    }
    SourceSpec spec;
    spec.text = std::string(text);
    spec.path = std::filesystem::path(spec.text);
    const std::string extension = spec.path.extension().string();
    if (EqualsIgnoreCase(extension, loglib::SESSION_BUNDLE_EXTENSION))
    {
        spec.kind = SourceKind::Bundle;
    }
    else
    {
        spec.kind = SourceKind::Path;
    }
    return spec;
}

std::string VersionLine()
{
    return std::format("slp {} ({}) loglib {}", VERSION, PRODUCT_NAME, loglib::Version());
}

std::expected<Options, UsageError> ParseArguments(int argc, char **argv)
{
    int topVerbose = 0;
    int queryVerbose = 0;
    int statsVerbose = 0;
    int schemaVerbose = 0;
    int validateVerbose = 0;
    int bundleVerbose = 0;
    int formatsVerbose = 0;
    int tailVerbose = 0;

    argparse::ArgumentParser program("slp", std::string(VERSION), argparse::default_arguments::none, false);
    program.add_description(
        "StructuredLogProcessor reads, filters, and exports structured logs without a GUI. "
        "Run `slp <command> --help` for command flags."
    );
    program.add_epilog(std::string(ExitCodeHelpEpilog()));
    AddGlobalFlags(program, topVerbose);
    AddHelpFlag(program);
    program.add_argument("--version")
        .action([](const std::string &) { throw VersionRequest(); })
        .default_value(false)
        .implicit_value(true)
        .nargs(0)
        .help("Print the slp and loglib versions and exit 0.");

    argparse::ArgumentParser query("query", "", argparse::default_arguments::none, false);
    query.add_description("Filter, sort, and export rows. Sources concatenate in the order given.");
    query.add_epilog(std::string(ExitCodeHelpEpilog()));
    AddGlobalFlags(query, queryVerbose);
    AddParserFlags(query);
    AddFilterFlags(query, true);
    AddAdvancedHelpFlag(query);
    AddHelpFlag(query);
    query.add_argument("--sort")
        .metavar("COLUMN[:asc|desc]")
        .help("Sort by one column. The direction defaults to asc. Without --sort, rows stay in source order.");
    query.add_argument("--columns")
        .metavar("A,B,C")
        .help("Comma-separated columns to emit, in that order. The default is the visible set from the configuration.");
    AddSwitch(
        query, "--all-columns", "Emit every column. Overrides --columns and the configuration. The default is off."
    );
    query.add_argument("--limit").metavar("N").help(
        "Emit at most N rows after filtering and sorting. The default is no cap."
    );
    auto &querySlice = query.add_mutually_exclusive_group();
    querySlice.add_argument("--offset")
        .metavar("N")
        .help("Skip the first N result rows. Mutually exclusive with --last. The default is to skip none.");
    querySlice.add_argument("--last").metavar("N").help(
        "Emit only the last N result rows. Mutually exclusive with --offset. The default is the full result."
    );
    query.add_argument("-o", "--output")
        .default_value(std::string{"-"})
        .nargs(1)
        .metavar("PATH")
        .help("Write rows to this path, or - for stdout. The default is -.");
    query.add_argument("-t", "--output-format")
        .default_value(std::string{"jsonl"})
        .nargs(1)
        .choices("jsonl", "csv", "markdown", "snapshot")
        .help("Export format. One of jsonl, csv, markdown, or snapshot. The default is jsonl.");
    AddSwitch(query, "--no-header", "Omit the header row for csv and markdown. The default is to include it.");
    AddSwitch(
        query, "--json-visible-only", "Honor --columns in JSON Lines. The default is to emit every field of each row."
    );
    AddSwitch(query, "--force", "Overwrite an existing --output file. The default is to refuse and exit 5.");
    AddSwitch(
        query,
        "--count",
        "Print the matching row count and no rows. Ignores --output-format, --columns, --limit, and --offset. The "
        "default is to print rows."
    );
    AddSwitch(query, "--allow-empty", "Exit 0 when no row matches. The default is to exit 1 on an empty result.");
    AddSwitch(
        query, "--strict", "Exit 4 when the parse reported errors. The default is to exit 0 or 1 and keep the rows."
    );
    query.add_argument("--config")
        .metavar("PATH")
        .help("Session or columns JSON to apply before flags. The default is none.");
    query.add_argument("--save-session")
        .metavar("PATH")
        .help(
            "Write the effective full session JSON after the run. Does not change stdout. The default is not to write."
        );
    query.add_argument("--save-columns")
        .metavar("PATH")
        .help("Write columns and highlight rules only. Does not change stdout. The default is not to write.");
    query.add_argument("--max-error-rate")
        .metavar("PERCENT")
        .help("Not accepted on query. Use --strict instead. The default is unset.");
    const std::string sourceHelp = "Log file, .slvbundle, or - for stdin. Repeat to concatenate in the order given. "
                                   "tcp:// and udp:// are rejected here; use `slp tail`.";
    AddSources(query, sourceHelp);

    argparse::ArgumentParser stats("stats", "", argparse::default_arguments::none, false);
    stats.add_description("Summarise a source without emitting rows.");
    stats.add_epilog(std::string(ExitCodeHelpEpilog()));
    AddGlobalFlags(stats, statsVerbose);
    AddParserFlags(stats);
    AddFilterFlags(stats, true);
    AddAdvancedHelpFlag(stats);
    AddHelpFlag(stats);
    stats.add_argument("--by")
        .default_value(std::string{"level"})
        .nargs(1)
        .choices("level", "time", "column")
        .help("Summary axis. One of level, time, or column. The default is level.");
    stats.add_argument("--bucket")
        .default_value(std::string{"auto"})
        .nargs(1)
        .choices("1s", "10s", "1m", "10m", "1h", "1d", "auto")
        .help("Histogram width for --by time. The default is auto.");
    stats.add_argument("--max-buckets")
        .metavar("N")
        .help("Override the automatic bucket budget. The default is 60 for text and 500 for json and csv.");
    stats.add_argument("--top")
        .default_value(std::string{"20"})
        .nargs(1)
        .metavar("N")
        .help("How many distinct values --by column prints. The default is 20.");
    stats.add_argument("--column")
        .metavar("NAME")
        .help("Column to summarise for --by column. Required with that axis.");
    stats.add_argument("--output-format")
        .default_value(std::string{"text"})
        .nargs(1)
        .choices("text", "json", "csv")
        .help("Rendering. One of text, json, or csv. The default is text.");
    AddSources(stats, sourceHelp);

    argparse::ArgumentParser schema("schema", "", argparse::default_arguments::none, false);
    schema.add_description("Print the detected column schema without emitting rows.");
    schema.add_epilog(std::string(ExitCodeHelpEpilog()));
    AddGlobalFlags(schema, schemaVerbose);
    AddParserFlags(schema);
    AddAdvancedHelpFlag(schema);
    AddHelpFlag(schema);
    schema.add_argument("--sample")
        .default_value(std::string{"3"})
        .nargs(1)
        .metavar("N")
        .help("Sample values to print per column. 0 disables sampling. The default is 3.");
    schema.add_argument("--output-format")
        .default_value(std::string{"text"})
        .nargs(1)
        .choices("text", "json")
        .help("Rendering. One of text or json. The default is text.");
    AddSources(schema, sourceHelp);

    argparse::ArgumentParser validate("validate", "", argparse::default_arguments::none, false);
    validate.add_description("Report parse health without emitting rows.");
    validate.add_epilog(std::string(ExitCodeHelpEpilog()));
    AddGlobalFlags(validate, validateVerbose);
    AddParserFlags(validate);
    AddAdvancedHelpFlag(validate);
    AddHelpFlag(validate);
    validate.add_argument("--max-errors")
        .default_value(std::string{"20"})
        .nargs(1)
        .metavar("N")
        .help("How many individual parse errors to print in text mode. The default is 20.");
    validate.add_argument("--max-error-rate")
        .metavar("PERCENT")
        .help(
            "Exit 0 while the parse-error percentage stays at or below this value. The default is any error exits 4."
        );
    AddSwitch(
        validate, "--strict", "Not accepted on validate. Use --max-error-rate to relax the default, which is strict."
    );
    validate.add_argument("--output-format")
        .default_value(std::string{"text"})
        .nargs(1)
        .choices("text", "json")
        .help("Rendering. One of text or json. The default is text.");
    AddSources(validate, sourceHelp);

    argparse::ArgumentParser bundle("bundle", "", argparse::default_arguments::none, false);
    bundle.add_description(
        "Write a .slvbundle of every retained row. Filters, sort, and columns are stored in the bundle for the GUI "
        "and are not applied destructively unless --filtered-rows is set."
    );
    bundle.add_epilog(std::string(ExitCodeHelpEpilog()));
    AddGlobalFlags(bundle, bundleVerbose);
    AddParserFlags(bundle);
    AddFilterFlags(bundle, true);
    AddAdvancedHelpFlag(bundle);
    AddHelpFlag(bundle);
    bundle.add_argument("--sort")
        .metavar("COLUMN[:asc|desc]")
        .help("Sort stored in the bundle. The default is source order.");
    bundle.add_argument("--columns")
        .metavar("A,B,C")
        .help("Column set stored in the bundle. The default is the configuration's visible columns.");
    bundle.add_argument("-o", "--output")
        .metavar("PATH")
        .help("Destination path. A missing .slvbundle extension is appended. This flag is required.");
    AddSwitch(
        bundle,
        "--filtered-rows",
        "Write only matching rows and clear the embedded filter. The default is to keep every row and store the filter."
    );
    bundle.add_argument("--config")
        .metavar("PATH")
        .help("Session JSON that seeds the embedded configuration. The default is none.");
    bundle.add_argument("--compression-level")
        .default_value(std::string{"3"})
        .nargs(1)
        .metavar("N")
        .help("zstd compression level. The default is 3.");
    bundle.add_argument("--bundle-threads")
        .default_value(std::string{"0"})
        .nargs(1)
        .metavar("N")
        .help("zstd worker threads. 0 uses one thread. The default is 0.");
    AddSources(bundle, sourceHelp);

    argparse::ArgumentParser formats("formats", "", argparse::default_arguments::none, false);
    formats.add_description("List parsers and regex templates. No log source is read.");
    formats.add_epilog(std::string(ExitCodeHelpEpilog()));
    AddGlobalFlags(formats, formatsVerbose);
    AddHelpFlag(formats);
    formats.add_argument("--show").metavar("SLUG").help(
        "Print one template as JSON and exit. An unknown slug exits 2 and lists close matches."
    );
    AddSwitch(
        formats,
        "--user",
        "Also list user templates and which built-in each one shadows. The default is built-ins only."
    );
    formats.add_argument("--output-format")
        .default_value(std::string{"text"})
        .nargs(1)
        .choices("text", "json")
        .help("Rendering. One of text or json. The default is text.");

    argparse::ArgumentParser tail("tail", "", argparse::default_arguments::none, false);
    tail.add_description("Follow a file, stdin, or a tcp:// or udp:// listener and emit rows as they arrive.");
    tail.add_epilog(std::string(ExitCodeHelpEpilog()));
    AddGlobalFlags(tail, tailVerbose);
    AddParserFlags(tail);
    AddFilterFlags(tail, false);
    AddAdvancedHelpFlag(tail);
    AddHelpFlag(tail);
    tail.add_argument("--columns")
        .metavar("A,B,C")
        .help("Columns to emit. The default is the visible set from the configuration.");
    tail.add_argument("--output-format")
        .default_value(std::string{"auto"})
        .nargs(1)
        .choices("auto", "jsonl", "text")
        .help("Row rendering. auto uses text when stdout is a terminal and jsonl otherwise. The default is auto.");
    tail.add_argument("-o", "--output")
        .metavar("PATH")
        .help(
            "Append rows to this path. The file is not replaced atomically. The default is stdout, which is flushed "
            "every batch."
        );
    tail.add_argument("--max-rows")
        .metavar("N")
        .help("Stop after emitting N rows, with exit 0. The default is no row cap.");
    tail.add_argument("--for")
        .metavar("DURATION")
        .help(
            "Stop after this duration (30s, 5m, 1h), with exit 0. The default is to run until the source ends or a "
            "signal."
        );
    tail.add_argument("--retention")
        .metavar("N")
        .help(
            std::format(
                "Rows kept in memory. 0 is unbounded. Other values must be from {} to {}. The default is {}.",
                loglib::MIN_RETENTION_LINES,
                loglib::MAX_RETENTION_LINES,
                loglib::DEFAULT_RETENTION_LINES
            )
        );
    tail.add_argument("--max-error-rate")
        .metavar("PERCENT")
        .help("Not accepted on tail. Use --strict instead. The default is unset.");
    AddSwitch(tail, "--strict", "Exit 4 when the parse reported errors. The default is to keep going.");
    AddSources(
        tail, "File, .slvbundle, - for stdin, or tcp://[host]:port / udp://[host]:port. Repeat files to concatenate."
    );

    program.add_subparser(query);
    program.add_subparser(stats);
    program.add_subparser(schema);
    program.add_subparser(validate);
    program.add_subparser(bundle);
    program.add_subparser(formats);
    program.add_subparser(tail);

    UsageMap usage{
        .top = program.usage(),
        .query = query.usage(),
        .stats = stats.usage(),
        .schema = schema.usage(),
        .validate = validate.usage(),
        .bundle = bundle.usage(),
        .formats = formats.usage(),
        .tail = tail.usage(),
    };

    const std::string invoked = CommandToken(argc, argv);
    try
    {
        std::vector<std::string> args;
        args.reserve(static_cast<std::size_t>(argc));
        for (int index = 0; index < argc; ++index)
        {
            args.emplace_back(argv[index] == nullptr ? "" : argv[index]);
        }
        program.parse_args(args);
    }
    catch (const HelpRequest &help)
    {
        Options options;
        options.kind = ParseKind::Help;
        options.helpText = help.text;
        if (help.advanced)
        {
            options.helpText.append(ADVANCED_HELP);
        }
        return options;
    }
    catch (const VersionRequest &)
    {
        Options options;
        options.kind = ParseKind::Version;
        return options;
    }
    catch (const std::exception &ex)
    {
        return std::unexpected(UsageError{invoked, TranslateArgparse(ex.what()), usage.For(invoked)});
    }

    const argparse::ArgumentParser *selected = nullptr;
    Command command = Command::None;
    int commandVerbose = 0;
    if (program.is_subcommand_used("query"))
    {
        selected = &query;
        command = Command::Query;
        commandVerbose = queryVerbose;
    }
    else if (program.is_subcommand_used("stats"))
    {
        selected = &stats;
        command = Command::Stats;
        commandVerbose = statsVerbose;
    }
    else if (program.is_subcommand_used("schema"))
    {
        selected = &schema;
        command = Command::Schema;
        commandVerbose = schemaVerbose;
    }
    else if (program.is_subcommand_used("validate"))
    {
        selected = &validate;
        command = Command::Validate;
        commandVerbose = validateVerbose;
    }
    else if (program.is_subcommand_used("bundle"))
    {
        selected = &bundle;
        command = Command::Bundle;
        commandVerbose = bundleVerbose;
    }
    else if (program.is_subcommand_used("formats"))
    {
        selected = &formats;
        command = Command::Formats;
        commandVerbose = formatsVerbose;
    }
    else if (program.is_subcommand_used("tail"))
    {
        selected = &tail;
        command = Command::Tail;
        commandVerbose = tailVerbose;
    }

    if (selected == nullptr)
    {
        Options options;
        options.kind = ParseKind::MissingCommand;
        options.helpText = program.help().str();
        return options;
    }

    Options options;
    options.kind = ParseKind::Run;
    options.command = command;
    options.verbose = topVerbose + commandVerbose;
    options.quiet = FlagEither(program, *selected, "--quiet");
    options.color = ParseColor(PickString(program, *selected, "--color"));
    ApplyColor(options);
    options.timeZone = PickString(program, *selected, "--tz");
    options.noRotationHistory = FlagEither(program, *selected, "--no-rotation-history");

    const bool templateFlag = selected->is_used("--template-dir") || program.is_used("--template-dir");
    std::string templateValue;
    if (selected->is_used("--template-dir"))
    {
        templateValue = selected->get<std::string>("--template-dir");
    }
    else if (program.is_used("--template-dir"))
    {
        templateValue = program.get<std::string>("--template-dir");
    }
    ApplyTemplateDir(options, templateFlag, std::move(templateValue));

    const std::string &commandUsage = usage.For(CommandName(command));
    const auto threads = ParseUint(PickString(program, *selected, "--threads"), "--threads");
    if (!threads)
    {
        return std::unexpected(UsageError{CommandName(command), threads.error(), commandUsage});
    }
    if (*threads > std::numeric_limits<std::uint32_t>::max())
    {
        return std::unexpected(UsageError{CommandName(command), "'--threads' does not fit in 32 bits.", commandUsage});
    }
    options.threads = static_cast<std::uint32_t>(*threads);

    const bool parses = command != Command::Formats;
    if (parses)
    {
        options.format = ParseInputFormat(selected->get<std::string>("--format"));
        options.regex = OptionalString(*selected, "--regex");
        options.regexTemplate = OptionalString(*selected, "--regex-template");
        options.noMultilineLogfmt = Flag(*selected, "--no-multiline-logfmt");
        if (selected->is_used("--batch-size"))
        {
            const auto size = ParseSizeSuffix(selected->get<std::string>("--batch-size"));
            if (!size)
            {
                return std::unexpected(UsageError{CommandName(command), size.error(), commandUsage});
            }
            options.batchSizeBytes = *size;
        }
        if (selected->is_used("source"))
        {
            for (const std::string &token : selected->get<std::vector<std::string>>("source"))
            {
                auto classified = ClassifySource(token);
                if (!classified)
                {
                    return std::unexpected(UsageError{CommandName(command), classified.error(), commandUsage});
                }
                options.sources.push_back(std::move(*classified));
            }
        }
    }

    const bool filters = command == Command::Query || command == Command::Stats || command == Command::Bundle ||
                         command == Command::Tail;
    if (filters)
    {
        options.filters = Repeated(*selected, "--filter");
        options.since = OptionalString(*selected, "--since");
        if (command != Command::Tail)
        {
            options.until = OptionalString(*selected, "--until");
        }
        options.minLevel = OptionalString(*selected, "--min-level");
        options.invert = Flag(*selected, "--invert");
    }

    if (command == Command::Query)
    {
        options.sort = OptionalString(*selected, "--sort");
        options.columns = OptionalString(*selected, "--columns");
        options.allColumns = Flag(*selected, "--all-columns");
        if (selected->is_used("--limit"))
        {
            auto value = RequireUint(*selected, "--limit", "query", commandUsage);
            if (!value)
            {
                return std::unexpected(std::move(value.error()));
            }
            options.limit = *value;
        }
        if (selected->is_used("--offset"))
        {
            auto value = RequireUint(*selected, "--offset", "query", commandUsage);
            if (!value)
            {
                return std::unexpected(std::move(value.error()));
            }
            options.offset = *value;
        }
        if (selected->is_used("--last"))
        {
            auto value = RequireUint(*selected, "--last", "query", commandUsage);
            if (!value)
            {
                return std::unexpected(std::move(value.error()));
            }
            options.last = *value;
        }
        options.output = selected->get<std::string>("--output");
        options.outputExplicit = selected->is_used("--output");
        options.outputFormat = selected->get<std::string>("--output-format");
        options.noHeader = Flag(*selected, "--no-header");
        options.jsonVisibleOnly = Flag(*selected, "--json-visible-only");
        options.force = Flag(*selected, "--force");
        options.count = Flag(*selected, "--count");
        options.allowEmpty = Flag(*selected, "--allow-empty");
        options.strict = Flag(*selected, "--strict");
        if (const auto path = OptionalString(*selected, "--config"))
        {
            options.config = std::filesystem::path(*path);
        }
        if (const auto path = OptionalString(*selected, "--save-session"))
        {
            options.saveSession = std::filesystem::path(*path);
        }
        if (const auto path = OptionalString(*selected, "--save-columns"))
        {
            options.saveColumns = std::filesystem::path(*path);
        }
        if (selected->is_used("--max-error-rate"))
        {
            options.maxErrorRate = 0;
        }
    }
    else if (command == Command::Stats)
    {
        options.statsBy = selected->get<std::string>("--by");
        options.bucket = selected->get<std::string>("--bucket");
        options.outputFormat = selected->get<std::string>("--output-format");
        options.column = OptionalString(*selected, "--column");
        if (selected->is_used("--max-buckets"))
        {
            auto value = RequireUint(*selected, "--max-buckets", "stats", commandUsage);
            if (!value)
            {
                return std::unexpected(std::move(value.error()));
            }
            options.maxBuckets = *value;
        }
        auto top = RequireUint(*selected, "--top", "stats", commandUsage);
        if (!top)
        {
            return std::unexpected(std::move(top.error()));
        }
        options.top = *top;
    }
    else if (command == Command::Schema)
    {
        options.outputFormat = selected->get<std::string>("--output-format");
        auto sample = RequireUint(*selected, "--sample", "schema", commandUsage);
        if (!sample)
        {
            return std::unexpected(std::move(sample.error()));
        }
        options.sample = *sample;
    }
    else if (command == Command::Validate)
    {
        options.outputFormat = selected->get<std::string>("--output-format");
        options.strict = Flag(*selected, "--strict");
        auto maxErrors = RequireUint(*selected, "--max-errors", "validate", commandUsage);
        if (!maxErrors)
        {
            return std::unexpected(std::move(maxErrors.error()));
        }
        options.maxErrors = *maxErrors;
        if (selected->is_used("--max-error-rate"))
        {
            const std::string text = selected->get<std::string>("--max-error-rate");
            double rate = 0;
            const std::from_chars_result parsed = std::from_chars(text.data(), text.data() + text.size(), rate);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
            {
                return std::unexpected(
                    UsageError{
                        "validate",
                        std::format("'--max-error-rate' requires a percentage, not '{}'.", text),
                        commandUsage
                    }
                );
            }
            options.maxErrorRate = rate;
        }
    }
    else if (command == Command::Bundle)
    {
        options.sort = OptionalString(*selected, "--sort");
        options.columns = OptionalString(*selected, "--columns");
        options.filteredRows = Flag(*selected, "--filtered-rows");
        if (const auto path = OptionalString(*selected, "--output"))
        {
            options.output = *path;
            options.outputExplicit = true;
        }
        if (const auto path = OptionalString(*selected, "--config"))
        {
            options.config = std::filesystem::path(*path);
        }
        const auto level = ParseInt(selected->get<std::string>("--compression-level"), "--compression-level");
        if (!level)
        {
            return std::unexpected(UsageError{"bundle", level.error(), commandUsage});
        }
        options.compressionLevel = *level;
        const auto workers = ParseUint(selected->get<std::string>("--bundle-threads"), "--bundle-threads");
        if (!workers)
        {
            return std::unexpected(UsageError{"bundle", workers.error(), commandUsage});
        }
        if (*workers > std::numeric_limits<std::uint32_t>::max())
        {
            return std::unexpected(UsageError{"bundle", "'--bundle-threads' does not fit in 32 bits.", commandUsage});
        }
        options.bundleThreads = static_cast<std::uint32_t>(*workers);
    }
    else if (command == Command::Formats)
    {
        options.showSlug = OptionalString(*selected, "--show");
        options.listUserTemplates = Flag(*selected, "--user");
        options.outputFormat = selected->get<std::string>("--output-format");
    }
    else if (command == Command::Tail)
    {
        options.columns = OptionalString(*selected, "--columns");
        options.outputFormat = selected->get<std::string>("--output-format");
        if (const auto path = OptionalString(*selected, "--output"))
        {
            options.output = *path;
            options.outputExplicit = true;
        }
        options.strict = Flag(*selected, "--strict");
        if (selected->is_used("--max-error-rate"))
        {
            options.maxErrorRate = 0;
        }
        if (selected->is_used("--max-rows"))
        {
            auto value = RequireUint(*selected, "--max-rows", "tail", commandUsage);
            if (!value)
            {
                return std::unexpected(std::move(value.error()));
            }
            options.maxRows = *value;
        }
        if (selected->is_used("--for"))
        {
            const auto duration = ParseDurationSuffix(selected->get<std::string>("--for"));
            if (!duration)
            {
                return std::unexpected(UsageError{"tail", duration.error(), commandUsage});
            }
            options.duration = *duration;
        }
        if (selected->is_used("--retention"))
        {
            auto value = RequireUint(*selected, "--retention", "tail", commandUsage);
            if (!value)
            {
                return std::unexpected(std::move(value.error()));
            }
            options.retention = *value;
        }
    }

    return ValidateSemantics(std::move(options), commandUsage);
}

} // namespace slp
