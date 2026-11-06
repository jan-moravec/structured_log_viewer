#include "loglib/output/file_loading.hpp"

#include "loglib/core/error.hpp"
#include "loglib/core/internal/path_encoding.hpp"
#include "loglib/core/log_file.hpp"
#include "loglib/parse/format_detection.hpp"
#include "loglib/parse/log_parser.hpp"
#include "loglib/parse/parsers/json_parser.hpp"
#include "loglib/table/table_loader.hpp"

#include <fmt/format.h>

#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

namespace loglib
{

OpenedFile OpenFileSource(const std::filesystem::path &path, const OpenFileOptions &options)
{
    const bool isBundle = HasSessionBundleExtension(path);

    DecompressingByteSource::Options decompressOptions;
    decompressOptions.discardFirstLine = isBundle;
    decompressOptions.maxDecompressedBytes = options.maxDecompressedBytes;
    auto decompressor =
        std::make_shared<DecompressingByteSource>(path, options.progress, options.stopToken, decompressOptions);

    OpenedFile opened;
    if (isBundle)
    {
        opened.bundle = ParseSessionBundleMetadata(decompressor->DiscardedFirstLine());
    }

    opened.effectivePath = decompressor->EffectivePath();
    auto logFile = std::make_unique<LogFile>(opened.effectivePath, path);
    // The file is unmapped first and the temporary copy is deleted after it.
    logFile->AttachLifetimeAnchor(std::move(decompressor));
    opened.source = std::make_unique<FileLineSource>(std::move(logFile));
    return opened;
}

namespace
{

// The format detection returns JSON both for JSON and when nothing matched, so JSON is checked once more here. Unknown
// content is then reported instead of being pushed through the JSON parser.
std::unique_ptr<LogParser> ParserForHead(const std::string &head, const std::filesystem::path &file)
{
    if (head.empty())
    {
        throw FormatError(fmt::format("Input file '{}' could not be parsed.", internal::PathToUtf8(file)));
    }

    const DetectedFormat detected = DetectFormatFromBytes(head);
    if (detected.format == Source::Format::Json)
    {
        const JsonParser probe;
        if (!probe.IsValidBytes(head))
        {
            throw FormatError(fmt::format("Input file '{}' could not be parsed.", internal::PathToUtf8(file)));
        }
    }
    return MakeParserForFormat(detected.format, detected.Options());
}

} // namespace

LoadResult LoadFile(const std::filesystem::path &file, LoadOptions options)
{
    std::error_code ec;
    if (!std::filesystem::exists(file, ec) || ec)
    {
        throw IoError(fmt::format("File '{}' does not exist.", internal::PathToUtf8(file)));
    }
    if (std::filesystem::is_regular_file(file, ec) && std::filesystem::file_size(file, ec) == 0)
    {
        throw IoError(fmt::format("File '{}' is empty.", internal::PathToUtf8(file)));
    }

    OpenedFile opened = OpenFileSource(file, OpenFileOptions{.stopToken = options.stopToken});

    LoadResult result;
    result.bundle = opened.bundle;
    result.table.SetDisplayTimeZone(options.displayTimeZone);
    result.table.SetDefaultSourceTimeZone(options.defaultSourceTimeZone);
    if (options.configuration)
    {
        result.table.Configuration().SetConfiguration(std::move(*options.configuration));
    }
    else if (opened.bundle)
    {
        result.table.Configuration().SetConfiguration(opened.bundle->configuration);
    }
    result.warnings = result.table.Configuration().TakeWarnings();

    // The rows of a bundle are normalized JSON, however many there are.
    std::unique_ptr<LogParser> parser;
    if (opened.bundle)
    {
        parser = std::make_unique<JsonParser>();
    }
    else
    {
        parser = ParserForHead(ReadProbeHead(opened.effectivePath), file);
    }

    ParseOutcome outcome = ParseIntoTable(
        result.table, *parser, std::move(opened.source), ParserOptionsFor(result.table, options.stopToken)
    );
    if (outcome.cancelled)
    {
        throw OperationCancelled("Loading cancelled");
    }
    FinishLoading(result.table);
    result.errors = std::move(outcome.errors);
    return result;
}

} // namespace loglib
