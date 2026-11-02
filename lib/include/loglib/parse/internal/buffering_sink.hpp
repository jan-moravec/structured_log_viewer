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
 * LogParseSink adapter behind the synchronous LogParser::Parse(path)
 * overload: accumulates every batch into a single LogData. One sink per parse.
 */
class BufferingSink : public LogParseSink
{
public:
    /**
     * Takes ownership of @p source. The source's LogFile is the
     * arena for OwnedString payload concatenation; line offsets are
     * pushed in via OnBatch.
     */
    explicit BufferingSink(std::unique_ptr<FileLineSource> source);

    KeyIndex &Keys() override;

    void OnStarted() override;
    void OnBatch(StreamedBatch batch) override;
    void OnFinished(bool cancelled) override;

    /** Call exactly once after OnFinished. */
    LogData TakeData();
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
