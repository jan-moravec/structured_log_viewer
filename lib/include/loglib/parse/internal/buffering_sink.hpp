/**
 * @brief Sink that collects everything into a LogData.
 *
 * BufferingSink backs the synchronous ParseFile helpers. It appends every batch to one LogData and keeps the errors
 * until the caller takes them.
 *
 * This is part of the parse module.
 */
#pragma once

#include "loglib/core/file_line_source.hpp"
#include "loglib/core/key_index.hpp"
#include "loglib/core/log_data.hpp"
#include "loglib/parse/log_parse_sink.hpp"

#include <memory>
#include <string>
#include <vector>

namespace loglib::internal
{

/**
 * @brief A sink that collects every batch of one parse into a single LogData. Use one sink for each parse. ParseFile
 * uses it.
 */
class BufferingSink : public LogParseSink
{
public:
    /**
     * @brief Takes ownership of the source. Its LogFile holds the decoded strings, and the line offsets arrive with
     * the batches.
     */
    explicit BufferingSink(std::unique_ptr<FileLineSource> source);

    KeyIndex &Keys() override;

    void OnStarted() override;
    void OnBatch(RowBatch batch) override;
    void OnFinished(bool cancelled) override;

    /** @brief Returns the collected data. Call it once, after OnFinished. */
    LogData TakeData();
    /** @brief Returns the collected error messages. */
    std::vector<std::string> TakeErrors();

private:
    std::unique_ptr<FileLineSource> mSource;
    KeyIndex mKeys;
    std::vector<LogLine> mLines;
    std::vector<uint64_t> mLineOffsets;
    std::vector<MultiLineRecordSpan> mMultiLineSpans;
    std::vector<std::string> mErrors;
    bool mFinished = false;
};

} // namespace loglib::internal
