/**
 * @brief Concept for turning one line into a row.
 *
 * Each format provides a decoder that reads a line and fills a row. The file pipeline and the live-stream loop both
 * call decoders, which keeps the two paths consistent. This header also defines the result codes a decoder returns.
 *
 * This is part of the parse module.
 */
#pragma once

#include "loglib/core/compact_log_value.hpp"
#include "loglib/core/key_index.hpp"
#include "loglib/parse/internal/parse_runtime.hpp"

#include <concepts>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace loglib::internal
{

/**
 * @brief What a decoder did with one line.
 *
 * - Emit: add a row built from the decoded values, which may be none.
 * - Skip: ignore the line without a row or an error. A parser with a header line, such as CSV, uses it to consume the
 *   header. The line counter still moves on.
 * - Error: add no row. The loop turns the error text into "Error on line N: ...".
 * - Continue: do not emit now; the loop appends the line to the previous record. Without a previous record the line is
 *   reported as a continuation with nothing to continue.
 */
enum class LineDecodeResult : uint8_t
{
    Emit,
    Skip,
    Error,
    Continue,
};

/**
 * @brief The result of adding continuation text to the last field of a record.
 *
 * MissingTarget includes a capture that was optional and is absent. NonStringTarget includes a string that lives in the
 * mapped file and cannot be extended.
 */
enum class ContinuationSpliceOutcome : uint8_t
{
    Ok,
    MissingTarget,
    NonStringTarget,
};

/**
 * @brief What a format-specific line decoder must provide for the streaming loop.
 *
 * RunStreamingParseLoop hands over one physical line at a time and knows nothing about the format. The loop skips blank
 * lines, so the line is never empty. DecodeCompact fills outValues with key ids and compact values. OwnedString offsets
 * point into outOwnedArena, which the loop hands to StreamLineSource in one AppendLine call. On Error the decoder puts
 * a message in outError.
 *
 * A decoder may keep scratch state for a run, such as a simdjson parser, padded buffers or whether the CSV header was
 * read. A decoder that can return Continue must also provide LastContinuationTarget with the field to extend.
 */
template <class T>
concept CompactLineDecoder = requires(
    T &decoder,
    std::string_view line,
    KeyIndex &keys,
    PerWorkerKeyCache &keyCache,
    std::vector<CompactLogValue> &outValues,
    std::string &outOwnedArena,
    std::string &outError
) {
    {
        decoder.DecodeCompact(line, keys, keyCache, outValues, outOwnedArena, outError)
    } -> std::convertible_to<LineDecodeResult>;
};

} // namespace loglib::internal
