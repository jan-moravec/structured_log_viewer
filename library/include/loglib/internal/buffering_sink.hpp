#pragma once

#include "loglib/key_index.hpp"
#include "loglib/log_data.hpp"
#include "loglib/log_file.hpp"
#include "loglib/streaming_log_sink.hpp"

#include <memory>
#include <string>
#include <vector>

namespace loglib::internal
{

/// `StreamingLogSink` adapter behind the synchronous `LogParser::Parse(path)`
/// overload: accumulates every batch into a single `LogData`. One sink per parse.
///
/// Lives in `loglib::internal` because nothing outside the loglib library
/// (or its unit tests) should construct one directly — synchronous callers
/// reach it through `LogParser::Parse(path)`, streaming callers reach it
/// through their own sink. The header sits under `library/include/loglib/
/// internal/` so unit tests can reuse the same `loglib`-prefixed include
/// path; the namespace pins the "private" intent.
class BufferingSink : public StreamingLogSink
{
public:
    /// Takes ownership of @p logFile and routes batches into an internal `KeyIndex`.
    explicit BufferingSink(std::unique_ptr<LogFile> logFile);

    KeyIndex &Keys() override;

    void OnStarted() override;
    void OnBatch(StreamedBatch batch) override;
    void OnFinished(bool cancelled) override;

    /// We re-buffer batches anyway, so opt out of the harness's coalescing.
    [[nodiscard]] bool PrefersUncoalesced() const noexcept override
    {
        return true;
    }

    /// Call exactly once after `OnFinished`.
    LogData TakeData();
    std::vector<std::string> TakeErrors();

private:
    std::unique_ptr<LogFile> mFile;
    KeyIndex mKeys;
    std::vector<LogLine> mLines;
    std::vector<uint64_t> mLineOffsets;
    std::vector<std::string> mErrors;
    bool mFinished = false;
};

} // namespace loglib::internal
