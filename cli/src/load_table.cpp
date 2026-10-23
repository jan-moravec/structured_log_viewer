#include "load_table.hpp"

#include "console.hpp"
#include "session_config.hpp"
#include "text_util.hpp"

#include <loglib/auto_detect_parser.hpp>
#include <loglib/file_line_source.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_file.hpp>
#include <loglib/log_parse_sink.hpp>
#include <loglib/parse_error_text.hpp>
#include <loglib/regex_templates.hpp>
#include <loglib/session_bundle.hpp>
#include <loglib/source_plan.hpp>
#include <loglib/stdin_bytes_producer.hpp>
#include <loglib/stdin_peek.hpp>
#include <loglib/stream_line_source.hpp>
#include <loglib/user_paths.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <sstream>
#include <system_error>
#include <utility>

namespace slp
{

namespace
{

[[nodiscard]] Failure LoadFail(ExitCode code, std::string_view sentence)
{
    return MakeFailure(code, {}, sentence);
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

[[nodiscard]] std::filesystem::path TemplateDirectory(const GlobalOptions &global)
{
    return global.templateDir ? *global.templateDir : loglib::UserRegexTemplateDirectory();
}

/** @brief The pattern named by `--regex` or `--regex-template`. */
[[nodiscard]] std::expected<std::string, Failure> ResolveRegexPattern(
    const GlobalOptions &global, const InputOptions &input
)
{
    if (input.regex)
    {
        return *input.regex;
    }
    if (!input.regexTemplate)
    {
        return std::unexpected(LoadFail(ExitCode::Usage, "'--format regex' requires '--regex' or '--regex-template'."));
    }

    const std::string &slug = *input.regexTemplate;
    for (const auto &entry : loglib::BuiltinRegexTemplateCatalog())
    {
        if (EqualsIgnoreCase(entry.slug, slug))
        {
            return entry.tmpl.pattern;
        }
    }

    const std::filesystem::path templateDir = TemplateDirectory(global);
    if (!templateDir.empty())
    {
        const std::filesystem::path candidate = templateDir / (slug + ".json");
        const std::ifstream file(candidate, std::ios::binary);
        if (file)
        {
            std::ostringstream buffer;
            buffer << file.rdbuf();
            try
            {
                return loglib::ParseRegexTemplate(buffer.str()).pattern;
            }
            catch (const std::exception &ex)
            {
                return std::unexpected(LoadFail(
                    ExitCode::Usage,
                    std::format("could not parse regex template '{}': {}.", candidate.string(), ex.what())
                ));
            }
        }
        for (const auto &tmpl : loglib::LoadRegexTemplatesFromDirectory(templateDir).templates)
        {
            if (EqualsIgnoreCase(tmpl.name, slug))
            {
                return tmpl.pattern;
            }
        }
    }

    return std::unexpected(LoadFail(ExitCode::Usage, std::format("unknown regex template slug '{}'.", slug)));
}

/** @brief Feeds parsed batches into the table and records parse errors and line coverage. */
class TableSink final : public loglib::LogParseSink
{
public:
    explicit TableSink(LoadedTable &loaded) noexcept
        : mLoaded(loaded)
    {
    }

    loglib::KeyIndex &Keys() override
    {
        return mLoaded.table.Keys();
    }

    void OnStarted() override
    {
    }

    void OnBatch(loglib::StreamedBatch batch) override
    {
        std::uint64_t &consumed = mLoaded.linesConsumed;
        if (batch.firstLineNumber != 0)
        {
            consumed = std::max(consumed, static_cast<std::uint64_t>(batch.firstLineNumber));
        }
        for (const loglib::LogLine &line : batch.lines)
        {
            consumed = std::max(consumed, static_cast<std::uint64_t>(line.LineId()) + std::uint64_t{1});
        }
        for (std::string &error : batch.errors)
        {
            consumed = std::max(consumed, loglib::LocateParseError(error).lastLine);
            mLoaded.parseErrors.push_back(std::move(error));
        }
        batch.errors.clear();
        mLoaded.table.AppendBatchArranged(std::move(batch));
    }

    void OnFinished(bool /*cancelled*/) override
    {
    }

private:
    LoadedTable &mLoaded;
};

/**
 * @brief How one source chooses its parser format.
 *
 * `accepts` checks a pinned `--format` against the source; `detect`
 * picks a format when none is pinned.
 */
struct FormatProbe
{
    std::string_view label;
    std::function<bool(const loglib::LogParser &)> accepts;
    std::function<loglib::DetectedFormat()> detect;
};

/** @brief Loads sources one at a time into one table, in the order given. */
class TableLoader
{
public:
    TableLoader(
        const GlobalOptions &global, const InputOptions &input, const Console &console, const RuntimeContext &runtime
    )
        : mGlobal(global), mInput(input), mConsole(console), mRuntime(runtime)
    {
    }

    /** @brief Seeds the configuration from `--config`. */
    [[nodiscard]] std::optional<Failure> SeedConfig()
    {
        if (!mInput.config)
        {
            return std::nullopt;
        }
        auto configuration = LoadConfigFile(*mInput.config);
        if (!configuration)
        {
            return LoadFail(ExitCode::Input, configuration.error());
        }
        mLoaded.table.Configuration().SetConfiguration(std::move(*configuration));
        return std::nullopt;
    }

    /** @brief Loads plain paths, expanded with their rotated siblings. */
    [[nodiscard]] std::optional<Failure> AddPaths(const std::vector<std::filesystem::path> &paths)
    {
        if (paths.empty())
        {
            return std::nullopt;
        }
        loglib::SourcePlanOptions planOptions;
        planOptions.rotationHistory = !mGlobal.noRotationHistory;
        const auto plan = loglib::ResolveSourcePlan(paths, planOptions);
        if (mConsole.Verbose() > 0)
        {
            for (const auto &entry : plan)
            {
                mConsole.Warn("loading " + entry.path.string() + (entry.autoAdded ? " (rotated sibling)" : ""));
            }
        }
        for (const auto &entry : plan)
        {
            if (auto failure = AddFile(entry.path, false))
            {
                return failure;
            }
        }
        return std::nullopt;
    }

    /** @brief Loads one `.slvbundle`, seeding the configuration from it unless `--config` was given. */
    [[nodiscard]] std::optional<Failure> AddBundle(const std::filesystem::path &path)
    {
        return AddFile(path, true);
    }

    /** @brief Loads standard input to its end. */
    [[nodiscard]] std::optional<Failure> AddStdin()
    {
        if (loglib::IsStdinInteractive())
        {
            return LoadFail(
                ExitCode::Input, "standard input is a terminal; pipe log data into slp or redirect from a file"
            );
        }

        constexpr auto PEEK_TIMEOUT = std::chrono::milliseconds(500);
        std::string peek = loglib::StdinPeek(loglib::PROBE_BYTES_BUDGET, PEEK_TIMEOUT);
        const std::string_view sniff = peek;
        auto format = ResolveFormat(
            FormatProbe{
                .label = "-",
                .accepts = [sniff](const loglib::LogParser &parser) { return parser.IsValidBytes(sniff); },
                .detect = [sniff] { return loglib::DetectFormatFromBytes(sniff); },
            }
        );
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
            return LoadFail(ExitCode::Input, std::format("failed to attach to stdin: {}", ex.what()));
        }

        auto lineSource = std::make_unique<loglib::StreamLineSource>(std::filesystem::path{"-"}, std::move(producer));
        loglib::StreamLineSource &source = *lineSource;
        Attach(std::move(lineSource));

        loglib::ParserOptions parserOptions = MakeParserOptions(mGlobal, mInput, mRuntime.stop);
        parserOptions.initialCarry = std::move(peek);
        try
        {
            Parse(*format, source, parserOptions);
        }
        catch (const std::exception &ex)
        {
            return LoadFail(ExitCode::Input, std::format("standard input: {}", ex.what()));
        }
        return StopCheck();
    }

    /** @brief Completes deferred type detection and returns the loaded table. */
    [[nodiscard]] LoadedTable Finish()
    {
        mLoaded.table.FinalizeAutoDetection();
        mLoaded.table.ApplyPendingLevelBubbles();
        return std::move(mLoaded);
    }

private:
    [[nodiscard]] std::optional<Failure> AddFile(const std::filesystem::path &displayPath, bool bundle)
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
            decompressOptions.discardFirstLine = bundle;
            auto decompressor = std::make_shared<loglib::DecompressingByteSource>(
                displayPath, loglib::DecompressingByteSource::ProgressCallback{}, mRuntime.stop, decompressOptions
            );
            if (bundle)
            {
                const auto metadata = loglib::ParseSessionBundleMetadata(decompressor->DiscardedFirstLine());
                // Seed the embedded session before the first BeginStreaming so
                // column pins and filter defaults apply.
                if (mFirst && !mInput.config)
                {
                    mLoaded.table.Configuration().SetConfiguration(metadata.configuration);
                }
            }

            const std::filesystem::path effective = decompressor->EffectivePath();
            mLoaded.ownedDecompressors.push_back(decompressor);

            const std::string label = displayPath.string();
            auto format = ResolveFormat(
                FormatProbe{
                    .label = label,
                    .accepts = [&effective](const loglib::LogParser &parser) { return parser.IsValid(effective); },
                    .detect = [&effective] { return loglib::DetectFormatForPath(effective); },
                }
            );
            if (!format)
            {
                return std::move(format.error());
            }

            auto logFile = std::make_unique<loglib::LogFile>(effective, displayPath);
            logFile->AttachLifetimeAnchor(decompressor);
            auto lineSource = std::make_unique<loglib::FileLineSource>(std::move(logFile));
            loglib::FileLineSource &source = *lineSource;
            Attach(std::move(lineSource));
            Parse(*format, source, MakeParserOptions(mGlobal, mInput, mRuntime.stop));
        }
        catch (const loglib::DecompressionCancelled &)
        {
            return CancelledFailure();
        }
        catch (const loglib::DecompressionSizeCapExceeded &ex)
        {
            return LoadFail(ExitCode::Input, ex.what());
        }
        catch (const loglib::SessionBundleVersionError &ex)
        {
            return LoadFail(ExitCode::Input, ex.what());
        }
        catch (const loglib::SessionBundleReadError &ex)
        {
            return LoadFail(ExitCode::Input, ex.what());
        }
        catch (const std::exception &ex)
        {
            return OpenFailure(displayPath, ex.what());
        }
        return StopCheck();
    }

    /**
     * @brief Resolves the parser format for one source.
     *
     * A pinned `--format` must accept the probe; otherwise the probe detects it.
     */
    [[nodiscard]] std::expected<loglib::DetectedFormat, Failure> ResolveFormat(const FormatProbe &probe)
    {
        loglib::DetectedFormat resolved;
        if (const auto pinned = ToSourceFormat(mInput.format))
        {
            resolved.format = *pinned;
            if (*pinned == loglib::Source::Format::Regex)
            {
                auto pattern = ResolveRegexPattern(mGlobal, mInput);
                if (!pattern)
                {
                    return std::unexpected(std::move(pattern.error()));
                }
                resolved.regexPattern = std::move(*pattern);
            }
            const auto parser = loglib::MakeParserForFormat(resolved.format, resolved.regexPattern);
            if (!probe.accepts(*parser))
            {
                return std::unexpected(LoadFail(
                    ExitCode::Input,
                    std::format("source '{}' was rejected by the {} parser.", probe.label, FormatName(*pinned))
                ));
            }
        }
        else
        {
            resolved = probe.detect();
            if (resolved.format == loglib::Source::Format::Regex && resolved.regexPattern.empty())
            {
                return std::unexpected(
                    LoadFail(ExitCode::Input, std::format("could not detect a parser for source '{}'.", probe.label))
                );
            }
        }
        if (!mLoaded.format)
        {
            mLoaded.format = resolved;
        }
        return resolved;
    }

    template <typename Source> void Attach(std::unique_ptr<Source> source)
    {
        if (mFirst)
        {
            mLoaded.table.BeginStreaming(std::move(source));
            mFirst = false;
        }
        else
        {
            mLoaded.table.AppendStreaming(std::move(source));
        }
    }

    template <typename Source>
    void Parse(const loglib::DetectedFormat &format, Source &source, const loglib::ParserOptions &parserOptions)
    {
        const auto parser = loglib::MakeParserForFormat(format.format, format.regexPattern);
        TableSink sink(mLoaded);
        parser->ParseStreaming(source, sink, parserOptions);
    }

    [[nodiscard]] std::optional<Failure> StopCheck() const
    {
        if (mRuntime.stop.stop_requested())
        {
            return CancelledFailure();
        }
        return std::nullopt;
    }

    [[nodiscard]] static Failure OpenFailure(const std::filesystem::path &path, std::string_view reason)
    {
        return LoadFail(ExitCode::Input, std::format("source '{}': {}", path.string(), reason));
    }

    const GlobalOptions &mGlobal;
    const InputOptions &mInput;
    const Console &mConsole;
    const RuntimeContext &mRuntime;
    LoadedTable mLoaded;
    bool mFirst = true;
};

} // namespace

std::string_view FormatName(loglib::Source::Format format) noexcept
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

std::expected<LoadedTable, Failure> LoadTable(
    const GlobalOptions &global, const InputOptions &input, const Console &console, const RuntimeContext &runtime
)
{
    LoadUserTemplates(global, console);
    if (input.sources.empty())
    {
        return std::unexpected(LoadFail(ExitCode::Usage, "at least one source is required."));
    }

    TableLoader loader(global, input, console, runtime);
    if (auto failure = loader.SeedConfig())
    {
        return std::unexpected(std::move(*failure));
    }

    // Consecutive plain paths are planned together so rotation siblings
    // expand and deduplicate across them.
    std::vector<std::filesystem::path> pendingPaths;
    auto flushPaths = [&]() {
        auto failure = loader.AddPaths(pendingPaths);
        pendingPaths.clear();
        return failure;
    };

    for (const SourceSpec &source : input.sources)
    {
        std::optional<Failure> failure;
        switch (source.kind)
        {
        case SourceKind::Path:
            pendingPaths.push_back(source.path);
            continue;
        case SourceKind::Stdin:
            failure = flushPaths();
            if (!failure)
            {
                failure = loader.AddStdin();
            }
            break;
        case SourceKind::Bundle:
            failure = flushPaths();
            if (!failure)
            {
                failure = loader.AddBundle(source.path);
            }
            break;
        case SourceKind::Tcp:
        case SourceKind::Udp:
            failure = LoadFail(
                ExitCode::Usage, std::format("Network source '{}' is only valid for `slp tail`.", source.text)
            );
            break;
        }
        if (failure)
        {
            return std::unexpected(std::move(*failure));
        }
    }
    if (auto failure = flushPaths())
    {
        return std::unexpected(std::move(*failure));
    }
    return loader.Finish();
}

void LoadUserTemplates(const GlobalOptions &global, const Console &console)
{
    const std::filesystem::path dir = TemplateDirectory(global);
    if (dir.empty())
    {
        return;
    }
    const auto loaded = loglib::LoadRegexTemplatesFromDirectory(dir);
    for (const std::string &error : loaded.errors)
    {
        console.Warn(error);
    }
    if (!loaded.templates.empty())
    {
        loglib::SetExtraRegexTemplates(loaded.templates);
    }
}

std::expected<std::unique_ptr<loglib::LogParser>, Failure> MakeStreamParser(
    const GlobalOptions &global, const InputOptions &input
)
{
    const auto pinned = ToSourceFormat(input.format);
    if (!pinned)
    {
        return std::make_unique<loglib::AutoDetectParser>();
    }
    std::string pattern;
    if (*pinned == loglib::Source::Format::Regex)
    {
        auto resolved = ResolveRegexPattern(global, input);
        if (!resolved)
        {
            return std::unexpected(std::move(resolved.error()));
        }
        pattern = std::move(*resolved);
    }
    return loglib::MakeParserForFormat(*pinned, pattern);
}

loglib::ParserOptions MakeParserOptions(
    const GlobalOptions &global, const InputOptions &input, const loglib::StopToken &stop
)
{
    loglib::ParserOptions parserOptions;
    parserOptions.stopToken = stop;
    parserOptions.threads = global.threads;
    parserOptions.multilineLogfmt = !input.noMultilineLogfmt;
    if (input.batchSizeBytes)
    {
        parserOptions.batchSizeBytes = static_cast<std::size_t>(*input.batchSizeBytes);
    }
    return parserOptions;
}

} // namespace slp
