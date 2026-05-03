#pragma once

#include "loglib/file_line_source.hpp"
#include "loglib/key_index.hpp"
#include "loglib/log_data.hpp"
#include "loglib/log_parse_sink.hpp"

#include <memory>
#include <string>
#include <vector>

namespace loglib::internal
{

/// `LogParseSink` adapter behind the synchronous `LogParser::Parse(path)`
/// overload: accumulates every batch into a single `LogData`. One sink per parse.
class BufferingSink : public LogParseSink
{
public:
    /// Takes ownership of @p source and routes batches into an internal
    /// `KeyIndex`. The owned source's `LogFile` is the canonical arena for
    /// `OwnedString` payload concatenation; line-offset pushes come in
    /// through `OnBatch`.
    explicit BufferingSink(std::unique_ptr<FileLineSource> source);

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
    std::unique_ptr<FileLineSource> mSource;
    KeyIndex mKeys;
    std::vector<LogLine> mLines;
    std::vector<uint64_t> mLineOffsets;
    std::vector<std::string> mErrors;
    bool mFinished = false;
};

} // namespace loglib::internal
