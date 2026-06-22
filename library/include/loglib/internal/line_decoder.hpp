#pragma once

#include "loglib/internal/compact_log_value.hpp"
#include "loglib/internal/parse_runtime.hpp"
#include "loglib/key_index.hpp"

#include <concepts>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace loglib::internal
{

/// Outcome of a single-line decode:
///  - `Emit`: append a `LogLine` from @p outValues (may be empty).
///  - `Skip`: swallow the line silently (no row, no error). Used by
///    parsers with a header prelude (CSV) to consume the schema row.
///    The loop still advances the line-number cursor.
///  - `Error`: emit no row; the loop wraps @p errorOut as
///    "Error on line N: ...".
enum class LineDecodeResult : uint8_t
{
    Emit,
    Skip,
    Error,
};

/// Format-specific record decoder for the streaming pipeline.
/// `RunStreamingParseLoop` feeds one record at a time and stays
/// format-agnostic; per-format code implements this concept.
///
/// The pipeline pre-filters empty/blank lines, so @p line is non-empty.
/// `DecodeCompact` fills @p outValues with `(KeyId, CompactLogValue)`,
/// where `OwnedString` payloads index into @p outOwnedArena. The
/// arena is transferred into `StreamLineSource` in a single
/// `AppendLine` call.
///
/// On `Error`: put a human message in @p errorOut; the pipeline
/// wraps it with "Error on line N: ...".
///
/// Implementations may carry per-run scratch state (simdjson parser,
/// padded buffers, CSV header latch, etc.) as member fields.
template <class T>
concept CompactLineDecoder = requires(
    T &decoder,
    std::string_view line,
    KeyIndex &keys,
    PerWorkerKeyCache *keyCache,
    std::vector<std::pair<KeyId, CompactLogValue>> &outValues,
    std::string &outOwnedArena,
    std::string &outError
) {
    {
        decoder.DecodeCompact(line, keys, keyCache, outValues, outOwnedArena, outError)
    } -> std::convertible_to<LineDecodeResult>;
};

} // namespace loglib::internal
