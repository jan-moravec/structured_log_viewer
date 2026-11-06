
#include "parse_only.hpp"

#include "loglib/core/internal/path_encoding.hpp"
#include "loglib/parse/format_detection.hpp"
#include "loglib/parse/parser_options.hpp"
#include "loglib/parse/parsers/json_parser.hpp"

#include <fmt/format.h>

#include <filesystem>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace loglib
{

namespace internal
{

BufferingSink::BufferingSink(std::unique_ptr<FileLineSource> source)
    : mSource(std::move(source))
{
}

void BufferingSink::OnStarted()
{
    mLines.clear();
    mLineOffsets.clear();
    mMultiLineSpans.clear();
    mErrors.clear();
    mFinished = false;
}

void BufferingSink::OnBatch(RowBatch batch)
{
    // Avoid reserve(size + n): some STL implementations take it as exact and make this quadratic. insert grows
    // geometrically.
    if (!batch.lines.empty())
    {
        mLines.insert(
            mLines.end(), std::make_move_iterator(batch.lines.begin()), std::make_move_iterator(batch.lines.end())
        );
    }
    if (!batch.localLineOffsets.empty())
    {
        mLineOffsets.insert(
            mLineOffsets.end(),
            std::make_move_iterator(batch.localLineOffsets.begin()),
            std::make_move_iterator(batch.localLineOffsets.end())
        );
    }
    if (!batch.multiLineSpans.empty())
    {
        mMultiLineSpans.insert(
            mMultiLineSpans.end(),
            std::make_move_iterator(batch.multiLineSpans.begin()),
            std::make_move_iterator(batch.multiLineSpans.end())
        );
    }
    if (!batch.errors.empty())
    {
        mErrors.insert(
            mErrors.end(), std::make_move_iterator(batch.errors.begin()), std::make_move_iterator(batch.errors.end())
        );
    }
}

void BufferingSink::OnFinished(bool /*cancelled*/)
{
    mFinished = true;
}

KeyIndex &BufferingSink::Keys()
{
    return mKeys;
}

LogData BufferingSink::TakeData()
{
    // Register spans after offsets so widened line lookups can resolve.
    if (mSource)
    {
        if (!mLineOffsets.empty())
        {
            mSource->AppendLineOffsets(mLineOffsets);
            mLineOffsets.clear();
        }
        if (!mMultiLineSpans.empty())
        {
            for (const auto &span : mMultiLineSpans)
            {
                mSource->RegisterMultiLineRecord(span.headerLineId, span.lastLineId);
            }
            mMultiLineSpans.clear();
        }
    }
    return {std::move(mSource), std::move(mLines), std::move(mKeys)};
}

std::vector<ParseError> BufferingSink::TakeErrors()
{
    return std::move(mErrors);
}

} // namespace internal

// MSVC's <filesystem> casts a combined bitmask back to __std_fs_stats_flags (for example _Follow_symlinks | _File_size
// = 9), and clang's analyzer flags the resulting value as out-of-range. False positive originating in stdlib.
// NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange)
ParseResult ParseFile(const LogParser &parser, const std::filesystem::path &file)
{
    if (!std::filesystem::exists(file))
    {
        throw std::runtime_error(fmt::format("File '{}' does not exist.", internal::PathToUtf8(file)));
    }
    if (std::filesystem::file_size(file) == 0)
    {
        throw std::runtime_error(fmt::format("File '{}' is empty.", internal::PathToUtf8(file)));
    }

    // The sink owns the FileLineSource for the duration of the parse. TakeData moves it into the LogData, so the
    // back-pointers of the rows stay valid.
    auto fileSource = std::make_unique<FileLineSource>(file);
    FileLineSource *sourceRaw = fileSource.get();
    internal::BufferingSink sink(std::move(fileSource));

    parser.ParseStreaming(*sourceRaw, sink, ParserOptions{});

    LogData data = sink.TakeData();
    std::vector<ParseError> errors = sink.TakeErrors();
    return ParseResult{.data = std::move(data), .errors = std::move(errors)};
}

ParseResult ParseFile(const std::filesystem::path &file)
{
    const std::string head = ReadProbeHead(file, PROBE_BYTES_BUDGET);
    if (head.empty())
    {
        throw std::runtime_error(fmt::format("Input file '{}' could not be parsed.", internal::PathToUtf8(file)));
    }

    const DetectedFormat detected = DetectFormatFromBytes(head);
    // DetectFormatFromBytes returns JSON both for a JSON match and for nothing matched, so check JSON again.
    if (detected.format == Source::Format::Json)
    {
        const JsonParser probe;
        if (!probe.IsValidBytes(head))
        {
            throw std::runtime_error(fmt::format("Input file '{}' could not be parsed.", internal::PathToUtf8(file)));
        }
    }

    const std::unique_ptr<LogParser> parser = MakeParserForFormat(detected.format, detected.Options());
    return ParseFile(*parser, file);
}
// NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange)

} // namespace loglib
