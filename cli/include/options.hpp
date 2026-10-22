#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace slp
{

/** @brief Subcommand selected by argv, or `None` when the user passed none. */
enum class Command
{
    None,
    Query,
    Stats,
    Schema,
    Validate,
    Bundle,
    Formats,
    Tail,
};

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

/**
 * @brief Fully resolved invocation. Produced without reading files.
 *
 * Environment input is limited to `NO_COLOR` and `SLV_TEMPLATE_DIR`.
 * The flag wins over `SLV_TEMPLATE_DIR`. `NO_COLOR` forces `Never`.
 */
struct Options
{
    ParseKind kind = ParseKind::Run;
    /** @brief Help text when `kind` is `Help` or `MissingCommand`. */
    std::string helpText;
    Command command = Command::None;

    bool quiet = false;
    int verbose = 0;
    ColorMode color = ColorMode::Auto;
    std::uint32_t threads = 0;
    std::string timeZone = "local";
    bool noRotationHistory = false;
    std::optional<std::filesystem::path> templateDir;

    std::vector<SourceSpec> sources;

    InputFormat format = InputFormat::Auto;
    std::optional<std::string> regex;
    std::optional<std::string> regexTemplate;
    bool noMultilineLogfmt = false;
    std::optional<std::uint64_t> batchSizeBytes;
    std::vector<std::string> filters;
    std::optional<std::string> since;
    std::optional<std::string> until;
    std::optional<std::string> minLevel;
    bool invert = false;
    std::optional<std::string> sort;
    std::optional<std::string> columns;
    bool allColumns = false;
    std::optional<std::uint64_t> limit;
    std::optional<std::uint64_t> offset;
    std::optional<std::uint64_t> last;
    std::string output = "-";
    bool outputExplicit = false;
    bool force = false;
    bool noHeader = false;
    bool jsonVisibleOnly = false;
    bool count = false;
    bool allowEmpty = false;
    bool strict = false;
    std::optional<std::filesystem::path> config;
    std::optional<std::filesystem::path> saveSession;
    std::optional<std::filesystem::path> saveColumns;

    /**
     * @brief `--output-format` token for the selected command.
     *
     * Empty for `bundle`. `tail` uses `auto` when the flag is omitted,
     * meaning jsonl when stdout is not a terminal and text when it is.
     */
    std::string outputFormat;

    std::string statsBy = "level";
    std::string bucket = "auto";
    std::optional<std::uint64_t> maxBuckets;
    std::uint64_t top = 20;
    std::optional<std::string> column;

    std::uint64_t sample = 3;
    std::uint64_t maxErrors = 20;
    std::optional<double> maxErrorRate;

    bool filteredRows = false;
    int compressionLevel = 3;
    std::uint32_t bundleThreads = 0;

    std::optional<std::string> showSlug;
    bool listUserTemplates = false;

    std::optional<std::uint64_t> maxRows;
    std::optional<std::chrono::milliseconds> duration;
    /** @brief nullopt means the shared default retention. 0 means unbounded. */
    std::optional<std::uint64_t> retention;
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
