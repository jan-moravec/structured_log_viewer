#pragma once

#include "loglib/key_index.hpp"
#include "loglib/log_configuration.hpp"
#include "loglib/log_line.hpp"
#include "loglib/log_processing.hpp"
#include "loglib/stream_log_line.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace loglib::detail
{

/// Per-worker per-time-column same-bytes short-circuit, exploiting the common
/// case of consecutive lines sharing a timestamp.
struct LastTimestampBytesHit
{
    KeyId keyId = kInvalidKeyId;
    std::string bytes;
    TimeStamp parsed{};
    bool valid = false;
};

/// Pre-resolved view of one configured `Type::time` column: keys resolved to
/// `KeyId`s, formats pre-classified.
struct TimeColumnSpec
{
    std::vector<KeyId> keyIds;
    std::vector<std::string> parseFormats;
    std::vector<TimestampFormatKind> formatKinds;
};

/// Builds one `TimeColumnSpec` per `Type::time` column. Returns empty when
/// @p configuration is null.
std::vector<TimeColumnSpec> BuildTimeColumnSpecs(KeyIndex &keys, const LogConfiguration *configuration);

/// Promotes one line's `Type::time` columns in place. Returns `true` iff at
/// least one column was promoted on this line. Lines that don't match any
/// (KeyId, format) pair are left untouched — the `LogTable` mid-stream
/// back-fill picks them up.
///
/// @p ownedArena is the byte buffer that any `OwnedString` compact values
/// on @p line currently reference: during Stage B that's the per-batch
/// staging buffer (`ParsedPipelineBatch::ownedStringsArena`); for the
/// post-stream `BackfillTimestampColumn` path it's the file's arena
/// (`LogFile::OwnedStringsView()`).
bool PromoteLineTimestamps(
    LogLine &line,
    std::span<const TimeColumnSpec> timeColumns,
    std::vector<std::optional<LastValidTimestampParse>> &lastValid,
    std::vector<LastTimestampBytesHit> &bytesHits,
    TimestampParseScratch &tsScratch,
    std::string_view ownedArena
);

/// `StreamLogLine` overload of `PromoteLineTimestamps`. Stream lines own
/// their values directly (no `MmapSlice` / `OwnedString` arena
/// indirection — see `stream_log_line.hpp`), so the @p ownedArena
/// parameter is not needed; the helper reads strings via
/// `StreamLogLine::GetValue` and writes promoted timestamps via
/// `StreamLogLine::SetValue`. Returns true iff at least one column was
/// promoted on this line. Used by `WorkerScratchBase::PromoteTimestamps`
/// when driving the streaming-loop parser path
/// (`JsonParser::ParseStreaming(LogSource&, ...)` non-mmap branch).
bool PromoteStreamLineTimestamps(
    StreamLogLine &line,
    std::span<const TimeColumnSpec> timeColumns,
    std::vector<std::optional<LastValidTimestampParse>> &lastValid,
    std::vector<LastTimestampBytesHit> &bytesHits,
    TimestampParseScratch &tsScratch
);

} // namespace loglib::detail
