#pragma once

#include "loglib/internal/compact_log_value.hpp"
#include "loglib/internal/parser_pipeline.hpp"
#include "loglib/key_index.hpp"

#include <concepts>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace loglib::detail
{

/// Format-specific record decoder used by the streaming pipeline.
/// `RunStreamingParserToLogLines` in `parser_pipeline.hpp` feeds the
/// decoder one record at a time and stays format-agnostic; per-format
/// code (JSON, future logfmt / CSV / syslog / key=value) implements
/// this concept.
///
/// The streaming pipeline filters out empty / blank lines before
/// calling `DecodeCompact`, so implementations may assume @p line is
/// non-empty.
///
/// `DecodeCompact` fills @p outValues with `(KeyId, CompactLogValue)`
/// pairs whose `OwnedString` payloads carry offsets into
/// @p outOwnedArena. The streaming pipeline transfers
/// @p outOwnedArena into the `StreamLineSource` (which becomes the
/// canonical `LineSource *` for the resulting `LogLine`) in a single
/// `AppendLine` call — the hot-path `LogLine` ctor never has to
/// mutate the source.
///
/// On parse error the implementation returns false and writes a
/// human-facing message into @p outError; the streaming pipeline
/// composes the absolute "Error on line N: <body>" wrapper from this
/// body and the running line cursor.
///
/// Implementations are expected to carry whatever scratch state the
/// format requires (e.g. a simdjson parser, a padded-line buffer, a
/// type cache) as member fields and reuse it across `DecodeCompact`
/// invocations within a single parse run.
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
    } -> std::convertible_to<bool>;
};

} // namespace loglib::detail
