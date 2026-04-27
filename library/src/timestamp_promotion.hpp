#pragma once

#include "loglib/key_index.hpp"
#include "loglib/log_configuration.hpp"
#include "loglib/log_line.hpp"
#include "loglib/log_processing.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace loglib::detail
{

/// Per-worker per-time-column same-bytes short-circuit. Many real-world logs emit
/// consecutive lines with identical timestamps; on a hit we reuse the previous parse
/// and skip the format dispatch entirely.
struct LastTimestampBytesHit
{
    KeyId keyId = kInvalidKeyId;
    std::string bytes;
    TimeStamp parsed{};
    bool valid = false;
};

/// Pre-resolved view of one configured `Type::time` column. The harness materialises
/// one of these per `Type::time` column at pipeline start so the per-line promotion
/// hook never re-resolves keys or re-classifies format strings.
struct TimeColumnSpec
{
    std::vector<KeyId> keyIds;
    std::vector<std::string> parseFormats;
    std::vector<TimestampFormatKind> formatKinds;
};

/// Walks `configuration->columns`, emits one `TimeColumnSpec` per `Type::time` column
/// with its keys resolved to `KeyId`s via `keys.GetOrInsert` and every format string
/// pre-classified by `ClassifyTimestampFormat`. Returns an empty vector when the
/// configuration pointer is null.
std::vector<TimeColumnSpec>
BuildTimeColumnSpecs(KeyIndex &keys, const LogConfiguration *configuration);

/// Promotes one stored line's `Type::time` column values to `TimeStamp` using the
/// pre-resolved `TimeColumnSpec` matrix and the worker's per-column carry-over caches.
///
/// Failure-mode contract: lines whose configured key is missing or whose value does not
/// match any (KeyId, format) pair are silently left as-is — `LogTable::AppendBatch`'s
/// mid-stream back-fill takes a second pass on them.
void PromoteLineTimestamps(
    LogLine &line,
    std::span<const TimeColumnSpec> timeColumns,
    std::vector<std::optional<LastValidTimestampParse>> &lastValid,
    std::vector<LastTimestampBytesHit> &bytesHits,
    TimestampParseScratch &tsScratch
);

} // namespace loglib::detail
