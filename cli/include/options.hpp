#pragma once

#include <loglib/histogram_bucket_index.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace slp
{

/** @brief How `--color` is resolved after `NO_COLOR` is applied. */
enum class ColorMode
{
    Auto,
    Always,
    Never,
};

/** @brief Parser selected by `--format`, after `--regex` implication. */
enum class InputFormat
{
    Auto,
    Json,
    Logfmt,
    Csv,
    Regex,
};

/** @brief Kind of one positional source. */
enum class SourceKind
{
    Path,
    Stdin,
    Tcp,
    Udp,
    Bundle,
};

/** @brief One positional source after classification. */
struct SourceSpec
{
    SourceKind kind = SourceKind::Path;
    /** @brief Text the user typed. */
    std::string text;
    /** @brief Filesystem path for `Path` and `Bundle`. */
    std::filesystem::path path;
    /** @brief Bind host for `Tcp` and `Udp`. Empty means every interface. */
    std::string host;
    /** @brief Bind port for `Tcp` and `Udp`. */
    std::uint16_t port = 0;
};

/**
 * @brief Flags every command accepts, before or after the command name.
 *
 * Environment input is limited to `NO_COLOR` and `SLV_TEMPLATE_DIR`.
 * `--template-dir` wins over `SLV_TEMPLATE_DIR`. `NO_COLOR` forces `Never`.
 */
struct GlobalOptions
{
    bool quiet = false;
    int verbose = 0;
    ColorMode color = ColorMode::Auto;
    std::uint32_t threads = 0;
    std::string timeZone = "local";
    bool noRotationHistory = false;
    std::optional<std::filesystem::path> templateDir;
};

/**
 * @brief Sources and parser selection for commands that read logs.
 *
 * `config` is the `--config` session or columns file; only `query` and
 * `bundle` accept it, and it seeds the table before the first row loads.
 */
struct InputOptions
{
    std::vector<SourceSpec> sources;
    InputFormat format = InputFormat::Auto;
    std::optional<std::string> regex;
    std::optional<std::string> regexTemplate;
    bool noMultilineLogfmt = false;
    std::optional<std::uint64_t> batchSizeBytes;
    std::optional<std::filesystem::path> config;
};

/** @brief Row-selection flags shared by `query`, `stats`, `bundle`, and `tail`. */
struct FilterOptions
{
    std::vector<std::string> filters;
    std::optional<std::string> since;
    std::optional<std::string> until;
    std::optional<std::string> minLevel;
    bool invert = false;

    /** @brief Whether any row-restricting flag is set. */
    [[nodiscard]] bool Active() const noexcept;

    /** @brief Whether `--invert` is the only row flag, which negates the configured expression. */
    [[nodiscard]] bool OnlyInvert() const noexcept;
};

/** @brief Sort and column flags that override the configured view. */
struct ViewOptions
{
    std::optional<std::string> sort;
    std::optional<std::string> columns;
    bool allColumns = false;
};

/** @brief `--save-session` and `--save-columns` destinations. */
struct SaveTargets
{
    std::optional<std::filesystem::path> session;
    std::optional<std::filesystem::path> columns;
};

/** @brief Rendering of `stats`, `schema`, `validate`, and `formats`. */
enum class ReportFormat
{
    Text,
    Json,
    Csv,
};

/** @brief Export format of `query`. */
enum class QueryFormat
{
    Jsonl,
    Csv,
    Markdown,
    Snapshot,
};

/** @brief `slp query`. */
struct QueryOptions
{
    static constexpr std::string_view NAME = "query";

    InputOptions input;
    FilterOptions filter;
    ViewOptions view;
    SaveTargets save;
    std::optional<std::uint64_t> limit;
    std::optional<std::uint64_t> offset;
    std::optional<std::uint64_t> last;
    /** @brief Destination path, or `-` for stdout. */
    std::string output = "-";
    QueryFormat outputFormat = QueryFormat::Jsonl;
    bool noHeader = false;
    bool jsonVisibleOnly = false;
    bool force = false;
    bool count = false;
    bool allowEmpty = false;
    bool strict = false;
};

/** @brief Summary axis of `slp stats`. */
enum class StatsAxis
{
    Level,
    Time,
    Column,
};

/** @brief Default number of distinct values `slp stats --by column` prints. */
inline constexpr std::uint64_t DEFAULT_TOP_VALUES = 20;

/** @brief Default number of individual parse errors `slp validate` prints. */
inline constexpr std::uint64_t DEFAULT_MAX_ERRORS = 20;

/** @brief `slp stats`. `column` is set whenever `by` is `Column`. */
struct StatsOptions
{
    static constexpr std::string_view NAME = "stats";

    InputOptions input;
    FilterOptions filter;
    StatsAxis by = StatsAxis::Level;
    /** @brief Histogram width for `--by time`; unset selects it automatically. */
    std::optional<loglib::HistogramBucketSize> bucket;
    std::optional<std::uint64_t> maxBuckets;
    std::uint64_t top = DEFAULT_TOP_VALUES;
    std::optional<std::string> column;
    ReportFormat outputFormat = ReportFormat::Text;
};

/** @brief `slp schema`. */
struct SchemaOptions
{
    static constexpr std::string_view NAME = "schema";

    InputOptions input;
    std::uint64_t sample = 3;
    ReportFormat outputFormat = ReportFormat::Text;
};

/** @brief `slp validate`. */
struct ValidateOptions
{
    static constexpr std::string_view NAME = "validate";

    InputOptions input;
    std::uint64_t maxErrors = DEFAULT_MAX_ERRORS;
    /** @brief Accepted parse-error percentage; unset means any error fails. */
    std::optional<double> maxErrorRate;
    ReportFormat outputFormat = ReportFormat::Text;
};

/** @brief `slp bundle`. `output` is a file path; stdout is rejected while parsing. */
struct BundleOptions
{
    static constexpr std::string_view NAME = "bundle";

    InputOptions input;
    FilterOptions filter;
    ViewOptions view;
    std::filesystem::path output;
    bool filteredRows = false;
    int compressionLevel = 3;
    std::uint32_t bundleThreads = 0;
};

/** @brief `slp formats`. */
struct FormatsOptions
{
    static constexpr std::string_view NAME = "formats";

    std::optional<std::string> showSlug;
    bool listUserTemplates = false;
    ReportFormat outputFormat = ReportFormat::Text;
};

/** @brief Row rendering of `slp tail`. `Auto` is text on a terminal and jsonl otherwise. */
enum class TailFormat
{
    Auto,
    Jsonl,
    Text,
};

/** @brief `slp tail`. Only `view.columns` is accepted. */
struct TailOptions
{
    static constexpr std::string_view NAME = "tail";

    InputOptions input;
    FilterOptions filter;
    ViewOptions view;
    /** @brief File to append rows to; unset means stdout. */
    std::optional<std::filesystem::path> output;
    TailFormat outputFormat = TailFormat::Auto;
    bool strict = false;
    std::optional<std::uint64_t> maxRows;
    std::optional<std::chrono::milliseconds> duration;
    /** @brief nullopt means the shared default retention. 0 means unbounded. */
    std::optional<std::uint64_t> retention;
};

/** @brief Options of the selected subcommand. */
using CommandOptions = std::
    variant<QueryOptions, StatsOptions, SchemaOptions, ValidateOptions, BundleOptions, FormatsOptions, TailOptions>;

/** @brief Name of the subcommand that @p command holds. */
[[nodiscard]] std::string_view CommandName(const CommandOptions &command);

/**
 * @brief Failure of `ParseArguments`. Exit code is always 2.
 *
 * `message` is one sentence and does not include the `slp:` prefix.
 * `usage` is the `Usage:` line of the command that failed.
 */
struct UsageError
{
    std::string command;
    std::string message;
    std::string usage;
};

/** @brief Why `ParseArguments` returned success without a command body. */
enum class ParseKind
{
    /** @brief A subcommand is ready to run. */
    Run,
    /** @brief `--help` or `--help-advanced`. Exit 0. */
    Help,
    /** @brief `--version`. Exit 0. */
    Version,
    /** @brief No subcommand. Print `helpText` and exit 2. */
    MissingCommand,
};

/** @brief Fully resolved invocation. Produced without reading files. */
struct Options
{
    ParseKind kind = ParseKind::Run;
    /** @brief Help text when `kind` is `Help` or `MissingCommand`. */
    std::string helpText;
    GlobalOptions global;
    /** @brief Meaningful only when `kind` is `Run`. */
    CommandOptions command;
};

/**
 * @brief Parses argv into `Options` or a usage error.
 *
 * Does not read the log sources and does not write to the terminal.
 * Global flags are accepted both before and after the subcommand.
 */
[[nodiscard]] std::expected<Options, UsageError> ParseArguments(int argc, char **argv);

/** @brief Classifies one positional source token. */
[[nodiscard]] std::expected<SourceSpec, std::string> ClassifySource(std::string_view text);

/**
 * @brief Parses a byte size such as `1MiB`.
 *
 * The unit is a power of 1024. Accepted suffixes are `B`, `K`, `KB`,
 * `KiB`, `M`, `MB`, `MiB`, `G`, `GB`, and `GiB`. A bare integer is bytes.
 */
[[nodiscard]] std::expected<std::uint64_t, std::string> ParseSizeSuffix(std::string_view text);

/**
 * @brief Parses a duration such as `30s`, `5m`, or `1h`.
 *
 * The suffix is required.
 */
[[nodiscard]] std::expected<std::chrono::milliseconds, std::string> ParseDurationSuffix(std::string_view text);

/** @brief `slp <version> (StructuredLogProcessor) loglib <version>`. */
[[nodiscard]] std::string VersionLine();

} // namespace slp
