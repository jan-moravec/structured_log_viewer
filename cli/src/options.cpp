#include "options.hpp"

#include "diagnostics.hpp"
#include "text_util.hpp"
#include "version.hpp"

#include <loglib/line_source.hpp>
#include <loglib/session_bundle.hpp>
#include <loglib/user_paths.hpp>
#include <loglib/version.hpp>

#include <CLI/CLI.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <format>
#include <functional>
#include <limits>
#include <memory>
#include <ranges>
#include <string>
#include <type_traits>
#include <utility>

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

/** @brief Option group that keeps an option out of `--help`; `--help-advanced` lists it. */
constexpr std::string_view HIDDEN_GROUP = {};

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

/** @brief A flag combination the command rejects. Becomes a `UsageError`. */
struct InvalidFlags
{
    std::string message;
};

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

[[nodiscard]] std::expected<std::uint16_t, std::string> ParsePort(std::string_view portText, std::string_view source)
{
    const auto parsed = ParseUint(portText, "port");
    if (!parsed)
    {
        return std::unexpected(std::format("Network source '{}' has an invalid port.", source));
    }
    if (*parsed < static_cast<std::uint64_t>(MIN_TCP_PORT) || *parsed > static_cast<std::uint64_t>(MAX_TCP_PORT))
    {
        return std::unexpected(
            std::format(
                "Network source '{}' has port {}, which is outside {}-{}.", source, *parsed, MIN_TCP_PORT, MAX_TCP_PORT
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

void Require(bool condition, std::string_view message)
{
    if (!condition)
    {
        throw InvalidFlags{std::string(message)};
    }
}

/** @brief Value of @p value, or throws `InvalidFlags` with its error. */
template <typename T> T Check(std::expected<T, std::string> value)
{
    if (!value)
    {
        throw InvalidFlags{std::move(value.error())};
    }
    return std::move(*value);
}

/** @brief Adds an option whose value, when given, is stored in @p target. */
template <typename T>
CLI::Option *AddOptional(CLI::App &app, std::string name, std::optional<T> &target, std::string help)
{
    return app.add_option_function<T>(std::move(name), [&target](const T &value) { target = value; }, std::move(help));
}

/**
 * @brief Adds an option that maps fixed tokens to a value.
 *
 * Only the listed tokens are accepted, ignoring case, and help shows them as `a|b|c`.
 */
template <typename T>
CLI::Option *AddChoice(
    CLI::App &app, std::string name, T &target, std::vector<std::pair<std::string, T>> choices, std::string help
)
{
    std::vector<std::string> tokens;
    std::string typeName;
    for (const auto &choice : choices)
    {
        tokens.push_back(choice.first);
        typeName += (typeName.empty() ? "" : "|") + choice.first;
    }
    return app
        .add_option_function<std::string>(
            std::move(name),
            [&target, choices = std::move(choices)](const std::string &token) {
                const auto match = std::ranges::find_if(choices, [&token](const auto &choice) {
                    return EqualsIgnoreCase(choice.first, token);
                });
                if (match != choices.end())
                {
                    target = match->second;
                }
            },
            std::move(help)
        )
        ->check(CLI::IsMember(std::move(tokens), CLI::ignore_case))
        ->type_name(std::move(typeName));
}

// ---- shared flag groups -----------------------------------------------------

void AddGlobalFlags(CLI::App &app, GlobalOptions &global)
{
    app.add_flag(
        "-q,--quiet",
        global.quiet,
        "Suppress warnings and progress. Errors still print. The default is to print warnings."
    );
    app.add_flag(
        "-v,--verbose",
        global.verbose,
        "Print per-stage timing and row counts to stderr. Repeat for parser-level detail. The default is off."
    );
    AddChoice(
        app,
        "--color",
        global.color,
        {{"auto", ColorMode::Auto}, {"always", ColorMode::Always}, {"never", ColorMode::Never}},
        "Colorize human-readable text. One of auto, always, or never. The default is auto, which colors only when "
        "stderr is a terminal and NO_COLOR is unset."
    );
    app.add_option(
        "--threads",
        global.threads,
        std::format(
            "Number of parser threads forwarded to the library. 0 selects the library default. The default is {}.",
            global.threads
        )
    );
    app.add_option(
        "--tz",
        global.timeZone,
        std::format(
            "Time zone for human-readable times and for --since and --until values without a zone. An IANA name, "
            "local, or UTC. Log timestamps without a zone are read as UTC. The default is {}.",
            global.timeZone
        )
    );
    app.add_flag(
        "--no-rotation-history",
        global.noRotationHistory,
        "Skip rotated-sibling discovery. The default is to load numbered and dated siblings, oldest first."
    );
    AddOptional(
        app,
        "--template-dir",
        global.templateDir,
        "Directory of user regex templates. Overrides SLV_TEMPLATE_DIR and the default user directory. The default "
        "is the platform user-template directory."
    );
}

/** @brief Raw source tokens and the `--help-advanced` switch, read after parsing. */
struct InputTokens
{
    std::vector<std::string> sources;
    bool helpAdvanced = false;
};

void AddInputFlags(CLI::App &app, InputOptions &input, InputTokens &tokens, std::string sourceHelp)
{
    AddChoice(
        app,
        "-f,--format",
        input.format,
        {{"auto", InputFormat::Auto},
         {"json", InputFormat::Json},
         {"logfmt", InputFormat::Logfmt},
         {"csv", InputFormat::Csv},
         {"regex", InputFormat::Regex}},
        "Parser to use. One of auto, json, logfmt, csv, or regex. The default is auto. --regex and --regex-template "
        "imply regex."
    );
    AddOptional(
        app,
        "--regex",
        input.regex,
        "PCRE2 pattern with named captures. Implies --format regex. Mutually exclusive with --regex-template."
    );
    AddOptional(
        app,
        "--regex-template",
        input.regexTemplate,
        "Built-in or user regex template slug, as printed by `slp formats`. Implies --format regex. Mutually "
        "exclusive with --regex."
    );
    app.add_flag(
        "--no-multiline-logfmt",
        input.noMultilineLogfmt,
        "Parse each logfmt line on its own. The default is to join continuations."
    );
    app.add_option_function<std::string>(
           "--batch-size",
           [&input](const std::string &text) {
               auto size = ParseSizeSuffix(text);
               if (!size)
               {
                   throw CLI::ValidationError("--batch-size", size.error());
               }
               input.batchSizeBytes = *size;
           },
           "Parser batch size, for example 1MiB. The default is the library batch size."
    )
        ->group(std::string(HIDDEN_GROUP));
    app.add_flag("--help-advanced", tokens.helpAdvanced, "Show flags omitted from the default help and exit 0.");
    app.add_option("source", tokens.sources, std::move(sourceHelp));
}

void AddFilterFlags(CLI::App &app, FilterOptions &filter, bool includeUntil)
{
    app.add_option(
           "--filter",
           filter.filters,
           "Filter query in the GUI syntax. Repeat to combine with AND in the order given. The default is to keep "
           "every row."
    )
        ->allow_extra_args(false);
    AddOptional(
        app,
        "--since",
        filter.since,
        "Keep rows at or after this time on the first time column. Accepts the column's formats, ISO-8601, now, and "
        "relative values such as -30m. A value without Z or an offset is read in --tz. The default is no lower bound."
    );
    if (includeUntil)
    {
        AddOptional(
            app,
            "--until",
            filter.until,
            "Keep rows at or before this time on the first time column. The same forms as --since are accepted. The "
            "default is no upper bound."
        );
    }
    AddOptional(
        app,
        "--min-level",
        filter.minLevel,
        "Keep rows at or above this canonical level. One of trace, debug, info, warn, error, or fatal. The default "
        "is to keep every level."
    )
        ->check(CLI::IsMember({"trace", "debug", "info", "warn", "error", "fatal"}));
    app.add_flag(
        "-x,--invert", filter.invert, "Keep rows the filter rejects. The default is to keep rows the filter accepts."
    );
}

void AddReportFormat(CLI::App &app, ReportFormat &target, bool allowCsv)
{
    std::vector<std::pair<std::string, ReportFormat>> choices = {
        {"text", ReportFormat::Text}, {"json", ReportFormat::Json}
    };
    if (allowCsv)
    {
        choices.emplace_back("csv", ReportFormat::Csv);
    }
    AddChoice(
        app,
        "--output-format",
        target,
        std::move(choices),
        allowCsv ? "Rendering. One of text, json, or csv. The default is text."
                 : "Rendering. One of text or json. The default is text."
    );
}

/** @brief Classifies @p tokens into `input.sources` and checks source combinations. */
void ResolveSources(InputOptions &input, const std::vector<std::string> &tokens, bool networkAllowed)
{
    for (const std::string &token : tokens)
    {
        input.sources.push_back(Check(ClassifySource(token)));
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
}

// ---- commands ---------------------------------------------------------------

/**
 * @brief One subcommand: its parser, its options, and the checks that run after parsing.
 *
 * `finish` classifies sources and validates combinations; it throws
 * `InvalidFlags`.
 */
struct CommandParser
{
    CLI::App *app = nullptr;
    CommandOptions options;
    InputTokens tokens;
    std::function<void(CommandParser &)> finish;
};

void AddQuery(CLI::App &root, CommandParser &command)
{
    command.options = QueryOptions{};
    auto &query = std::get<QueryOptions>(command.options);
    CLI::App &app = *root.add_subcommand(
        std::string(QueryOptions::NAME), "Filter, sort, and export rows. Sources concatenate in the order given."
    );
    command.app = &app;
    AddInputFlags(app, query.input, command.tokens, std::string(SOURCE_HELP));
    AddFilterFlags(app, query.filter, true);
    AddOptional(
        app,
        "--sort",
        query.view.sort,
        "Sort by one column. The direction defaults to asc. Without --sort, rows stay in source order."
    )
        ->type_name("COLUMN[:asc|desc]");
    AddOptional(
        app,
        "--columns",
        query.view.columns,
        "Comma-separated columns to emit, in that order. The default is the visible set from the configuration."
    )
        ->type_name("A,B,C");
    app.add_flag(
        "--all-columns",
        query.view.allColumns,
        "Emit every column. Overrides --columns and the configuration. The default is off."
    );
    AddOptional(app, "--limit", query.limit, "Emit at most N rows after filtering and sorting. The default is no cap.");
    auto *offset = AddOptional(
        app,
        "--offset",
        query.offset,
        "Skip the first N result rows. Mutually exclusive with --last. The default is to skip none."
    );
    auto *last = AddOptional(
        app,
        "--last",
        query.last,
        "Emit only the last N result rows. Mutually exclusive with --offset. The default is the full result."
    );
    offset->excludes(last);
    app.add_option("-o,--output", query.output, "Write rows to this path, or - for stdout. The default is -.");
    AddChoice(
        app,
        "-t,--output-format",
        query.outputFormat,
        {{"jsonl", QueryFormat::Jsonl},
         {"csv", QueryFormat::Csv},
         {"markdown", QueryFormat::Markdown},
         {"snapshot", QueryFormat::Snapshot}},
        "Export format. One of jsonl, csv, markdown, or snapshot. The default is jsonl."
    );
    app.add_flag(
        "--no-header", query.noHeader, "Omit the header row for csv and markdown. The default is to include it."
    );
    app.add_flag(
        "--json-visible-only",
        query.jsonVisibleOnly,
        "Honor --columns in JSON Lines. The default is to emit every field of each row."
    );
    app.add_flag("--force", query.force, "Overwrite an existing --output file. The default is to refuse and exit 5.");
    app.add_flag(
        "--count",
        query.count,
        "Print the matching row count and no rows. Ignores --output-format, --columns, --limit, and --offset. The "
        "default is to print rows."
    );
    app.add_flag(
        "--allow-empty", query.allowEmpty, "Exit 0 when no row matches. The default is to exit 1 on an empty result."
    );
    app.add_flag(
        "--strict",
        query.strict,
        "Exit 4 when the parse reported errors. The default is to exit 0 or 1 and keep the rows."
    );
    AddOptional(
        app, "--config", query.input.config, "Session or columns JSON to apply before flags. The default is none."
    );
    AddOptional(
        app,
        "--save-session",
        query.save.session,
        "Write the effective full session JSON after the run. Does not change stdout. The default is not to write."
    );
    AddOptional(
        app,
        "--save-columns",
        query.save.columns,
        "Write columns and highlight rules only. Does not change stdout. The default is not to write."
    );
    command.finish = [](CommandParser &self) {
        auto &options = std::get<QueryOptions>(self.options);
        ResolveSources(options.input, self.tokens.sources, false);
    };
}

void AddStats(CLI::App &root, CommandParser &command)
{
    command.options = StatsOptions{};
    auto &stats = std::get<StatsOptions>(command.options);
    CLI::App &app = *root.add_subcommand(std::string(StatsOptions::NAME), "Summarise a source without emitting rows.");
    command.app = &app;
    AddInputFlags(app, stats.input, command.tokens, std::string(SOURCE_HELP));
    AddFilterFlags(app, stats.filter, true);
    AddChoice(
        app,
        "--by",
        stats.by,
        {{"level", StatsAxis::Level}, {"time", StatsAxis::Time}, {"column", StatsAxis::Column}},
        "Summary axis. One of level, time, or column. The default is level."
    );
    AddChoice(
        app,
        "--bucket",
        stats.bucket,
        {{"auto", std::nullopt},
         {"1s", loglib::HistogramBucketSize::OneSecond},
         {"10s", loglib::HistogramBucketSize::TenSeconds},
         {"1m", loglib::HistogramBucketSize::OneMinute},
         {"10m", loglib::HistogramBucketSize::TenMinutes},
         {"1h", loglib::HistogramBucketSize::OneHour},
         {"1d", loglib::HistogramBucketSize::OneDay}},
        "Histogram width for --by time. The default is auto."
    );
    AddOptional(
        app,
        "--max-buckets",
        stats.maxBuckets,
        "Override the automatic bucket budget. The default is 60 for text and 500 for json and csv."
    );
    app.add_option(
        "--top", stats.top, std::format("How many distinct values --by column prints. The default is {}.", stats.top)
    );
    AddOptional(app, "--column", stats.column, "Column to summarise for --by column. Required with that axis.");
    AddReportFormat(app, stats.outputFormat, true);
    command.finish = [](CommandParser &self) {
        auto &options = std::get<StatsOptions>(self.options);
        ResolveSources(options.input, self.tokens.sources, false);
        Require(options.by != StatsAxis::Column || options.column.has_value(), "'--by column' requires '--column'.");
    };
}

void AddSchema(CLI::App &root, CommandParser &command)
{
    command.options = SchemaOptions{};
    auto &schema = std::get<SchemaOptions>(command.options);
    CLI::App &app = *root.add_subcommand(
        std::string(SchemaOptions::NAME), "Print the detected column schema without emitting rows."
    );
    command.app = &app;
    AddInputFlags(app, schema.input, command.tokens, std::string(SOURCE_HELP));
    app.add_option(
        "--sample",
        schema.sample,
        std::format("Sample values to print per column. 0 disables sampling. The default is {}.", schema.sample)
    );
    AddReportFormat(app, schema.outputFormat, false);
    command.finish = [](CommandParser &self) {
        ResolveSources(std::get<SchemaOptions>(self.options).input, self.tokens.sources, false);
    };
}

void AddValidate(CLI::App &root, CommandParser &command)
{
    command.options = ValidateOptions{};
    auto &validate = std::get<ValidateOptions>(command.options);
    CLI::App &app =
        *root.add_subcommand(std::string(ValidateOptions::NAME), "Report parse health without emitting rows.");
    command.app = &app;
    AddInputFlags(app, validate.input, command.tokens, std::string(SOURCE_HELP));
    app.add_option(
        "--max-errors",
        validate.maxErrors,
        std::format("How many individual parse errors to print in text mode. The default is {}.", validate.maxErrors)
    );
    app.add_option_function<std::string>(
        "--max-error-rate",
        [&validate](const std::string &text) {
            const auto rate = ParsePercent(text);
            if (!rate)
            {
                throw CLI::ValidationError("--max-error-rate", std::format("requires a percentage, not '{}'.", text));
            }
            validate.maxErrorRate = rate;
        },
        "Exit 0 while the parse-error percentage stays at or below this value. The default is any error exits 4."
    );
    AddReportFormat(app, validate.outputFormat, false);
    command.finish = [](CommandParser &self) {
        auto &options = std::get<ValidateOptions>(self.options);
        ResolveSources(options.input, self.tokens.sources, false);
        Require(
            !options.maxErrorRate ||
                (*options.maxErrorRate >= MIN_ERROR_RATE && *options.maxErrorRate <= MAX_ERROR_RATE),
            "'--max-error-rate' must be a percentage from 0 to 100."
        );
    };
}

void AddBundle(CLI::App &root, CommandParser &command)
{
    command.options = BundleOptions{};
    auto &bundle = std::get<BundleOptions>(command.options);
    CLI::App &app = *root.add_subcommand(
        std::string(BundleOptions::NAME),
        "Write a .slvbundle of every retained row. Filters, sort, and columns are stored in the bundle for the GUI and "
        "are not applied destructively unless --filtered-rows is set."
    );
    command.app = &app;
    AddInputFlags(app, bundle.input, command.tokens, std::string(SOURCE_HELP));
    AddFilterFlags(app, bundle.filter, true);
    AddOptional(app, "--sort", bundle.view.sort, "Sort stored in the bundle. The default is source order.")
        ->type_name("COLUMN[:asc|desc]");
    AddOptional(
        app,
        "--columns",
        bundle.view.columns,
        "Column set stored in the bundle. The default is the configuration's visible columns."
    )
        ->type_name("A,B,C");
    app.add_option(
        "-o,--output",
        bundle.output,
        "Destination path. A missing .slvbundle extension is appended. This flag is required."
    );
    app.add_flag(
        "--filtered-rows",
        bundle.filteredRows,
        "Write only matching rows and clear the embedded filter. The default is to keep every row and store the filter."
    );
    AddOptional(
        app, "--config", bundle.input.config, "Session JSON that seeds the embedded configuration. The default is none."
    );
    app.add_option(
        "--compression-level",
        bundle.compressionLevel,
        std::format("zstd compression level. The default is {}.", bundle.compressionLevel)
    );
    app.add_option(
        "--bundle-threads",
        bundle.bundleThreads,
        std::format("zstd worker threads. 0 uses one thread. The default is {}.", bundle.bundleThreads)
    );
    command.finish = [](CommandParser &self) {
        auto &options = std::get<BundleOptions>(self.options);
        ResolveSources(options.input, self.tokens.sources, false);
        Require(!options.output.empty(), "'--output' is required for bundle.");
        Require(options.output != "-", "'--output' must be a file path; a bundle cannot be written to stdout.");
    };
}

void AddFormats(CLI::App &root, CommandParser &command)
{
    command.options = FormatsOptions{};
    auto &formats = std::get<FormatsOptions>(command.options);
    CLI::App &app = *root.add_subcommand(
        std::string(FormatsOptions::NAME), "List parsers and regex templates. No log source is read."
    );
    command.app = &app;
    AddOptional(
        app,
        "--show",
        formats.showSlug,
        "Print one template as JSON and exit. An unknown slug exits 2 and lists close matches."
    );
    app.add_flag(
        "--user",
        formats.listUserTemplates,
        "Also list user templates and which built-in each one shadows. The default is built-ins only."
    );
    AddReportFormat(app, formats.outputFormat, false);
    command.finish = [](CommandParser &) {};
}

void AddTail(CLI::App &root, CommandParser &command)
{
    command.options = TailOptions{};
    auto &tail = std::get<TailOptions>(command.options);
    CLI::App &app = *root.add_subcommand(
        std::string(TailOptions::NAME),
        "Follow a file, stdin, or a tcp:// or udp:// listener and emit rows as they arrive."
    );
    command.app = &app;
    AddInputFlags(
        app,
        tail.input,
        command.tokens,
        "One source to follow: a file, - for stdin, or tcp://[host]:port / udp://[host]:port."
    );
    AddFilterFlags(app, tail.filter, false);
    AddOptional(
        app, "--columns", tail.view.columns, "Columns to emit. The default is the visible set from the configuration."
    )
        ->type_name("A,B,C");
    AddChoice(
        app,
        "--output-format",
        tail.outputFormat,
        {{"auto", TailFormat::Auto}, {"jsonl", TailFormat::Jsonl}, {"text", TailFormat::Text}},
        "Row rendering. auto uses text when stdout is a terminal and jsonl otherwise. The default is auto."
    );
    app.add_option_function<std::string>(
        "-o,--output",
        [&tail](const std::string &path) {
            if (path != "-")
            {
                tail.output = std::filesystem::path(path);
            }
        },
        "Append rows to this path. The file is not replaced atomically. The default is stdout, which is flushed every "
        "batch."
    );
    AddOptional(app, "--max-rows", tail.maxRows, "Stop after emitting N rows, with exit 0. The default is no row cap.");
    app.add_option_function<std::string>(
        "--for",
        [&tail](const std::string &text) {
            auto duration = ParseDurationSuffix(text);
            if (!duration)
            {
                throw CLI::ValidationError("--for", duration.error());
            }
            tail.duration = *duration;
        },
        "Stop after this duration (30s, 5m, 1h), with exit 0. The default is to run until the source ends or a signal."
    );
    AddOptional(
        app,
        "--retention",
        tail.retention,
        std::format(
            "Rows kept in memory. 0 is unbounded. Other values must be from {} to {}. The default is {}.",
            loglib::MIN_RETENTION_LINES,
            loglib::MAX_RETENTION_LINES,
            loglib::DEFAULT_RETENTION_LINES
        )
    );
    app.add_flag("--strict", tail.strict, "Exit 4 when the parse reported errors. The default is to keep going.");
    command.finish = [](CommandParser &self) {
        auto &options = std::get<TailOptions>(self.options);
        ResolveSources(options.input, self.tokens.sources, true);
        Require(
            !options.retention || *options.retention == 0 ||
                (*options.retention >= loglib::MIN_RETENTION_LINES &&
                 *options.retention <= loglib::MAX_RETENTION_LINES),
            std::format(
                "'--retention' must be 0 (unbounded) or from {} to {}.",
                loglib::MIN_RETENTION_LINES,
                loglib::MAX_RETENTION_LINES
            )
        );
    };
}

/** @brief Program name prefix for @p app in usage and help text. */
[[nodiscard]] std::string ParentPrefix(const CLI::App &app)
{
    return app.get_parent() == nullptr ? std::string{} : "slp";
}

/** @brief `Usage:` line of @p app. */
[[nodiscard]] std::string UsageLine(const CLI::App &app)
{
    const std::string name = app.get_parent() == nullptr ? "slp" : "slp " + app.get_name();
    std::string usage = CLI::Formatter().make_usage(&app, name);
    while (!usage.empty() && (usage.back() == '\n' || usage.back() == ' '))
    {
        usage.pop_back();
    }
    while (!usage.empty() && usage.front() == '\n')
    {
        usage.erase(usage.begin());
    }
    if (!usage.starts_with("Usage:"))
    {
        usage.insert(0, "Usage: ");
    }
    return usage;
}

/**
 * @brief Turns a CLI11 message raised by @p app into one sentence.
 *
 * CLI11 prefixes `ExtrasError` with `<app name>: ` for the app that raised
 * it, which can be @p app or any of its parents.
 */
[[nodiscard]] std::string Sentence(std::string_view what, const CLI::App &app)
{
    for (const CLI::App *owner = &app; owner != nullptr; owner = owner->get_parent())
    {
        const std::string prefix = owner->get_name() + ": ";
        if (what.starts_with(prefix))
        {
            what.remove_prefix(prefix.size());
            break;
        }
    }
    std::string text(what);
    std::ranges::replace(text, '\n', ' ');
    while (!text.empty() && text.back() == ' ')
    {
        text.pop_back();
    }
    if (!text.empty() && text.back() != '.')
    {
        text.push_back('.');
    }
    return text;
}

/** @brief The hint for a flag given to the wrong command, if any argv word is one. */
[[nodiscard]] std::optional<std::string> MisplacedFlagHint(std::string_view command, int argc, char **argv)
{
    for (int index = 1; index < argc; ++index)
    {
        if (argv[index] == nullptr)
        {
            continue;
        }
        std::string_view word{argv[index]};
        word = word.substr(0, word.find('='));
        for (const MisplacedFlag &misplaced : MISPLACED_FLAGS)
        {
            if (misplaced.command == command && misplaced.flag == word)
            {
                return std::string(misplaced.message);
            }
        }
    }
    return std::nullopt;
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

std::string_view CommandName(const CommandOptions &command)
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
    Options result;
    CLI::App root(
        "StructuredLogProcessor reads, filters, and exports structured logs without a GUI. Run `slp <command> --help` "
        "for command flags.",
        "slp"
    );
    root.footer(std::string(ExitCodeHelpEpilog()));
    root.set_version_flag("--version", VersionLine(), "Print the slp and loglib versions and exit 0.");
    root.set_help_flag("-h,--help", "Show this help and exit 0.");
    root.require_subcommand(0, 1);
    AddGlobalFlags(root, result.global);

    constexpr std::array COMMAND_ADDERS = {
        &AddQuery, &AddStats, &AddSchema, &AddValidate, &AddBundle, &AddFormats, &AddTail
    };
    std::array<CommandParser, COMMAND_ADDERS.size()> commands;
    for (auto &&[add, command] : std::views::zip(COMMAND_ADDERS, commands))
    {
        add(root, command);
    }
    for (CommandParser &command : commands)
    {
        // Global flags may follow the command name.
        command.app->fallthrough();
        command.app->footer(std::string(ExitCodeHelpEpilog()));
        command.app->set_help_flag("-h,--help", "Show this help and exit 0.");
    }

    // The deepest command parsed so far names errors and help.
    auto current = [&root]() -> const CLI::App & {
        const auto parsed = root.get_subcommands();
        return parsed.empty() ? root : *parsed.back();
    };

    try
    {
        root.parse(argc, argv);
    }
    catch (const CLI::CallForHelp &)
    {
        result.kind = ParseKind::Help;
        result.helpText = current().help(ParentPrefix(current()));
        return result;
    }
    catch (const CLI::CallForVersion &)
    {
        result.kind = ParseKind::Version;
        return result;
    }
    catch (const CLI::ParseError &error)
    {
        const CLI::App &app = current();
        const std::string command = app.get_parent() == nullptr ? std::string{} : app.get_name();
        std::string message = Sentence(error.what(), app);
        if (dynamic_cast<const CLI::ExtrasError *>(&error) != nullptr)
        {
            message = MisplacedFlagHint(command, argc, argv).value_or(std::move(message));
        }
        return std::unexpected(UsageError{.command = command, .message = std::move(message), .usage = UsageLine(app)});
    }

    if (loglib::EnvironmentIsSet("NO_COLOR"))
    {
        result.global.color = ColorMode::Never;
    }
    if (!result.global.templateDir)
    {
        result.global.templateDir = loglib::EnvironmentPath("SLV_TEMPLATE_DIR");
    }

    for (CommandParser &command : commands)
    {
        if (!command.app->parsed())
        {
            continue;
        }
        if (command.tokens.helpAdvanced)
        {
            result.kind = ParseKind::Help;
            result.helpText = command.app->help(ParentPrefix(*command.app)) + std::string(ADVANCED_HELP);
            return result;
        }
        try
        {
            command.finish(command);
        }
        catch (const InvalidFlags &invalid)
        {
            return std::unexpected(
                UsageError{
                    .command = command.app->get_name(), .message = invalid.message, .usage = UsageLine(*command.app)
                }
            );
        }
        result.command = std::move(command.options);
        return result;
    }

    result.kind = ParseKind::MissingCommand;
    result.helpText = root.help();
    return result;
}

} // namespace slp
