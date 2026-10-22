#include "load_table.hpp"

#include "diagnostics.hpp"
#include "session_config.hpp"

#include <loglib/auto_detect_parser.hpp>
#include <loglib/decompressing_byte_source.hpp>
#include <loglib/file_line_source.hpp>
#include <loglib/format_detection.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_file.hpp>
#include <loglib/log_parse_sink.hpp>
#include <loglib/log_parser.hpp>
#include <loglib/regex_templates.hpp>
#include <loglib/session_bundle.hpp>
#include <loglib/source_plan.hpp>
#include <loglib/stdin_bytes_producer.hpp>
#include <loglib/stdin_peek.hpp>
#include <loglib/stream_line_source.hpp>
#include <loglib/user_paths.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace slp
{

namespace
{

[[nodiscard]] std::string AsciiLower(std::string_view text)
{
    std::string out(text);
    for (char &ch : out)
    {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return out;
}

[[nodiscard]] bool EqualsIgnoreCase(std::string_view lhs, std::string_view rhs) noexcept
{
    if (lhs.size() != rhs.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i)
    {
        if (std::tolower(static_cast<unsigned char>(lhs[i])) != std::tolower(static_cast<unsigned char>(rhs[i])))
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::string FormatName(loglib::Source::Format format) noexcept
{
    switch (format)
    {
    case loglib::Source::Format::Json:
        return "json";
    case loglib::Source::Format::Logfmt:
        return "logfmt";
    case loglib::Source::Format::Csv:
        return "csv";
    case loglib::Source::Format::Regex:
        return "regex";
    }
    return "unknown";
}

[[nodiscard]] std::optional<loglib::Source::Format> ToSourceFormat(InputFormat format) noexcept
{
    switch (format)
    {
    case InputFormat::Auto:
        return std::nullopt;
    case InputFormat::Json:
        return loglib::Source::Format::Json;
    case InputFormat::Logfmt:
        return loglib::Source::Format::Logfmt;
    case InputFormat::Csv:
        return loglib::Source::Format::Csv;
    case InputFormat::Regex:
        return loglib::Source::Format::Regex;
    }
    return std::nullopt;
}

[[nodiscard]] std::expected<std::string, std::string> ResolveRegexPattern(const Options &options)
{
    if (options.regex)
    {
        return *options.regex;
    }
    if (!options.regexTemplate)
    {
        return std::unexpected(std::string{"'--format regex' requires '--regex' or '--regex-template'."});
    }

    const std::string wanted = AsciiLower(*options.regexTemplate);
    const auto builtins = loglib::BuiltinRegexTemplateCatalog();
    for (const auto &entry : builtins)
    {
        if (AsciiLower(entry.slug) == wanted)
        {
            return entry.tmpl.pattern;
        }
    }

    std::filesystem::path templateDir =
        options.templateDir ? *options.templateDir : loglib::UserRegexTemplateDirectory();
    if (!templateDir.empty())
    {
        const std::filesystem::path candidate = templateDir / (*options.regexTemplate + ".json");
        std::ifstream input(candidate, std::ios::binary);
        if (input)
        {
            std::ostringstream buffer;
            buffer << input.rdbuf();
            try
            {
                return loglib::ParseRegexTemplate(buffer.str()).pattern;
            }
            catch (const std::exception &ex)
            {
                return std::unexpected(
                    std::format("could not parse regex template '{}': {}.", candidate.string(), ex.what())
                );
            }
        }
        const auto loaded = loglib::LoadRegexTemplatesFromDirectory(templateDir);
        for (const auto &tmpl : loaded.templates)
        {
            if (EqualsIgnoreCase(tmpl.name, *options.regexTemplate))
            {
                return tmpl.pattern;
            }
        }
    }

    return std::unexpected(std::format("unknown regex template slug '{}'.", *options.regexTemplate));
}

[[nodiscard]] CommandOutput Fail(ExitCode code, std::string_view sentence, std::string errExtra = {})
{
    CommandOutput out;
    out.exitCode = static_cast<int>(code);
    out.err = FormatErrorLine({}, sentence);
    out.err.push_back('\n');
    out.err += std::move(errExtra);
    return out;
}

struct QuerySink final : loglib::LogParseSink
{
    loglib::LogTable *table = nullptr;
    std::vector<std::string> errors;
    std::uint64_t *linesConsumed = nullptr;

    loglib::KeyIndex &Keys() override
    {
        return table->Keys();
    }

    void OnStarted() override
    {
    }

    void OnBatch(loglib::StreamedBatch batch) override
    {
        if (!batch.errors.empty())
        {
            errors.insert(
                errors.end(), std::make_move_iterator(batch.errors.begin()), std::make_move_iterator(batch.errors.end())
            );
            batch.errors.clear();
        }

        if (linesConsumed != nullptr)
        {
            if (batch.firstLineNumber != 0)
            {
                *linesConsumed = std::max(*linesConsumed, static_cast<std::uint64_t>(batch.firstLineNumber));
            }
            for (const loglib::LogLine &line : batch.lines)
            {
                *linesConsumed = std::max(*linesConsumed, static_cast<std::uint64_t>(line.LineId()) + std::uint64_t{1});
            }
        }

        AppendStreamedBatch(*table, std::move(batch));
    }

    void OnFinished(bool /*cancelled*/) override
    {
    }
};

[[nodiscard]] std::optional<std::uint64_t> ParseLeadingLineNumber(std::string_view text)
{
    std::uint64_t value = 0;
    std::size_t index = 0;
    while (index < text.size() && std::isdigit(static_cast<unsigned char>(text[index])) != 0)
    {
        value = value * 10 + static_cast<std::uint64_t>(text[index] - '0');
        ++index;
    }
    if (index == 0)
    {
        return std::nullopt;
    }
    return value;
}

void UpdateLinesConsumedFromErrors(std::uint64_t &linesConsumed, const std::vector<std::string> &errors)
{
    constexpr std::string_view kSinglePrefix = "Error on line ";
    constexpr std::string_view kRangePrefix = "Error on lines ";
    for (const std::string &error : errors)
    {
        if (error.starts_with(kSinglePrefix))
        {
            const auto parsed = ParseLeadingLineNumber(std::string_view{error}.substr(kSinglePrefix.size()));
            if (parsed)
            {
                linesConsumed = std::max(linesConsumed, *parsed);
            }
            continue;
        }
        if (error.starts_with(kRangePrefix))
        {
            const std::string_view tail = std::string_view{error}.substr(kRangePrefix.size());
            const std::size_t dash = tail.find('-');
            if (dash == std::string_view::npos)
            {
                continue;
            }
            const auto endLine = ParseLeadingLineNumber(tail.substr(dash + 1));
            if (endLine)
            {
                linesConsumed = std::max(linesConsumed, *endLine);
            }
        }
    }
}

[[nodiscard]] std::expected<loglib::DetectedFormat, CommandOutput> ResolveFormat(
    const Options &options, const std::filesystem::path &effectivePath, CommandOutput &warnings
)
{
    (void)warnings;
    if (const auto pinned = ToSourceFormat(options.format))
    {
        std::string pattern;
        if (*pinned == loglib::Source::Format::Regex)
        {
            auto resolved = ResolveRegexPattern(options);
            if (!resolved)
            {
                return std::unexpected(Fail(ExitCode::Usage, resolved.error()));
            }
            pattern = std::move(*resolved);
        }
        auto parser = loglib::MakeParserForFormat(*pinned, pattern);
        if (!parser->IsValid(effectivePath))
        {
            return std::unexpected(Fail(
                ExitCode::Input,
                std::format("source '{}' was rejected by the {} parser.", effectivePath.string(), FormatName(*pinned))
            ));
        }
        return loglib::DetectedFormat{.format = *pinned, .regexPattern = std::move(pattern)};
    }

    loglib::DetectedFormat detected = loglib::DetectFormatForPath(effectivePath);
    if (detected.format == loglib::Source::Format::Regex && detected.regexPattern.empty())
    {
        return std::unexpected(
            Fail(ExitCode::Input, std::format("could not detect a parser for source '{}'.", effectivePath.string()))
        );
    }
    return detected;
}

[[nodiscard]] loglib::ParserOptions MakeParserOptions(const Options &options, const RuntimeContext &runtime)
{
    loglib::ParserOptions parserOptions;
    parserOptions.stopToken = runtime.stop;
    parserOptions.threads = options.threads;
    parserOptions.multilineLogfmt = !options.noMultilineLogfmt;
    if (options.batchSizeBytes)
    {
        parserOptions.batchSizeBytes = static_cast<std::size_t>(*options.batchSizeBytes);
    }
    return parserOptions;
}

[[nodiscard]] CommandOutput CancelledOutput()
{
    CommandOutput cancelled;
    cancelled.exitCode = static_cast<int>(ExitCode::Cancelled);
    return cancelled;
}

[[nodiscard]] CommandOutput OpenFailure(const std::filesystem::path &path, std::string_view reason)
{
    return Fail(ExitCode::Input, std::format("source '{}': {}", path.string(), reason));
}

[[nodiscard]] std::expected<loglib::DetectedFormat, CommandOutput> ResolveFormatFromBytes(
    const Options &options, std::string_view sniff, std::string_view sourceLabel
)
{
    if (const auto pinned = ToSourceFormat(options.format))
    {
        std::string pattern;
        if (*pinned == loglib::Source::Format::Regex)
        {
            auto resolved = ResolveRegexPattern(options);
            if (!resolved)
            {
                return std::unexpected(Fail(ExitCode::Usage, resolved.error()));
            }
            pattern = std::move(*resolved);
        }
        auto parser = loglib::MakeParserForFormat(*pinned, pattern);
        if (!parser->IsValidBytes(sniff))
        {
            return std::unexpected(Fail(
                ExitCode::Input,
                std::format("source '{}' was rejected by the {} parser.", sourceLabel, FormatName(*pinned))
            ));
        }
        return loglib::DetectedFormat{.format = *pinned, .regexPattern = std::move(pattern)};
    }
    return loglib::DetectFormatFromBytes(sniff);
}

void NoteDetectedFormat(std::string &detectedFormat, std::string &detectedRegex, const loglib::DetectedFormat &format)
{
    if (detectedFormat.empty())
    {
        detectedFormat = FormatName(format.format);
        detectedRegex = format.regexPattern;
    }
}

[[nodiscard]] CommandOutput ParseFilePath(
    const Options &options,
    const RuntimeContext &runtime,
    loglib::LogTable &table,
    bool &first,
    std::vector<std::string> &parseErrors,
    std::vector<std::shared_ptr<loglib::DecompressingByteSource>> &ownedDecompressors,
    const std::filesystem::path &displayPath,
    bool discardFirstLine,
    std::string &detectedFormat,
    std::string &detectedRegex,
    std::uint64_t &linesConsumed
)
{
    std::error_code ec;
    if (!std::filesystem::exists(displayPath, ec) || ec)
    {
        return OpenFailure(displayPath, "does not exist");
    }
    if (std::filesystem::is_directory(displayPath, ec))
    {
        return OpenFailure(displayPath, "is a directory");
    }

    try
    {
        loglib::DecompressingByteSource::Options decompressOptions;
        decompressOptions.discardFirstLine = discardFirstLine;
        auto decompressor = std::make_shared<loglib::DecompressingByteSource>(
            displayPath, loglib::DecompressingByteSource::ProgressCallback{}, runtime.stop, decompressOptions
        );
        if (discardFirstLine)
        {
            const auto metadata = loglib::ParseSessionBundleMetadata(decompressor->DiscardedFirstLine());
            // Seed the table with the embedded session before the first
            // BeginStreaming so column pins and filter defaults apply.
            if (first && !options.config)
            {
                table.Configuration().SetConfiguration(metadata.configuration);
            }
        }

        const std::filesystem::path effective = decompressor->EffectivePath();
        ownedDecompressors.push_back(decompressor);

        CommandOutput unused;
        auto format = ResolveFormat(options, effective, unused);
        if (!format)
        {
            return std::move(format.error());
        }
        NoteDetectedFormat(detectedFormat, detectedRegex, *format);

        auto logFile = std::make_unique<loglib::LogFile>(effective, displayPath);
        logFile->AttachLifetimeAnchor(decompressor);
        auto lineSource = std::make_unique<loglib::FileLineSource>(std::move(logFile));
        loglib::FileLineSource *rawSource = lineSource.get();

        if (first)
        {
            table.BeginStreaming(std::move(lineSource));
            first = false;
        }
        else
        {
            table.AppendStreaming(std::move(lineSource));
        }

        auto parser = loglib::MakeParserForFormat(format->format, format->regexPattern);
        QuerySink sink;
        sink.table = &table;
        sink.linesConsumed = &linesConsumed;
        parser->ParseStreaming(*rawSource, sink, MakeParserOptions(options, runtime));
        parseErrors.insert(parseErrors.end(), sink.errors.begin(), sink.errors.end());
    }
    catch (const loglib::DecompressionCancelled &)
    {
        return CancelledOutput();
    }
    catch (const loglib::DecompressionSizeCapExceeded &ex)
    {
        return Fail(ExitCode::Input, ex.what());
    }
    catch (const loglib::SessionBundleVersionError &ex)
    {
        return Fail(ExitCode::Input, ex.what());
    }
    catch (const loglib::SessionBundleReadError &ex)
    {
        return Fail(ExitCode::Input, ex.what());
    }
    catch (const std::exception &ex)
    {
        return OpenFailure(displayPath, ex.what());
    }

    if (runtime.stop.stop_requested())
    {
        return CancelledOutput();
    }
    return {};
}

[[nodiscard]] CommandOutput ParseStdin(
    const Options &options,
    const RuntimeContext &runtime,
    loglib::LogTable &table,
    bool &first,
    std::vector<std::string> &parseErrors,
    std::string &detectedFormat,
    std::string &detectedRegex,
    std::uint64_t &linesConsumed
)
{
    if (loglib::IsStdinInteractive())
    {
        return Fail(ExitCode::Input, "standard input is a terminal; pipe log data into slp or redirect from a file");
    }

    constexpr auto kPeekTimeout = std::chrono::milliseconds(500);
    std::string peek = loglib::StdinPeek(loglib::PROBE_BYTES_BUDGET, kPeekTimeout);
    auto format = ResolveFormatFromBytes(options, peek, "-");
    if (!format)
    {
        return std::move(format.error());
    }
    NoteDetectedFormat(detectedFormat, detectedRegex, *format);

    std::unique_ptr<loglib::StdinBytesProducer> producer;
    try
    {
        producer = std::make_unique<loglib::StdinBytesProducer>();
    }
    catch (const std::exception &ex)
    {
        return Fail(ExitCode::Input, std::format("failed to attach to stdin: {}", ex.what()));
    }

    auto lineSource = std::make_unique<loglib::StreamLineSource>(std::filesystem::path{"-"}, std::move(producer));
    loglib::StreamLineSource *rawSource = lineSource.get();
    if (first)
    {
        table.BeginStreaming(std::move(lineSource));
        first = false;
    }
    else
    {
        table.AppendStreaming(std::move(lineSource));
    }

    auto parser = loglib::MakeParserForFormat(format->format, format->regexPattern);
    auto parserOptions = MakeParserOptions(options, runtime);
    parserOptions.initialCarry = std::move(peek);
    QuerySink sink;
    sink.table = &table;
    sink.linesConsumed = &linesConsumed;
    parser->ParseStreaming(*rawSource, sink, parserOptions);
    parseErrors.insert(parseErrors.end(), sink.errors.begin(), sink.errors.end());

    if (runtime.stop.stop_requested())
    {
        return CancelledOutput();
    }
    return {};
}

[[nodiscard]] CommandOutput LoadSources(const Options &options, const RuntimeContext &runtime, LoadedTable &loaded)
{
    CommandOutput side;
    LoadUserTemplates(options, side);
    if (options.sources.empty())
    {
        return Fail(ExitCode::Usage, "at least one source is required.");
    }

    // `--config` replaces any bundle-embedded configuration, so seed it
    // here and let `ParseFilePath` skip its own bundle seeding.
    if (options.config)
    {
        auto configuration = LoadConfigFile(*options.config);
        if (!configuration)
        {
            return Fail(ExitCode::Input, configuration.error());
        }
        loaded.table.Configuration().SetConfiguration(std::move(*configuration));
    }

    bool first = true;
    std::vector<std::filesystem::path> pendingPaths;

    auto flushPaths = [&]() -> CommandOutput {
        if (pendingPaths.empty())
        {
            return {};
        }
        loglib::SourcePlanOptions planOptions;
        planOptions.rotationHistory = !options.noRotationHistory;
        const auto plan = loglib::ResolveSourcePlan(pendingPaths, planOptions);
        if (options.verbose > 0 && !options.quiet)
        {
            for (const auto &entry : plan)
            {
                std::string message = "loading " + entry.path.string();
                if (entry.autoAdded)
                {
                    message += " (rotated sibling)";
                }
                side.err += FormatWarningLine(message);
                side.err.push_back('\n');
            }
        }
        for (const auto &entry : plan)
        {
            CommandOutput part = ParseFilePath(
                options,
                runtime,
                loaded.table,
                first,
                loaded.parseErrors,
                loaded.ownedDecompressors,
                entry.path,
                false,
                loaded.detectedFormat,
                loaded.detectedRegexPattern,
                loaded.linesConsumed
            );
            if (part.exitCode != 0)
            {
                side.err += part.err;
                side.exitCode = part.exitCode;
                return side;
            }
        }
        pendingPaths.clear();
        return {};
    };

    for (const SourceSpec &source : options.sources)
    {
        if (source.kind == SourceKind::Tcp || source.kind == SourceKind::Udp)
        {
            return Fail(ExitCode::Usage, std::format("Network source '{}' is only valid for `slp tail`.", source.text));
        }
        if (source.kind == SourceKind::Path)
        {
            pendingPaths.push_back(source.path);
            continue;
        }

        CommandOutput flushed = flushPaths();
        if (flushed.exitCode != 0)
        {
            return flushed;
        }
        side.err += flushed.err;

        if (source.kind == SourceKind::Stdin)
        {
            CommandOutput part = ParseStdin(
                options,
                runtime,
                loaded.table,
                first,
                loaded.parseErrors,
                loaded.detectedFormat,
                loaded.detectedRegexPattern,
                loaded.linesConsumed
            );
            if (part.exitCode != 0)
            {
                side.err += part.err;
                side.exitCode = part.exitCode;
                return side;
            }
            continue;
        }

        if (source.kind == SourceKind::Bundle)
        {
            CommandOutput part = ParseFilePath(
                options,
                runtime,
                loaded.table,
                first,
                loaded.parseErrors,
                loaded.ownedDecompressors,
                source.path,
                true,
                loaded.detectedFormat,
                loaded.detectedRegexPattern,
                loaded.linesConsumed
            );
            if (part.exitCode != 0)
            {
                side.err += part.err;
                side.exitCode = part.exitCode;
                return side;
            }
            continue;
        }
    }

    CommandOutput flushed = flushPaths();
    if (flushed.exitCode != 0)
    {
        return flushed;
    }
    side.err += flushed.err;

    loaded.table.FinalizeAutoDetection();
    loaded.table.ApplyPendingLevelBubbles();
    UpdateLinesConsumedFromErrors(loaded.linesConsumed, loaded.parseErrors);
    return side;
}

} // namespace

void LoadUserTemplates(const Options &options, CommandOutput &out)
{
    const std::filesystem::path dir = options.templateDir ? *options.templateDir : loglib::UserRegexTemplateDirectory();
    if (dir.empty())
    {
        return;
    }
    const auto loaded = loglib::LoadRegexTemplatesFromDirectory(dir);
    for (const std::string &error : loaded.errors)
    {
        out.err += FormatWarningLine(error);
        out.err.push_back('\n');
    }
    if (!loaded.templates.empty())
    {
        loglib::SetExtraRegexTemplates(loaded.templates);
    }
}

void AppendStreamedBatch(loglib::LogTable &table, loglib::StreamedBatch batch)
{
    const std::size_t oldColumnCount = table.ColumnCount();
    table.AppendBatch(std::move(batch));
    const std::size_t newColumnCount = table.ColumnCount();

    // Match LogModel::AppendBatch: bubble each newly appended Time column to index 0.
    if (newColumnCount > oldColumnCount)
    {
        const auto &columns = table.Configuration().Configuration().columns;
        std::vector<std::size_t> timeColumns;
        for (std::size_t columnIndex = oldColumnCount; columnIndex < newColumnCount; ++columnIndex)
        {
            if (columns[columnIndex].type == loglib::ColumnType::Time)
            {
                timeColumns.push_back(columnIndex);
            }
        }
        for (const std::size_t srcIndex : timeColumns)
        {
            if (srcIndex == 0)
            {
                continue;
            }
            table.MoveColumn(srcIndex, 0);
        }
    }

    table.ApplyPendingLevelBubbles();
}

std::expected<std::unique_ptr<loglib::LogParser>, CommandOutput> MakeStreamParser(const Options &options)
{
    const auto pinned = ToSourceFormat(options.format);
    if (!pinned)
    {
        return std::make_unique<loglib::AutoDetectParser>();
    }
    std::string pattern;
    if (*pinned == loglib::Source::Format::Regex)
    {
        auto resolved = ResolveRegexPattern(options);
        if (!resolved)
        {
            return std::unexpected(Fail(ExitCode::Usage, resolved.error()));
        }
        pattern = std::move(*resolved);
    }
    return loglib::MakeParserForFormat(*pinned, pattern);
}

loglib::ParserOptions StreamParserOptions(const Options &options, const loglib::StopToken &stop)
{
    loglib::ParserOptions parserOptions;
    parserOptions.stopToken = stop;
    parserOptions.threads = options.threads;
    parserOptions.multilineLogfmt = !options.noMultilineLogfmt;
    return parserOptions;
}

std::pair<LoadedTable, CommandOutput> LoadTable(const Options &options, const RuntimeContext &runtime)
{
    LoadedTable loaded;
    CommandOutput side = LoadSources(options, runtime, loaded);
    return {std::move(loaded), std::move(side)};
}

} // namespace slp
