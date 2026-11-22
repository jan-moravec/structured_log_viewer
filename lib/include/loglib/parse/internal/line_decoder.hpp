/**
 * @brief Concept for turning one line into a row.
 *
 * Each format provides one decoder that reads a line and fills a row. The file pipeline and the live-stream loop both
 * call it, and they own everything else: splitting lines, blank lines, continuation lines, line numbers and errors. A
 * new format therefore writes one decoder and nothing else. This header also defines the result codes a decoder returns
 * and the context it works in.
 *
 * This is part of the parse module.
 */
#pragma once

#include "loglib/core/compact_log_value.hpp"
#include "loglib/core/key_index.hpp"
#include "loglib/parse/internal/parse_runtime.hpp"

#include <concepts>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace loglib::internal
{

/**
 * @brief What a decoder did with one line.
 *
 * - Emit: add a row built from the decoded values, which may be none.
 * - Skip: ignore the line without a row or an error. A parser with a header line, such as CSV, uses it to consume the
 *   header. The line counter still moves on.
 * - Error: add no row. The driver turns the error text into "Error on line N: ...".
 * - Continue: do not emit now; the driver appends the line to the previous record. Without a previous record the line
 * is reported as a continuation with nothing to continue.
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
 * @brief What a decoder can reach while it decodes one line.
 *
 * keys and keyCache turn key text into key ids. Decoded strings that cannot stay in the input are appended to
 * ownedBytes, and a cell refers to them by their offset in it, so a decoder never clears it. stableBytes is the memory
 * map of the file when the input is a file, where a string that lies inside it can be referred to without a copy. It is
 * empty for a stream. bytesAfterLine is how many bytes after the end of the line are readable inside stableBytes, which
 * a parser that reads past the end of its input, such as simdjson, needs to decide whether it can read the line in
 * place.
 */
struct LineContext
{
    // NOLINTBEGIN(cppcoreguidelines-avoid-const-or-ref-data-members)
    KeyIndex &keys;
    WorkerKeyCache &keyCache;
    std::string &ownedBytes;
    std::span<const char> stableBytes;
    size_t bytesAfterLine = 0;
    // NOLINTEND(cppcoreguidelines-avoid-const-or-ref-data-members)
};

/**
 * @brief What a format-specific line decoder must provide.
 *
 * Decode gets one physical line that is not blank, without its line break. It clears out and fills it with the key ids
 * and compact values of the row, sorted by key id with each key once. OwnedString offsets point into ctx.ownedBytes. On
 * Error the decoder puts a message in error. ContinuationTarget is the field that continuation lines extend for the
 * record that the last Emit started, or INVALID_KEY_ID when the format never continues a record.
 *
 * A decoder may keep scratch state for a run, such as a simdjson parser, padded buffers, match data or whether the CSV
 * header was read, so the drivers give each worker its own. A decoder may define a static member K_STRIP_BOM that is
 * true when the first line of the input can start with a UTF-8 byte order mark, which the drivers then remove before
 * decoding.
 */
template <class T>
concept LineDecoder =
    std::move_constructible<T> &&
    requires(
        T &decoder, std::string_view line, LineContext &ctx, std::vector<CompactLogValue> &out, std::string &error
    ) {
        { decoder.Decode(line, ctx, out, error) } -> std::same_as<LineDecodeResult>;
        { decoder.ContinuationTarget() } -> std::same_as<KeyId>;
    };

/** @brief True when the decoder wants the byte order mark of the first line removed before it decodes. */
template <class T> constexpr bool StripsBom()
{
    if constexpr (requires { T::K_STRIP_BOM; })
    {
        return T::K_STRIP_BOM;
    }
    else
    {
        return false;
    }
}

/** @brief The UTF-8 byte order mark. */
inline constexpr std::string_view UTF8_BYTE_ORDER_MARK = "\xEF\xBB\xBF";

} // namespace loglib::internal
