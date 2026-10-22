#include "options.hpp"

#include "diagnostics.hpp"
#include "text_util.hpp"
#include "version.hpp"

#include <loglib/line_source.hpp>
#include <loglib/session_bundle.hpp>
#include <loglib/version.hpp>

#include <argparse/argparse.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <format>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <type_traits>
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

constexpr std::string_view SOURCE_HELP = "Log file, .slvbundle, or - for stdin. Repeat to concatenate in the order "
                                         "given. tcp:// and udp:// are rejected here; use `slp tail`.";

/** @brief A flag given to a command that rejects it, with the sentence that points to the right one. */
struct MisplacedFlag
{
    std::string_view command;
    std::string_view flag;
    std::string_view message;
};

constexpr std::string_view MAX_ERROR_RATE_ELSEWHERE =
    "'--max-error-rate' is only valid for validate; use '--strict' on row-producing commands.";

constexpr std::array<MisplacedFlag, 3> MISPLACED_FLAGS = {{
    {.command = "query", .flag = "--max-error-rate", .message = MAX_ERROR_RATE_ELSEWHERE},
    {.command = "tail", .flag = "--max-error-rate", .message = MAX_ERROR_RATE_ELSEWHERE},
    {.command = "validate",
     .flag = "--strict",
     .message = "'--strict' is not valid for validate; use '--max-error-rate'."},
}};

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

/** @brief A flag value or combination the command rejects. Becomes a `UsageError`. */
struct InvalidFlags
{
    std::string message;
};

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
    const GlobalOptions defaults;
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
        .nargs(1)
        .choices("auto", "always", "never")
        .help(
            "Colorize human-readable text. One of auto, always, or never. The default is auto, which colors only "
            "when stderr is a terminal and NO_COLOR is unset."
        );
    parser.add_argument("--threads")
        .nargs(1)
        .metavar("N")
        .help(
            std::format(
                "Number of parser threads forwarded to the library. 0 selects the library default. The default is {}.",
                defaults.threads
            )
        );
    parser.add_argument("--tz").nargs(1).metavar("ZONE").help(
        std::format(
            "Time zone for naive timestamps and human-readable times. An IANA name, local, or UTC. The default is {}.",
            defaults.timeZone
        )
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

void AddInputFlags(argparse::ArgumentParser &parser, std::string sourceHelp)
{
    parser.add_argument("-f", "--format")
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
    AddAdvancedHelpFlag(parser);
    parser.add_argument("source").nargs(argparse::nargs_pattern::any).metavar("source").help(std::move(sourceHelp));
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

void AddConfigFlag(argparse::ArgumentParser &parser, std::string help)
{
    parser.add_argument("--config").metavar("PATH").help(std::move(help));
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

/**
 * @brief Parses a finite, unsigned decimal percentage.
 *
 * Floating-point `from_chars` is missing from Apple's libc++, so this
 * uses `strtod` and rejects what `from_chars` would: leading space,
 * a sign, `nan`, and `inf`.
 */
[[nodiscard]] std::optional<double> ParsePercent(const std::string &text)
{
    char *end = nullptr;
    errno = 0;
    const double rate = std::strtod(text.c_str(), &end);
    const bool wellFormed = !text.empty() &&
                            (std::isdigit(static_cast<unsigned char>(text.front())) != 0 || text.front() == '.') &&
                            errno == 0 && end == text.c_str() + text.size() && std::isfinite(rate);
    if (!wellFormed)
    {
        return std::nullopt;
    }
    return rate;
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

/** @brief The rejected flag name in an argparse "Unknown argument" message, without any `=value`. */
[[nodiscard]] std::optional<std::string_view> UnknownFlagName(std::string_view what) noexcept
{
    constexpr std::string_view UNKNOWN = "Unknown argument: ";
    if (!what.starts_with(UNKNOWN))
    {
        return std::nullopt;
    }
    std::string_view name = what.substr(UNKNOWN.size());
    name = name.substr(0, name.find_first_of("=\r\n"));
    return name;
}

[[nodiscard]] std::string TranslateArgparse(std::string_view command, std::string_view what)
{
    if (const auto flag = UnknownFlagName(what))
    {
        for (const MisplacedFlag &misplaced : MISPLACED_FLAGS)
        {
            if (misplaced.command == command && misplaced.flag == *flag)
            {
                return std::string(misplaced.message);
            }
        }
    }

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
    constexpr std::string_view SCHEME = "://";
    const auto scheme = text.find(SCHEME);
    if (scheme == std::string_view::npos)
    {
        return std::unexpected(std::format("Network source '{}' is missing '://'.", text));
    }
    const std::string_view rest = text.substr(scheme + SCHEME.size());
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

/** @brief Typed reads of one parsed subcommand. Invalid values throw `InvalidFlags`. */
class FlagReader
{
public:
    explicit FlagReader(const argparse::ArgumentParser &parser) noexcept
        : mParser(parser)
    {
    }

    [[nodiscard]] bool Switch(std::string_view name) const
    {
        return mParser.is_used(name);
    }

    [[nodiscard]] std::optional<std::string> String(std::string_view name) const
    {
        if (!mParser.is_used(name))
        {
            return std::nullopt;
        }
        return mParser.get<std::string>(name);
    }

    [[nodiscard]] std::optional<std::filesystem::path> Path(std::string_view name) const
    {
        if (const auto text = String(name))
        {
            return std::filesystem::path(*text);
        }
        return std::nullopt;
    }

    [[nodiscard]] std::vector<std::string> Strings(std::string_view name) const
    {
        if (!mParser.is_used(name))
        {
            return {};
        }
        return mParser.get<std::vector<std::string>>(name);
    }

    [[nodiscard]] std::optional<std::uint64_t> Uint(std::string_view name) const
    {
        const auto text = String(name);
        if (!text)
        {
            return std::nullopt;
        }
        return Check(ParseUint(*text, name));
    }

    /** @brief Overwrites @p target only when @p name was given. */
    void ReadUint(std::string_view name, std::uint64_t &target) const
    {
        if (const auto value = Uint(name))
        {
            target = *value;
        }
    }

    void ReadUint32(std::string_view name, std::uint32_t &target) const
    {
        if (const auto value = Uint(name))
        {
            if (*value > std::numeric_limits<std::uint32_t>::max())
            {
                throw InvalidFlags{std::format("'{}' does not fit in 32 bits.", name)};
            }
            target = static_cast<std::uint32_t>(*value);
        }
    }

    void ReadInt(std::string_view name, int &target) const
    {
        if (const auto text = String(name))
        {
            target = Check(ParseInt(*text, name));
        }
    }

    template <typename T> static T Check(std::expected<T, std::string> value)
    {
        if (!value)
        {
            throw InvalidFlags{std::move(value.error())};
        }
        return std::move(*value);
    }

private:
    const argparse::ArgumentParser &mParser;
};

void Require(bool condition, std::string_view message)
{
    if (!condition)
    {
        throw InvalidFlags{std::string(message)};
    }
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

[[nodiscard]] ReportFormat ParseReportFormat(const FlagReader &flags, ReportFormat fallback)
{
    const auto text = flags.String("--output-format");
    if (!text)
    {
        return fallback;
    }
    if (*text == "json")
    {
        return ReportFormat::Json;
    }
    if (*text == "csv")
    {
        return ReportFormat::Csv;
    }
    return ReportFormat::Text;
}

/** @brief Reads sources and parser flags, and checks source combinations. */
[[nodiscard]] InputOptions ReadInput(const FlagReader &flags, bool networkAllowed)
{
    InputOptions input;
    if (const auto format = flags.String("--format"))
    {
        input.format = ParseInputFormat(*format);
    }
    input.regex = flags.String("--regex");
    input.regexTemplate = flags.String("--regex-template");
    input.noMultilineLogfmt = flags.Switch("--no-multiline-logfmt");
    if (const auto size = flags.String("--batch-size"))
    {
        input.batchSizeBytes = FlagReader::Check(ParseSizeSuffix(*size));
    }
    for (const std::string &token : flags.Strings("source"))
    {
        input.sources.push_back(FlagReader::Check(ClassifySource(token)));
    }

    int stdinCount = 0;
    bool network = false;
    for (const SourceSpec &source : input.sources)
    {
        if (source.kind == SourceKind::Stdin)
        {
            ++stdinCount;
        }
        if (source.kind == SourceKind::Tcp || source.kind == SourceKind::Udp)
        {
            network = true;
            Require(networkAllowed, std::format("Network source '{}' is only valid for `slp tail`.", source.text));
        }
    }
    Require(stdinCount <= 1, "Stdin '-' may appear only once.");
    Require(stdinCount == 0 || !network, "Stdin '-' cannot be combined with a network source.");

    const bool regexGiven = input.regex.has_value() || input.regexTemplate.has_value();
    Require(
        !(input.regex.has_value() && input.regexTemplate.has_value()),
        "'--regex' cannot be combined with '--regex-template'."
    );
    Require(
        !regexGiven || input.format == InputFormat::Auto || input.format == InputFormat::Regex,
        "'--regex' and '--regex-template' imply --format regex and cannot be combined with another format."
    );
    if (regexGiven)
    {
        input.format = InputFormat::Regex;
    }
    return input;
}

[[nodiscard]] FilterOptions ReadFilter(const FlagReader &flags, bool includeUntil)
{
    FilterOptions filter;
    filter.filters = flags.Strings("--filter");
    filter.since = flags.String("--since");
    if (includeUntil)
    {
        filter.until = flags.String("--until");
    }
    filter.minLevel = flags.String("--min-level");
    filter.invert = flags.Switch("--invert");
    return filter;
}

// ---- query ----------------------------------------------------------------

void AddQueryFlags(argparse::ArgumentParser &parser)
{
    AddInputFlags(parser, std::string(SOURCE_HELP));
    AddFilterFlags(parser, true);
    parser.add_argument("--sort")
        .metavar("COLUMN[:asc|desc]")
        .help("Sort by one column. The direction defaults to asc. Without --sort, rows stay in source order.");
    parser.add_argument("--columns")
        .metavar("A,B,C")
        .help("Comma-separated columns to emit, in that order. The default is the visible set from the configuration.");
    AddSwitch(
        parser, "--all-columns", "Emit every column. Overrides --columns and the configuration. The default is off."
    );
    parser.add_argument("--limit").metavar("N").help(
        "Emit at most N rows after filtering and sorting. The default is no cap."
    );
    auto &slice = parser.add_mutually_exclusive_group();
    slice.add_argument("--offset")
        .metavar("N")
        .help("Skip the first N result rows. Mutually exclusive with --last. The default is to skip none.");
    slice.add_argument("--last").metavar("N").help(
        "Emit only the last N result rows. Mutually exclusive with --offset. The default is the full result."
    );
    parser.add_argument("-o", "--output")
        .nargs(1)
        .metavar("PATH")
        .help("Write rows to this path, or - for stdout. The default is -.");
    parser.add_argument("-t", "--output-format")
        .nargs(1)
        .choices("jsonl", "csv", "markdown", "snapshot")
        .help("Export format. One of jsonl, csv, markdown, or snapshot. The default is jsonl.");
    AddSwitch(parser, "--no-header", "Omit the header row for csv and markdown. The default is to include it.");
    AddSwitch(
        parser, "--json-visible-only", "Honor --columns in JSON Lines. The default is to emit every field of each row."
    );
    AddSwitch(parser, "--force", "Overwrite an existing --output file. The default is to refuse and exit 5.");
    AddSwitch(
        parser,
        "--count",
        "Print the matching row count and no rows. Ignores --output-format, --columns, --limit, and --offset. The "
        "default is to print rows."
    );
    AddSwitch(parser, "--allow-empty", "Exit 0 when no row matches. The default is to exit 1 on an empty result.");
    AddSwitch(
        parser, "--strict", "Exit 4 when the parse reported errors. The default is to exit 0 or 1 and keep the rows."
    );
    AddConfigFlag(parser, "Session or columns JSON to apply before flags. The default is none.");
    parser.add_argument("--save-session")
        .metavar("PATH")
        .help(
            "Write the effective full session JSON after the run. Does not change stdout. The default is not to write."
        );
    parser.add_argument("--save-columns")
        .metavar("PATH")
        .help("Write columns and highlight rules only. Does not change stdout. The default is not to write.");
}

[[nodiscard]] CommandOptions ReadQuery(const FlagReader &flags)
{
    QueryOptions query;
    query.input = ReadInput(flags, false);
    query.input.config = flags.Path("--config");
    query.filter = ReadFilter(flags, true);
    query.view.sort = flags.String("--sort");
    query.view.columns = flags.String("--columns");
    query.view.allColumns = flags.Switch("--all-columns");
    query.save.session = flags.Path("--save-session");
    query.save.columns = flags.Path("--save-columns");
    query.limit = flags.Uint("--limit");
    query.offset = flags.Uint("--offset");
    query.last = flags.Uint("--last");
    if (const auto output = flags.String("--output"))
    {
        query.output = *output;
    }
    if (const auto format = flags.String("--output-format"))
    {
        query.outputFormat = *format == "csv"        ? QueryFormat::Csv
                             : *format == "markdown" ? QueryFormat::Markdown
                             : *format == "snapshot" ? QueryFormat::Snapshot
                                                     : QueryFormat::Jsonl;
    }
    query.noHeader = flags.Switch("--no-header");
    query.jsonVisibleOnly = flags.Switch("--json-visible-only");
    query.force = flags.Switch("--force");
    query.count = flags.Switch("--count");
    query.allowEmpty = flags.Switch("--allow-empty");
    query.strict = flags.Switch("--strict");
    return query;
}

// ---- stats ----------------------------------------------------------------

void AddStatsFlags(argparse::ArgumentParser &parser)
{
    const StatsOptions defaults;
    AddInputFlags(parser, std::string(SOURCE_HELP));
    AddFilterFlags(parser, true);
    parser.add_argument("--by")
        .nargs(1)
        .choices("level", "time", "column")
        .help("Summary axis. One of level, time, or column. The default is level.");
    parser.add_argument("--bucket")
        .nargs(1)
        .choices("1s", "10s", "1m", "10m", "1h", "1d", "auto")
        .help(std::format("Histogram width for --by time. The default is {}.", defaults.bucket));
    parser.add_argument("--max-buckets")
        .metavar("N")
        .help("Override the automatic bucket budget. The default is 60 for text and 500 for json and csv.");
    parser.add_argument("--top").nargs(1).metavar("N").help(
        std::format("How many distinct values --by column prints. The default is {}.", defaults.top)
    );
    parser.add_argument("--column")
        .metavar("NAME")
        .help("Column to summarise for --by column. Required with that axis.");
    parser.add_argument("--output-format")
        .nargs(1)
        .choices("text", "json", "csv")
        .help("Rendering. One of text, json, or csv. The default is text.");
}

[[nodiscard]] CommandOptions ReadStats(const FlagReader &flags)
{
    StatsOptions stats;
    stats.input = ReadInput(flags, false);
    stats.filter = ReadFilter(flags, true);
    if (const auto by = flags.String("--by"))
    {
        stats.by = *by == "time" ? StatsAxis::Time : *by == "column" ? StatsAxis::Column : StatsAxis::Level;
    }
    if (const auto bucket = flags.String("--bucket"))
    {
        stats.bucket = *bucket;
    }
    stats.maxBuckets = flags.Uint("--max-buckets");
    flags.ReadUint("--top", stats.top);
    stats.column = flags.String("--column");
    stats.outputFormat = ParseReportFormat(flags, stats.outputFormat);
    Require(stats.by != StatsAxis::Column || stats.column.has_value(), "'--by column' requires '--column'.");
    return stats;
}

// ---- schema ---------------------------------------------------------------

void AddSchemaFlags(argparse::ArgumentParser &parser)
{
    const SchemaOptions defaults;
    AddInputFlags(parser, std::string(SOURCE_HELP));
    parser.add_argument("--sample")
        .nargs(1)
        .metavar("N")
        .help(
            std::format("Sample values to print per column. 0 disables sampling. The default is {}.", defaults.sample)
        );
    parser.add_argument("--output-format")
        .nargs(1)
        .choices("text", "json")
        .help("Rendering. One of text or json. The default is text.");
}

[[nodiscard]] CommandOptions ReadSchema(const FlagReader &flags)
{
    SchemaOptions schema;
    schema.input = ReadInput(flags, false);
    flags.ReadUint("--sample", schema.sample);
    schema.outputFormat = ParseReportFormat(flags, schema.outputFormat);
    return schema;
}

// ---- validate -------------------------------------------------------------

void AddValidateFlags(argparse::ArgumentParser &parser)
{
    const ValidateOptions defaults;
    AddInputFlags(parser, std::string(SOURCE_HELP));
    parser.add_argument("--max-errors")
        .nargs(1)
        .metavar("N")
        .help(
            std::format(
                "How many individual parse errors to print in text mode. The default is {}.", defaults.maxErrors
            )
        );
    parser.add_argument("--max-error-rate")
        .metavar("PERCENT")
        .help(
            "Exit 0 while the parse-error percentage stays at or below this value. The default is any error exits 4."
        );
    parser.add_argument("--output-format")
        .nargs(1)
        .choices("text", "json")
        .help("Rendering. One of text or json. The default is text.");
}

[[nodiscard]] CommandOptions ReadValidate(const FlagReader &flags)
{
    ValidateOptions validate;
    validate.input = ReadInput(flags, false);
    flags.ReadUint("--max-errors", validate.maxErrors);
    if (const auto text = flags.String("--max-error-rate"))
    {
        const auto rate = ParsePercent(*text);
        Require(rate.has_value(), std::format("'--max-error-rate' requires a percentage, not '{}'.", *text));
        Require(
            *rate >= MIN_ERROR_RATE && *rate <= MAX_ERROR_RATE, "'--max-error-rate' must be a percentage from 0 to 100."
        );
        validate.maxErrorRate = rate;
    }
    validate.outputFormat = ParseReportFormat(flags, validate.outputFormat);
    return validate;
}

// ---- bundle ---------------------------------------------------------------

void AddBundleFlags(argparse::ArgumentParser &parser)
{
    const BundleOptions defaults;
    AddInputFlags(parser, std::string(SOURCE_HELP));
    AddFilterFlags(parser, true);
    parser.add_argument("--sort")
        .metavar("COLUMN[:asc|desc]")
        .help("Sort stored in the bundle. The default is source order.");
    parser.add_argument("--columns")
        .metavar("A,B,C")
        .help("Column set stored in the bundle. The default is the configuration's visible columns.");
    parser.add_argument("-o", "--output")
        .metavar("PATH")
        .help("Destination path. A missing .slvbundle extension is appended. This flag is required.");
    AddSwitch(
        parser,
        "--filtered-rows",
        "Write only matching rows and clear the embedded filter. The default is to keep every row and store the filter."
    );
    AddConfigFlag(parser, "Session JSON that seeds the embedded configuration. The default is none.");
    parser.add_argument("--compression-level")
        .nargs(1)
        .metavar("N")
        .help(std::format("zstd compression level. The default is {}.", defaults.compressionLevel));
    parser.add_argument("--bundle-threads")
        .nargs(1)
        .metavar("N")
        .help(std::format("zstd worker threads. 0 uses one thread. The default is {}.", defaults.bundleThreads));
}

[[nodiscard]] CommandOptions ReadBundle(const FlagReader &flags)
{
    BundleOptions bundle;
    bundle.input = ReadInput(flags, false);
    bundle.input.config = flags.Path("--config");
    bundle.filter = ReadFilter(flags, true);
    bundle.view.sort = flags.String("--sort");
    bundle.view.columns = flags.String("--columns");
    const auto output = flags.String("--output");
    Require(output.has_value(), "'--output' is required for bundle.");
    Require(*output != "-", "'--output' must be a file path; a bundle cannot be written to stdout.");
    bundle.output = std::filesystem::path(*output);
    bundle.filteredRows = flags.Switch("--filtered-rows");
    flags.ReadInt("--compression-level", bundle.compressionLevel);
    flags.ReadUint32("--bundle-threads", bundle.bundleThreads);
    return bundle;
}

// ---- formats --------------------------------------------------------------

void AddFormatsFlags(argparse::ArgumentParser &parser)
{
    parser.add_argument("--show").metavar("SLUG").help(
        "Print one template as JSON and exit. An unknown slug exits 2 and lists close matches."
    );
    AddSwitch(
        parser, "--user", "Also list user templates and which built-in each one shadows. The default is built-ins only."
    );
    parser.add_argument("--output-format")
        .nargs(1)
        .choices("text", "json")
        .help("Rendering. One of text or json. The default is text.");
}

[[nodiscard]] CommandOptions ReadFormats(const FlagReader &flags)
{
    FormatsOptions formats;
    formats.showSlug = flags.String("--show");
    formats.listUserTemplates = flags.Switch("--user");
    formats.outputFormat = ParseReportFormat(flags, formats.outputFormat);
    return formats;
}

// ---- tail -----------------------------------------------------------------

void AddTailFlags(argparse::ArgumentParser &parser)
{
    AddInputFlags(parser, "One source to follow: a file, - for stdin, or tcp://[host]:port / udp://[host]:port.");
    AddFilterFlags(parser, false);
    parser.add_argument("--columns")
        .metavar("A,B,C")
        .help("Columns to emit. The default is the visible set from the configuration.");
    parser.add_argument("--output-format")
        .nargs(1)
        .choices("auto", "jsonl", "text")
        .help("Row rendering. auto uses text when stdout is a terminal and jsonl otherwise. The default is auto.");
    parser.add_argument("-o", "--output")
        .metavar("PATH")
        .help(
            "Append rows to this path. The file is not replaced atomically. The default is stdout, which is flushed "
            "every batch."
        );
    parser.add_argument("--max-rows")
        .metavar("N")
        .help("Stop after emitting N rows, with exit 0. The default is no row cap.");
    parser.add_argument("--for")
        .metavar("DURATION")
        .help(
            "Stop after this duration (30s, 5m, 1h), with exit 0. The default is to run until the source ends or a "
            "signal."
        );
    parser.add_argument("--retention")
        .metavar("N")
        .help(
            std::format(
                "Rows kept in memory. 0 is unbounded. Other values must be from {} to {}. The default is {}.",
                loglib::MIN_RETENTION_LINES,
                loglib::MAX_RETENTION_LINES,
                loglib::DEFAULT_RETENTION_LINES
            )
        );
    AddSwitch(parser, "--strict", "Exit 4 when the parse reported errors. The default is to keep going.");
}

[[nodiscard]] CommandOptions ReadTail(const FlagReader &flags)
{
    TailOptions tail;
    tail.input = ReadInput(flags, true);
    tail.filter = ReadFilter(flags, false);
    tail.view.columns = flags.String("--columns");
    if (const auto format = flags.String("--output-format"))
    {
        tail.outputFormat = *format == "jsonl"  ? TailFormat::Jsonl
                            : *format == "text" ? TailFormat::Text
                                                : TailFormat::Auto;
    }
    if (const auto output = flags.String("--output"); output && *output != "-")
    {
        tail.output = std::filesystem::path(*output);
    }
    tail.strict = flags.Switch("--strict");
    tail.maxRows = flags.Uint("--max-rows");
    if (const auto duration = flags.String("--for"))
    {
        tail.duration = FlagReader::Check(ParseDurationSuffix(*duration));
    }
    tail.retention = flags.Uint("--retention");
    Require(
        !tail.retention || *tail.retention == 0 ||
            (*tail.retention >= loglib::MIN_RETENTION_LINES && *tail.retention <= loglib::MAX_RETENTION_LINES),
        std::format(
            "'--retention' must be 0 (unbounded) or from {} to {}.",
            loglib::MIN_RETENTION_LINES,
            loglib::MAX_RETENTION_LINES
        )
    );
    return tail;
}

// ---- command table --------------------------------------------------------

/** @brief Flag registration and extraction for one subcommand. */
struct CommandDefinition
{
    std::string_view name;
    std::string_view description;
    void (*addFlags)(argparse::ArgumentParser &);
    CommandOptions (*read)(const FlagReader &);
};

constexpr std::array<CommandDefinition, 7> COMMANDS = {{
    {QueryOptions::NAME,
     "Filter, sort, and export rows. Sources concatenate in the order given.",
     AddQueryFlags,
     ReadQuery},
    {StatsOptions::NAME, "Summarise a source without emitting rows.", AddStatsFlags, ReadStats},
    {SchemaOptions::NAME, "Print the detected column schema without emitting rows.", AddSchemaFlags, ReadSchema},
    {ValidateOptions::NAME, "Report parse health without emitting rows.", AddValidateFlags, ReadValidate},
    {BundleOptions::NAME,
     "Write a .slvbundle of every retained row. Filters, sort, and columns are stored in the bundle for the GUI and "
     "are not applied destructively unless --filtered-rows is set.",
     AddBundleFlags,
     ReadBundle},
    {FormatsOptions::NAME, "List parsers and regex templates. No log source is read.", AddFormatsFlags, ReadFormats},
    {TailOptions::NAME,
     "Follow a file, stdin, or a tcp:// or udp:// listener and emit rows as they arrive.",
     AddTailFlags,
     ReadTail},
}};

/** @brief Name of the first argv word that is a subcommand, skipping global flag values. */
[[nodiscard]] std::string CommandToken(int argc, char **argv)
{
    static constexpr std::array<std::string_view, 10> VALUE_FLAGS = {
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
            if (equals == std::string_view::npos && std::ranges::find(VALUE_FLAGS, name) != VALUE_FLAGS.end())
            {
                ++index;
            }
            continue;
        }
        if (std::ranges::any_of(COMMANDS, [arg](const CommandDefinition &command) { return command.name == arg; }))
        {
            return std::string(arg);
        }
    }
    return {};
}

/** @brief Value of a global flag, preferring the subcommand's copy over the top-level one. */
[[nodiscard]] std::optional<std::string> GlobalString(
    const argparse::ArgumentParser &top, const argparse::ArgumentParser &command, std::string_view name
)
{
    if (command.is_used(name))
    {
        return command.get<std::string>(name);
    }
    if (top.is_used(name))
    {
        return top.get<std::string>(name);
    }
    return std::nullopt;
}

[[nodiscard]] GlobalOptions ReadGlobal(
    const argparse::ArgumentParser &top, const argparse::ArgumentParser &command, int verbose
)
{
    GlobalOptions global;
    global.verbose = verbose;
    global.quiet = top.is_used("--quiet") || command.is_used("--quiet");
    global.noRotationHistory = top.is_used("--no-rotation-history") || command.is_used("--no-rotation-history");
    if (const auto color = GlobalString(top, command, "--color"))
    {
        global.color = *color == "always" ? ColorMode::Always : *color == "never" ? ColorMode::Never : ColorMode::Auto;
    }
    if (EnvIsSet("NO_COLOR"))
    {
        global.color = ColorMode::Never;
    }
    if (const auto zone = GlobalString(top, command, "--tz"))
    {
        global.timeZone = *zone;
    }
    if (const auto threads = GlobalString(top, command, "--threads"))
    {
        const std::uint64_t value = FlagReader::Check(ParseUint(*threads, "--threads"));
        Require(value <= std::numeric_limits<std::uint32_t>::max(), "'--threads' does not fit in 32 bits.");
        global.threads = static_cast<std::uint32_t>(value);
    }
    if (const auto directory = GlobalString(top, command, "--template-dir"))
    {
        global.templateDir = std::filesystem::path(*directory);
    }
    else if (const auto env = EnvironmentString("SLV_TEMPLATE_DIR"))
    {
        global.templateDir = std::filesystem::path(*env);
    }
    return global;
}

} // namespace

bool FilterOptions::Active() const noexcept
{
    return !filters.empty() || since.has_value() || until.has_value() || minLevel.has_value() || invert;
}

bool FilterOptions::OnlyInvert() const noexcept
{
    return invert && filters.empty() && !since && !until && !minLevel;
}

std::string_view CommandName(const CommandOptions &command) noexcept
{
    return std::visit([](const auto &options) { return std::decay_t<decltype(options)>::NAME; }, command);
}

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
    constexpr std::uint64_t MILLIS_PER_SECOND = 1000;
    if (seconds > std::numeric_limits<std::uint64_t>::max() / MILLIS_PER_SECOND)
    {
        return std::unexpected(std::format("'{}' overflows a duration.", text));
    }
    return std::chrono::milliseconds(seconds * MILLIS_PER_SECOND);
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
    spec.kind = EqualsIgnoreCase(spec.path.extension().string(), loglib::SESSION_BUNDLE_EXTENSION) ? SourceKind::Bundle
                                                                                                   : SourceKind::Path;
    return spec;
}

std::string VersionLine()
{
    return std::format("slp {} ({}) loglib {}", VERSION, PRODUCT_NAME, loglib::Version());
}

std::expected<Options, UsageError> ParseArguments(int argc, char **argv)
{
    int topVerbose = 0;
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

    // Subparsers are held by reference inside `program`, and the help
    // and verbose actions capture them, so their addresses must stay put.
    std::array<std::unique_ptr<argparse::ArgumentParser>, COMMANDS.size()> parsers;
    std::array<int, COMMANDS.size()> verboseCounts{};
    for (std::size_t index = 0; index < COMMANDS.size(); ++index)
    {
        const CommandDefinition &definition = COMMANDS[index];
        parsers[index] = std::make_unique<argparse::ArgumentParser>(
            std::string(definition.name), "", argparse::default_arguments::none, false
        );
        argparse::ArgumentParser &parser = *parsers[index];
        parser.add_description(std::string(definition.description));
        parser.add_epilog(std::string(ExitCodeHelpEpilog()));
        AddGlobalFlags(parser, verboseCounts[index]);
        definition.addFlags(parser);
        AddHelpFlag(parser);
        program.add_subparser(parser);
    }

    auto usageFor = [&](std::string_view command) {
        for (std::size_t index = 0; index < COMMANDS.size(); ++index)
        {
            if (COMMANDS[index].name == command)
            {
                return parsers[index]->usage();
            }
        }
        return program.usage();
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
        return std::unexpected(UsageError{invoked, TranslateArgparse(invoked, ex.what()), usageFor(invoked)});
    }

    for (std::size_t index = 0; index < COMMANDS.size(); ++index)
    {
        const CommandDefinition &definition = COMMANDS[index];
        if (!program.is_subcommand_used(std::string(definition.name)))
        {
            continue;
        }
        const argparse::ArgumentParser &parser = *parsers[index];
        try
        {
            Options options;
            options.global = ReadGlobal(program, parser, topVerbose + verboseCounts[index]);
            options.command = definition.read(FlagReader(parser));
            return options;
        }
        catch (const InvalidFlags &invalid)
        {
            return std::unexpected(UsageError{std::string(definition.name), invalid.message, parser.usage()});
        }
    }

    Options options;
    options.kind = ParseKind::MissingCommand;
    options.helpText = program.help().str();
    return options;
}

} // namespace slp
