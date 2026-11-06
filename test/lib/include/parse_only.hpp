/**
 * @brief Parses a whole file and returns the raw parser output, with no table in between.
 *
 * Parser unit tests need to see what a parser produces: its rows, keys and errors, before a table decides column types.
 * Through a table a key named timestamp becomes a time column and its values change type, which would hide what the
 * parser did. ParseFile runs a parser over a file with default options and collects everything into a LogData.
 * BufferingSink is the sink it uses, for tests that drive a parser themselves. Tests that need a table use LoadFile
 * instead.
 *
 * This is a test helper. It is not part of the library.
 */
#pragma once

#include "loglib/core/file_line_source.hpp"
#include "loglib/core/key_index.hpp"
#include "loglib/core/log_data.hpp"
#include "loglib/parse/log_parse_sink.hpp"
#include "loglib/parse/log_parser.hpp"

#include <filesystem>
#include <memory>
#include <vector>

namespace loglib
{

/** @brief Buffered output of a synchronous parse. */
struct ParseResult
{
    LogData data;
    std::vector<ParseError> errors;
};

namespace internal
{

/**
 * @brief A sink that collects every batch of one parse into a single LogData. Use one sink for each parse.
 */
class BufferingSink : public LogParseSink
{
public:
    /** @brief Takes ownership of the source. The line offsets arrive with the batches and are applied by TakeData. */
    explicit BufferingSink(std::unique_ptr<FileLineSource> source);

    KeyIndex &Keys() override;

    void OnStarted() override;
    void OnBatch(RowBatch batch) override;
    void OnFinished(bool cancelled) override;

    /** @brief Returns the collected data. Call it once, after OnFinished. */
    LogData TakeData();
    /** @brief Returns the collected errors. */
    std::vector<ParseError> TakeErrors();

private:
    std::unique_ptr<FileLineSource> mSource;
    KeyIndex mKeys;
    std::vector<LogLine> mLines;
    std::vector<uint64_t> mLineOffsets;
    std::vector<MultiLineRecordSpan> mMultiLineSpans;
    std::vector<ParseError> mErrors;
    bool mFinished = false;
};

} // namespace internal

/**
 * @brief Parses a whole file with the given parser and returns everything it found.
 *
 * Throws std::runtime_error if the file does not exist or is empty.
 */
[[nodiscard]] ParseResult ParseFile(const LogParser &parser, const std::filesystem::path &file);

/**
 * @brief Detects the format of a file as LoadFile does and parses it with the matching parser.
 *
 * Throws std::runtime_error if the file cannot be read, is empty, or no parser recognizes it.
 */
[[nodiscard]] ParseResult ParseFile(const std::filesystem::path &file);

} // namespace loglib
