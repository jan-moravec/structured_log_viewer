#pragma once

#include "loglib/key_index.hpp"
#include "loglib/log_configuration.hpp"
#include "loglib/log_line.hpp"
#include "loglib/log_processing.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace loglib::internal
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

} // namespace loglib::internal
