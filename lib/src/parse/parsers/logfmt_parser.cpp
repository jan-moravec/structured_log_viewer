#include "loglib/parse/parsers/logfmt_parser.hpp"

#include "loglib/parse/format_detection.hpp"

#include "loglib/core/compact_log_value.hpp"
#include "loglib/core/file_line_source.hpp"
#include "loglib/core/log_line.hpp"
#include "loglib/core/stream_line_source.hpp"
#include "loglib/parse/internal/classify_bare_scalar.hpp"
#include "loglib/parse/internal/line_decoder.hpp"
#include "loglib/parse/internal/probe_line_view.hpp"
#include "loglib/parse/internal/static_parser_pipeline.hpp"
#include "loglib/parse/internal/streaming_parse_loop.hpp"
#include "loglib/parse/log_parser.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace loglib
{

namespace
{

constexpr size_t INITIAL_FIELD_CAPACITY = 16;

/**
 * Crossover at which InsertSorted switches from a linear back-scan to
 * std::lower_bound. Same threshold the JSON parser uses.
 */
constexpr size_t INSERT_SORTED_LOWER_BOUND_THRESHOLD = 8;

void InsertSorted(std::vector<loglib::CompactLogValue> &out, KeyId id, loglib::CompactLogValue value)
{
    if (out.size() < INSERT_SORTED_LOWER_BOUND_THRESHOLD)
    {
        auto it = out.end();
        while (it != out.begin())
        {
            auto prev = it - 1;
            if (prev->Key() < id)
            {
                break;
            }
            if (prev->Key() == id)
            {
                *prev = value.WithKey(id);
                return;
            }
            it = prev;
        }
        out.insert(it, value.WithKey(id));
        return;
    }

    const auto it = std::lower_bound(out.begin(), out.end(), id, [](const loglib::CompactLogValue &lhs, KeyId rhs) {
        return lhs.Key() < rhs;
    });
    if (it != out.end() && it->Key() == id)
    {
        *it = value.WithKey(id);
        return;
    }
    out.insert(it, value.WithKey(id));
}

/**
 * One key/value pair from a single record.
 * valueWasQuoted: disables typed-value detection ("42" stays a string).
 * unescapedFromQuoted: value bytes live in the decoder's scratch
 * buffer (not aliasing the source), so they must be copied into the arena.
 */
struct LogfmtField
{
    std::string_view key;
    std::string_view value;
    bool valueWasQuoted = false;
    bool unescapedFromQuoted = false;
    bool valueIsNull = false;
};

/**
 * State machine ported from kr/logfmt's scanner.go
 * (https://github.com/kr/logfmt/blob/19f9bcb100e6/scanner.go).
 * Walks @p line and emits each key=value (or bare-key) field via
 * @p emit. Returns false on an unterminated quoted value; fields
 * emitted before that point are kept. @p quotedScratch is reused
 * across calls to hold unescaped quoted bytes.
 */
template <class Emit> bool TokenizeLogfmtLine(std::string_view line, std::string &quotedScratch, Emit emit)
{
    const char *const data = line.data();
    const size_t end = line.size();
    size_t i = 0;

    auto isSpace = [](unsigned char c) noexcept {
        return c == ' ' || c == '\t' || c == '\v' || c == '\f' || c == '\r';
    };

    while (i < end)
    {
        // Skip inter-pair whitespace and any trailing junk after a previous pair.
        while (i < end && static_cast<unsigned char>(data[i]) <= ' ')
        {
            ++i;
        }
        if (i >= end)
        {
            return true;
        }

        // Read a key: printable ASCII excluding '=' / '"' / whitespace.
        const size_t keyStart = i;
        while (i < end)
        {
            const auto c = static_cast<unsigned char>(data[i]);
            if (c <= ' ' || c == '=' || c == '"')
            {
                break;
            }
            ++i;
        }
        const std::string_view key(data + keyStart, i - keyStart);
        if (key.empty())
        {
            // Stray '=' or '"' at the start — skip it so a single junk
            // byte cannot stall the scanner.
            ++i;
            continue;
        }

        // No '=' -> bare key, null value.
        if (i >= end || data[i] != '=')
        {
            LogfmtField field;
            field.key = key;
            field.valueIsNull = true;
            emit(field);
            continue;
        }
        ++i; // consume '='

        // Empty value: key= followed by whitespace or EOL is null.
        if (i >= end || isSpace(static_cast<unsigned char>(data[i])) || data[i] == '\n')
        {
            LogfmtField field;
            field.key = key;
            field.valueIsNull = true;
            emit(field);
            continue;
        }

        if (data[i] == '"')
        {
            // Quoted value: walk until matching '"' honouring backslash escapes.
            ++i;
            const size_t innerStart = i;
            bool sawEscape = false;
            bool terminated = false;
            while (i < end)
            {
                const char c = data[i];
                if (c == '\\')
                {
                    sawEscape = true;
                    if (i + 1 < end)
                    {
                        i += 2;
                        continue;
                    }
                    // Trailing backslash with no escape target — treat as unterminated.
                    i = end;
                    break;
                }
                if (c == '"')
                {
                    terminated = true;
                    break;
                }
                ++i;
            }
            if (!terminated)
            {
                // Surface the error without emitting a half-formed
                // field; callers wrap with a line number.
                return false;
            }

            const std::string_view rawInner(data + innerStart, i - innerStart);
            ++i; // consume closing '"'

            LogfmtField field;
            field.key = key;
            field.valueWasQuoted = true;
            if (!sawEscape)
            {
                field.value = rawInner;
            }
            else
            {
                quotedScratch.clear();
                quotedScratch.reserve(rawInner.size());
                for (size_t j = 0; j < rawInner.size(); ++j)
                {
                    const char c = rawInner[j];
                    if (c == '\\' && j + 1 < rawInner.size())
                    {
                        const char next = rawInner[j + 1];
                        switch (next)
                        {
                        case 'n':
                            quotedScratch.push_back('\n');
                            break;
                        case 'r':
                            quotedScratch.push_back('\r');
                            break;
                        case 't':
                            quotedScratch.push_back('\t');
                            break;
                        case '"':
                            quotedScratch.push_back('"');
                            break;
                        case '\\':
                            quotedScratch.push_back('\\');
                            break;
                        default:
                            // Unknown escape: pass both bytes through
                            // (matches Go's unquoteBytes fallback).
                            quotedScratch.push_back('\\');
                            quotedScratch.push_back(next);
                            break;
                        }
                        ++j;
                    }
                    else
                    {
                        quotedScratch.push_back(c);
                    }
                }
                field.value = std::string_view(quotedScratch);
                field.unescapedFromQuoted = true;
            }
            emit(field);
            continue;
        }

        // Bare value: printable ASCII excluding '"' / whitespace.
        // (kr/logfmt permits '=' inside bare values; we keep that.)
        const size_t valueStart = i;
        while (i < end)
        {
            const auto c = static_cast<unsigned char>(data[i]);
            if (c <= ' ' || c == '"')
            {
                break;
            }
            ++i;
        }
        LogfmtField field;
        field.key = key;
        field.value = std::string_view(data + valueStart, i - valueStart);
        emit(field);
    }
    return true;
}

/**
 * Parse one logfmt record into compact values.
 * fileBegin/fileSize describe the mmap (or 0/nullptr when
 * streaming). ownedArena is the per-batch buffer for
 * OwnedString payloads. quotedScratch is reused across calls.
 */
void ParseLogfmtLine(
    std::string_view line,
    KeyIndex &keys,
    internal::WorkerKeyCache &keyCache,
    const char *fileBegin,
    size_t fileSize,
    std::string &ownedArena,
    std::string &quotedScratch,
    std::vector<loglib::CompactLogValue> &out,
    bool &outUnterminated,
    KeyId *outLastSourceOrderKey = nullptr
)
{
    out.clear();
    out.reserve(INITIAL_FIELD_CAPACITY);
    if (outLastSourceOrderKey != nullptr)
    {
        // Continuations target source order, not sorted KeyId order.
        *outLastSourceOrderKey = INVALID_KEY_ID;
    }

    auto emit = [&](const LogfmtField &field) {
        const KeyId keyId = internal::InternKeyVia(field.key, keys, keyCache);
        if (outLastSourceOrderKey != nullptr)
        {
            *outLastSourceOrderKey = keyId;
        }

        if (field.valueIsNull)
        {
            InsertSorted(out, keyId, loglib::CompactLogValue::MakeMonostate());
            return;
        }

        if (field.valueWasQuoted)
        {
            // unescapedFromQuoted -> bytes are in quotedScratch
            // (which the next field overwrites), so copy into the
            // arena. Otherwise the view aliases the source (mmap or
            // streaming carry buffer) and MakeStringCompact picks
            // the right tag.
            if (field.unescapedFromQuoted)
            {
                const uint64_t offset = ownedArena.size();
                ownedArena.append(field.value.data(), field.value.size());
                InsertSorted(
                    out,
                    keyId,
                    loglib::CompactLogValue::MakeOwnedString(offset, static_cast<uint32_t>(field.value.size()))
                );
            }
            else
            {
                InsertSorted(out, keyId, internal::MakeStringCompact(field.value, fileBegin, fileSize, ownedArena));
            }
            return;
        }

        InsertSorted(out, keyId, internal::ClassifyBareScalar(field.value, fileBegin, fileSize, ownedArena));
    };

    outUnterminated = !TokenizeLogfmtLine(line, quotedScratch, emit);
}

bool LineLooksLikeLogfmt(std::string_view line)
{
    // Reject obvious JSON so JSON wins the auto-detect race.
    if (!line.empty() && (line.front() == '{' || line.front() == '['))
    {
        return false;
    }

    // Cheapest stable signal: an '=' after at least one key byte.
    size_t i = 0;
    while (i < line.size() && static_cast<unsigned char>(line[i]) <= ' ')
    {
        ++i;
    }
    const size_t keyStart = i;
    while (i < line.size())
    {
        const auto c = static_cast<unsigned char>(line[i]);
        if (c <= ' ' || c == '=' || c == '"')
        {
            break;
        }
        ++i;
    }
    return i != keyStart && i < line.size() && line[i] == '=';
}

/**
 * The logfmt decoder. It owns the scratch for unescaping quoted values. In multi-line mode, indented lines continue the
 * preceding record's last source-order field. Other lines retain normal logfmt parsing.
 */
class LogfmtLineDecoder
{
public:
    /** A UTF-8 byte order mark can start the first line of the input. */
    static constexpr bool kStripBom = true;

    LogfmtLineDecoder() = delete;

    explicit LogfmtLineDecoder(bool multiline) noexcept
        : mMultiline(multiline)
    {
    }

    internal::LineDecodeResult Decode(
        std::string_view line,
        internal::LineContext &ctx,
        std::vector<loglib::CompactLogValue> &out,
        std::string &errorOut
    )
    {
        out.clear();
        if (mMultiline && (line.front() == ' ' || line.front() == '\t'))
        {
            return internal::LineDecodeResult::Continue;
        }

        try
        {
            bool unterminated = false;
            KeyId lastSourceOrderKey = INVALID_KEY_ID;
            // A stream has no mapped file, so no string can refer to it and every string is copied.
            ParseLogfmtLine(
                line,
                ctx.keys,
                ctx.keyCache,
                ctx.stableBytes.data(),
                ctx.stableBytes.size(),
                ctx.ownedBytes,
                mQuotedScratch,
                out,
                unterminated,
                &lastSourceOrderKey
            );
            if (unterminated)
            {
                mContinuationTarget = INVALID_KEY_ID;
                errorOut = "Unterminated quoted value.";
                return internal::LineDecodeResult::Error;
            }
            if (out.empty())
            {
                // No key=value pairs: not a logfmt record. Surface it so the caller sees why the row is empty.
                mContinuationTarget = INVALID_KEY_ID;
                errorOut = "Not a logfmt record.";
                return internal::LineDecodeResult::Error;
            }
            mContinuationTarget = lastSourceOrderKey;
            return internal::LineDecodeResult::Emit;
        }
        catch (const std::exception &e)
        {
            mContinuationTarget = INVALID_KEY_ID;
            errorOut = std::string(e.what());
            return internal::LineDecodeResult::Error;
        }
    }

    /** The last source-order field of the most recent record. */
    [[nodiscard]] KeyId ContinuationTarget() const noexcept
    {
        return mMultiline ? mContinuationTarget : INVALID_KEY_ID;
    }

private:
    std::string mQuotedScratch;
    bool mMultiline = true;
    KeyId mContinuationTarget = INVALID_KEY_ID;
};

static_assert(internal::LineDecoder<LogfmtLineDecoder>, "LogfmtLineDecoder must satisfy the LineDecoder concept");

} // namespace

bool LooksLikeLogfmt(std::string_view sniffBuffer)
{
    // Callers cap sniffBuffer at PROBE_BYTES_BUDGET, so the
    // cursor bound is sufficient.
    size_t cursor = 0;
    while (cursor < sniffBuffer.size())
    {
        const internal::ProbeLine probe = internal::NextProbeLine(sniffBuffer, cursor);
        cursor = probe.nextOffset;
        if (probe.line.empty())
        {
            continue;
        }
        return LineLooksLikeLogfmt(probe.line);
    }
    return false;
}

LogfmtParser::LogfmtParser(bool multiline) noexcept
    : mMultiline(multiline)
{
}

void LogfmtParser::Parse(StreamLineSource &source, LogParseSink &sink, ParserOptions options) const
{
    LogfmtLineDecoder decoder(mMultiline);
    internal::RunStreamingParseLoop(source, decoder, sink, options);
}

void LogfmtParser::Parse(FileLineSource &source, LogParseSink &sink, ParserOptions options) const
{
    internal::RunStaticParserPipeline<LogfmtLineDecoder>(source, sink, options, [multiline = mMultiline] {
        return LogfmtLineDecoder(multiline);
    });
}

} // namespace loglib
