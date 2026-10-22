#include "commands.hpp"

#include "diagnostics.hpp"
#include "platform.hpp"

#include <loglib/column_projection.hpp>
#include <loglib/decompressing_byte_source.hpp>
#include <loglib/exports/export_sink.hpp>
#include <loglib/exports/row_exporter.hpp>
#include <loglib/file_line_source.hpp>
#include <loglib/filter_expression.hpp>
#include <loglib/format_detection.hpp>
#include <loglib/leaf_rule_compile.hpp>
#include <loglib/log_compare.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_file.hpp>
#include <loglib/log_filter.hpp>
#include <loglib/log_level.hpp>
#include <loglib/log_parse_sink.hpp>
#include <loglib/log_parser.hpp>
#include <loglib/log_table.hpp>
#include <loglib/query_parser.hpp>
#include <loglib/regex_templates.hpp>
#include <loglib/row_ordering.hpp>
#include <loglib/session_bundle.hpp>
#include <loglib/source_plan.hpp>
#include <loglib/stdin_bytes_producer.hpp>
#include <loglib/stdin_peek.hpp>
#include <loglib/stream_line_source.hpp>
#include <loglib/user_paths.hpp>
#include <loglib/user_timestamp.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
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

[[nodiscard]] std::optional<loglib::exports::ExportFormat> ToExportFormat(std::string_view token) noexcept
{
    if (token == "jsonl")
    {
        return loglib::exports::ExportFormat::JsonLines;
    }
    if (token == "csv")
    {
        return loglib::exports::ExportFormat::Csv;
    }
    if (token == "markdown")
    {
        return loglib::exports::ExportFormat::Markdown;
    }
    if (token == "snapshot")
    {
        return loglib::exports::ExportFormat::Snapshot;
    }
    return std::nullopt;
}

[[nodiscard]] std::string AvailableColumnList(const std::vector<loglib::Column> &columns)
{
    if (columns.empty())
    {
        return "(none)";
    }
    std::ostringstream out;
    for (std::size_t i = 0; i < columns.size(); ++i)
    {
        if (i != 0)
        {
            out << ", ";
        }
        out << columns[i].header;
    }
    return out.str();
}

[[nodiscard]] int FindColumnByName(const std::vector<loglib::Column> &columns, std::string_view name)
{
    for (std::size_t i = 0; i < columns.size(); ++i)
    {
        if (columns[i].header == name)
        {
            return static_cast<int>(i);
        }
    }
    for (std::size_t i = 0; i < columns.size(); ++i)
    {
        for (const std::string &key : columns[i].keys)
        {
            if (key == name)
            {
                return static_cast<int>(i);
            }
        }
    }
    for (std::size_t i = 0; i < columns.size(); ++i)
    {
        if (EqualsIgnoreCase(columns[i].header, name))
        {
            return static_cast<int>(i);
        }
        for (const std::string &key : columns[i].keys)
        {
            if (EqualsIgnoreCase(key, name))
            {
                return static_cast<int>(i);
            }
        }
    }
    return -1;
}

[[nodiscard]] int FirstColumnOfType(const std::vector<loglib::Column> &columns, loglib::ColumnType type)
{
    for (std::size_t i = 0; i < columns.size(); ++i)
    {
        if (columns[i].type == type)
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}

[[nodiscard]] std::vector<std::string> SplitCsv(std::string_view text)
{
    std::vector<std::string> parts;
    std::size_t begin = 0;
    while (begin <= text.size())
    {
        const std::size_t comma = text.find(',', begin);
        const std::string_view part =
            comma == std::string_view::npos ? text.substr(begin) : text.substr(begin, comma - begin);
        std::size_t start = 0;
        std::size_t end = part.size();
        while (start < end && std::isspace(static_cast<unsigned char>(part[start])) != 0)
        {
            ++start;
        }
        while (end > start && std::isspace(static_cast<unsigned char>(part[end - 1])) != 0)
        {
            --end;
        }
        if (start < end)
        {
            parts.emplace_back(part.substr(start, end - start));
        }
        if (comma == std::string_view::npos)
        {
            break;
        }
        begin = comma + 1;
    }
    return parts;
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

struct QuerySink final : loglib::LogParseSink
{
    loglib::LogTable *table = nullptr;
    std::vector<std::string> errors;

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

        const std::size_t oldColumnCount = table->ColumnCount();
        table->AppendBatch(std::move(batch));
        const std::size_t newColumnCount = table->ColumnCount();

        // Match LogModel::AppendBatch: bubble each newly appended Time column to index 0.
        if (newColumnCount > oldColumnCount)
        {
            const auto &columns = table->Configuration().Configuration().columns;
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
                table->MoveColumn(srcIndex, 0);
            }
        }

        table->ApplyPendingLevelBubbles();
    }

    void OnFinished(bool /*cancelled*/) override
    {
    }
};

[[nodiscard]] CommandOutput Fail(ExitCode code, std::string_view sentence, std::string errExtra = {})
{
    CommandOutput out;
    out.exitCode = static_cast<int>(code);
    out.err = FormatErrorLine("query", sentence);
    out.err.push_back('\n');
    out.err += std::move(errExtra);
    return out;
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

[[nodiscard]] CommandOutput ParseFilePath(
    const Options &options,
    const RuntimeContext &runtime,
    loglib::LogTable &table,
    bool &first,
    std::vector<std::string> &parseErrors,
    std::vector<std::shared_ptr<loglib::DecompressingByteSource>> &ownedDecompressors,
    const std::filesystem::path &displayPath,
    bool discardFirstLine
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
            if (first)
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
    std::vector<std::string> &parseErrors
)
{
    if (loglib::IsStdinInteractive())
    {
        return Fail(
            ExitCode::Input,
            "standard input is a terminal; pipe log data into slp or redirect from a file"
        );
    }

    constexpr auto kPeekTimeout = std::chrono::milliseconds(500);
    std::string peek = loglib::StdinPeek(loglib::PROBE_BYTES_BUDGET, kPeekTimeout);
    auto format = ResolveFormatFromBytes(options, peek, "-");
    if (!format)
    {
        return std::move(format.error());
    }

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
    parser->ParseStreaming(*rawSource, sink, parserOptions);
    parseErrors.insert(parseErrors.end(), sink.errors.begin(), sink.errors.end());

    if (runtime.stop.stop_requested())
    {
        return CancelledOutput();
    }
    return {};
}

[[nodiscard]] CommandOutput LoadSources(
    const Options &options,
    const RuntimeContext &runtime,
    loglib::LogTable &table,
    std::vector<std::string> &parseErrors,
    std::vector<std::shared_ptr<loglib::DecompressingByteSource>> &ownedDecompressors
)
{
    CommandOutput side;
    LoadUserTemplates(options, side);
    if (options.sources.empty())
    {
        return Fail(ExitCode::Usage, "query requires at least one source.");
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
            CommandOutput part =
                ParseFilePath(options, runtime, table, first, parseErrors, ownedDecompressors, entry.path, false);
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
            return Fail(
                ExitCode::Usage, std::format("Network source '{}' is only valid for `slp tail`.", source.text)
            );
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
            CommandOutput part = ParseStdin(options, runtime, table, first, parseErrors);
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
            CommandOutput part =
                ParseFilePath(options, runtime, table, first, parseErrors, ownedDecompressors, source.path, true);
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

    table.FinalizeAutoDetection();
    table.ApplyPendingLevelBubbles();
    return side;
}

[[nodiscard]] std::expected<loglib::FilterExpression, CommandOutput> BuildFilter(
    const Options &options, const loglib::LogTable &table, const RuntimeContext &runtime
)
{
    const auto &columns = table.Configuration().Configuration().columns;
    loglib::FilterExpression::And combined;

    for (const std::string &query : options.filters)
    {
        auto parsed = loglib::ParseQuery(query);
        if (!parsed)
        {
            CommandOutput out;
            out.exitCode = static_cast<int>(ExitCode::Usage);
            out.err = FormatFilterParseDiagnostic(query, parsed.error().offset, parsed.error().message);
            out.err.push_back('\n');
            return std::unexpected(std::move(out));
        }
        combined.children.push_back(std::move(*parsed));
    }

    if (options.since || options.until)
    {
        const int timeIndex = FirstColumnOfType(columns, loglib::ColumnType::Time);
        if (timeIndex < 0)
        {
            return std::unexpected(Fail(
                ExitCode::Usage,
                std::format(
                    "'--since' and '--until' need a time column; available columns are {}.",
                    AvailableColumnList(columns)
                )
            ));
        }
        const loglib::Column &timeColumn = columns[static_cast<std::size_t>(timeIndex)];
        const auto now = std::chrono::system_clock::now();
        std::optional<std::int64_t> begin;
        std::optional<std::int64_t> end;
        auto parseBound = [&](std::string_view text,
                              std::string_view flag) -> std::expected<std::int64_t, CommandOutput> {
            const auto parsed = loglib::ParseUserTimestamp(text, timeColumn.parseFormats, now);
            if (!parsed)
            {
                return std::unexpected(
                    Fail(ExitCode::Usage, std::format("'{}' could not parse timestamp '{}'.", flag, text))
                );
            }
            std::int64_t micros = parsed->micros;
            if (parsed->isNaive)
            {
                micros = runtime.timeZone.LocalMicrosecondsToUtc(micros);
            }
            return micros;
        };
        if (options.since)
        {
            auto value = parseBound(*options.since, "--since");
            if (!value)
            {
                return std::unexpected(std::move(value.error()));
            }
            begin = *value;
        }
        if (options.until)
        {
            auto value = parseBound(*options.until, "--until");
            if (!value)
            {
                return std::unexpected(std::move(value.error()));
            }
            end = *value;
        }
        if (begin && end && *end < *begin)
        {
            return std::unexpected(Fail(ExitCode::Usage, "'--until' must not be earlier than '--since'."));
        }
        loglib::LeafRule rule;
        rule.type = loglib::LeafRule::Type::Time;
        rule.columnKeys = timeColumn.keys;
        rule.filterBegin = begin;
        rule.filterEnd = end;
        combined.children.push_back(loglib::FilterExpression{loglib::FilterExpression::Leaf{std::move(rule)}});
    }

    if (options.minLevel)
    {
        const int levelIndex = FirstColumnOfType(columns, loglib::ColumnType::Level);
        if (levelIndex < 0)
        {
            return std::unexpected(Fail(
                ExitCode::Usage,
                std::format(
                    "'--min-level' needs a level column; available columns are {}.", AvailableColumnList(columns)
                )
            ));
        }
        const auto level = loglib::ParseLevelName(*options.minLevel);
        if (!level || *level == loglib::LogLevel::Unknown)
        {
            return std::unexpected(Fail(
                ExitCode::Usage,
                std::format(
                    "'--min-level' expects trace, debug, info, warn, error, or fatal, not '{}'.", *options.minLevel
                )
            ));
        }
        loglib::LeafRule rule;
        rule.type = loglib::LeafRule::Type::Enumeration;
        rule.columnKeys = columns[static_cast<std::size_t>(levelIndex)].keys;
        for (int value = static_cast<int>(*level); value <= static_cast<int>(loglib::LogLevel::Fatal); ++value)
        {
            rule.filterValues.emplace_back(loglib::CanonicalLevelName(static_cast<loglib::LogLevel>(value)));
        }
        combined.children.push_back(loglib::FilterExpression{loglib::FilterExpression::Leaf{std::move(rule)}});
    }

    loglib::FilterExpression expression{std::move(combined)};
    if (options.invert)
    {
        expression = loglib::FilterExpression{loglib::FilterExpression::Not{std::move(expression)}};
    }
    return expression;
}

[[nodiscard]] std::expected<std::vector<std::size_t>, CommandOutput> ProjectColumns(
    const Options &options, const loglib::LogTable &table
)
{
    const auto &columns = table.Configuration().Configuration().columns;
    if (options.allColumns)
    {
        std::vector<std::size_t> indices(columns.size());
        for (std::size_t i = 0; i < columns.size(); ++i)
        {
            indices[i] = i;
        }
        return indices;
    }
    if (options.columns)
    {
        std::vector<std::size_t> indices;
        for (const std::string &name : SplitCsv(*options.columns))
        {
            const int index = FindColumnByName(columns, name);
            if (index < 0)
            {
                return std::unexpected(Fail(
                    ExitCode::Usage,
                    std::format(
                        "unresolvable column '{}'; available columns are {}.", name, AvailableColumnList(columns)
                    )
                ));
            }
            indices.push_back(static_cast<std::size_t>(index));
        }
        return indices;
    }
    return loglib::ColumnProjection(columns).Indices();
}

} // namespace

CommandOutput RunQuery(const Options &options, const RuntimeContext &runtime)
{
    CommandOutput out;
    if (!options.quiet && (options.config || options.saveSession || options.saveColumns))
    {
        out.err += FormatWarningLine("session configuration flags are not applied yet");
        out.err.push_back('\n');
    }

    loglib::LogConfigurationManager configuration;
    loglib::LogTable table{loglib::LogData{}, std::move(configuration)};
    std::vector<std::string> parseErrors;
    std::vector<std::shared_ptr<loglib::DecompressingByteSource>> ownedDecompressors;

    const auto parseStarted = std::chrono::steady_clock::now();
    CommandOutput loadSide = LoadSources(options, runtime, table, parseErrors, ownedDecompressors);
    out.err += loadSide.err;
    if (loadSide.exitCode != 0)
    {
        out.exitCode = loadSide.exitCode;
        return out;
    }
    const auto parseMs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - parseStarted).count()
    );
    if (options.verbose > 0)
    {
        out.err += FormatStageLine("parse", table.RowCount(), parseMs);
        out.err.push_back('\n');
    }

    auto filter = BuildFilter(options, table, runtime);
    if (!filter)
    {
        out.err += filter.error().err;
        out.exitCode = filter.error().exitCode;
        return out;
    }

    const auto filterStarted = std::chrono::steady_clock::now();
    const auto compiled = loglib::CompileExpression(*filter, table.Configuration().Configuration().columns, &table);
    std::vector<std::size_t> rows = loglib::FilterAcceptedRows(table, compiled);
    const auto filterMs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - filterStarted).count()
    );
    if (options.verbose > 0)
    {
        out.err += FormatStageLine("filter", rows.size(), filterMs);
        out.err.push_back('\n');
    }

    if (options.sort)
    {
        std::string_view sortSpec = *options.sort;
        loglib::SortDirection direction = loglib::SortDirection::Ascending;
        const std::size_t colon = sortSpec.rfind(':');
        if (colon != std::string_view::npos)
        {
            const std::string_view dir = sortSpec.substr(colon + 1);
            if (EqualsIgnoreCase(dir, "asc"))
            {
                direction = loglib::SortDirection::Ascending;
                sortSpec = sortSpec.substr(0, colon);
            }
            else if (EqualsIgnoreCase(dir, "desc"))
            {
                direction = loglib::SortDirection::Descending;
                sortSpec = sortSpec.substr(0, colon);
            }
        }
        const auto &columns = table.Configuration().Configuration().columns;
        const int columnIndex = FindColumnByName(columns, sortSpec);
        if (columnIndex < 0)
        {
            return Fail(
                ExitCode::Usage,
                std::format(
                    "unresolvable sort column '{}'; available columns are {}.", sortSpec, AvailableColumnList(columns)
                )
            );
        }
        std::optional<loglib::EnumDictRank> rankStorage;
        const loglib::EnumDictRank *rank = nullptr;
        if (columns[static_cast<std::size_t>(columnIndex)].type == loglib::ColumnType::Enumeration)
        {
            const auto lookup = table.ResolveEnumColumn(static_cast<std::size_t>(columnIndex));
            if (lookup.dictionary != nullptr)
            {
                rankStorage.emplace(*lookup.dictionary);
                rank = &*rankStorage;
            }
        }
        const loglib::RowOrdering ordering(static_cast<std::size_t>(columnIndex), direction, rank);
        rows = ordering.Permute(table, std::span<const std::size_t>{rows});
    }

    if (options.last)
    {
        if (rows.size() > *options.last)
        {
            rows.erase(rows.begin(), rows.end() - static_cast<std::ptrdiff_t>(*options.last));
        }
    }
    else
    {
        std::size_t start = 0;
        if (options.offset)
        {
            start = static_cast<std::size_t>(std::min<std::uint64_t>(*options.offset, rows.size()));
        }
        if (start > 0)
        {
            rows.erase(rows.begin(), rows.begin() + static_cast<std::ptrdiff_t>(start));
        }
        if (options.limit && rows.size() > *options.limit)
        {
            rows.resize(static_cast<std::size_t>(*options.limit));
        }
    }

    if (!parseErrors.empty())
    {
        out.err += FormatErrorLine(
            {}, std::format("{} lines could not be parsed (use --verbose to list)", parseErrors.size())
        );
        out.err.push_back('\n');
        if (options.verbose > 0)
        {
            for (const std::string &error : parseErrors)
            {
                out.err += FormatWarningLine(error);
                out.err.push_back('\n');
            }
        }
    }

    if (options.count)
    {
        out.out = std::to_string(rows.size());
        out.out.push_back('\n');
        if (options.strict && !parseErrors.empty())
        {
            out.exitCode = static_cast<int>(ExitCode::Data);
            return out;
        }
        out.exitCode = rows.empty() && !options.allowEmpty ? static_cast<int>(ExitCode::Empty)
                                                           : static_cast<int>(ExitCode::Success);
        return out;
    }

    auto visible = ProjectColumns(options, table);
    if (!visible)
    {
        out.err += visible.error().err;
        out.exitCode = visible.error().exitCode;
        return out;
    }

    const auto exportFormat = ToExportFormat(options.outputFormat);
    if (!exportFormat)
    {
        return Fail(
            ExitCode::Usage,
            std::format("'--output-format' expects jsonl, csv, markdown, or snapshot, not '{}'.", options.outputFormat)
        );
    }

    if (*exportFormat != loglib::exports::ExportFormat::Csv &&
        *exportFormat != loglib::exports::ExportFormat::Markdown && options.noHeader)
    {
        out.err += FormatWarningLine("'--no-header' has no effect for this output format");
        out.err.push_back('\n');
    }

    loglib::exports::ExportPlan plan;
    plan.format = *exportFormat;
    plan.sourceRows = std::move(rows);
    plan.visibleColumns = std::move(*visible);
    plan.includeAllFieldsForJson = !options.jsonVisibleOnly;
    plan.includeHeaderRow = !options.noHeader;
    plan.table = &table;

    auto exporter = loglib::exports::MakeExporter(plan.format);
    if (!exporter)
    {
        return Fail(ExitCode::Usage, "unsupported export format.");
    }

    const bool empty = plan.sourceRows.empty();
    const bool toStdout = options.output == "-" || options.output.empty();
    if (!toStdout)
    {
        std::error_code ec;
        if (std::filesystem::exists(options.output, ec) && !options.force)
        {
            return Fail(
                ExitCode::Output, std::format("output '{}' already exists; pass --force to overwrite.", options.output)
            );
        }
        loglib::exports::FileSink sink(options.output);
        exporter->Run(plan.View(), sink, runtime.stop);
        sink.Finish();
    }
    else
    {
        // Write through StreamSink so a closed pipe surfaces as BrokenPipe
        // and large results are not buffered in memory. Execute must not
        // reprint stdout for query when `out` is empty.
        loglib::exports::StreamSink sink(stdout);
        exporter->Run(plan.View(), sink, runtime.stop);
        sink.Finish();
    }

    if (options.strict && !parseErrors.empty())
    {
        out.exitCode = static_cast<int>(ExitCode::Data);
        return out;
    }
    out.exitCode =
        empty && !options.allowEmpty ? static_cast<int>(ExitCode::Empty) : static_cast<int>(ExitCode::Success);
    return out;
}

} // namespace slp
