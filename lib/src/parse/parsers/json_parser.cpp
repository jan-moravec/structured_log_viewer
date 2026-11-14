#include "loglib/parse/parsers/json_parser.hpp"

#include "loglib/core/compact_log_value.hpp"
#include "loglib/core/file_line_source.hpp"
#include "loglib/core/log_line.hpp"
#include "loglib/core/stream_line_source.hpp"
#include "loglib/parse/internal/line_decoder.hpp"
#include "loglib/parse/internal/probe_line_view.hpp"
#include "loglib/parse/internal/static_parser_pipeline.hpp"
#include "loglib/parse/internal/streaming_parse_loop.hpp"
#include "loglib/parse/log_parser.hpp"
#include "loglib/time/internal/timestamp_promotion.hpp"

#include <date/date.h>
#include <fmt/format.h>
#include <glaze/glaze.hpp>
#include <algorithm>

#include <simdjson.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace loglib
{

namespace
{

/** The key of a JSON field. A key without escapes is a view into the line; one with escapes is decoded into owned. */
struct FastFieldKey
{
    bool isView = false;
    std::string_view view;
    std::string owned;
};

template <class Field> FastFieldKey ExtractFieldKey(Field &field)
{
    FastFieldKey result;

    std::string_view escaped;
    if (field.escaped_key().get(escaped))
    {
        return result;
    }

    if (!escaped.contains('\\'))
    {
        result.isView = true;
        result.view = escaped;
        return result;
    }

    std::string_view unescaped;
    if (!field.unescaped_key().get(unescaped))
    {
        result.isView = false;
        result.owned = std::string(unescaped);
    }
    return result;
}

/**
 * The field count from which InsertSorted uses a binary search in place of a scan from the back. Set on the wide-row
 * benchmarks.
 */
constexpr size_t INSERT_SORTED_LOWER_BOUND_THRESHOLD = 8;
constexpr size_t INITIAL_OBJECT_FIELD_CAPACITY = 16;
constexpr size_t LINE_PADDED_EXTRA_SLACK_BYTES = 64;

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

    // Asymmetric comparator: std::ranges::lower_bound rejects it.
    auto it = std::lower_bound(out.begin(), out.end(), id, [](const loglib::CompactLogValue &lhs, KeyId rhs) {
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
 * Turns a string into a compact value. Text that lies inside the mapped file becomes a MmapSlice value with no copy.
 * Other text is appended to ownedArena and becomes an OwnedString value.
 */
loglib::CompactLogValue MakeStringCompact(
    std::string_view sv, const char *fileBegin, size_t fileSize, std::string &ownedArena
)
{
    if (sv.data() >= fileBegin && sv.data() + sv.size() <= fileBegin + fileSize)
    {
        const auto offset = static_cast<uint64_t>(sv.data() - fileBegin);
        return loglib::CompactLogValue::MakeMmapSlice(offset, static_cast<uint32_t>(sv.size()));
    }
    const uint64_t offset = ownedArena.size();
    ownedArena.append(sv.data(), sv.size());
    return loglib::CompactLogValue::MakeOwnedString(offset, static_cast<uint32_t>(sv.size()));
}

template <class Value>
loglib::CompactLogValue ExtractStringValue(
    Value &value, bool sourceIsStable, const char *fileBegin, size_t fileSize, std::string &ownedArena
)
{
    // The string_view fast path is only safe when simdjson iterates
    // over the mmap; padded-scratch fallback views dangle on the next
    // line. `raw_json_token()` / `raw_json()` source-aliasing is pinned
    // to simdjson v4.6.3 — re-verify on bump (the `[allocations]`
    // benchmark's fast-path fraction is the regression signal).
    if (sourceIsStable)
    {
        const std::string_view rawToken(value.raw_json_token());
        if (rawToken.size() >= 2 && rawToken.front() == '"' && rawToken.back() == '"')
        {
            const std::string_view inner = rawToken.substr(1, rawToken.size() - 2);
            if (!inner.contains('\\'))
            {
                return MakeStringCompact(inner, fileBegin, fileSize, ownedArena);
            }
        }
    }

    std::string_view stringValue;
    if (!value.get_string().get(stringValue))
    {
        // Padded-scratch fallback: the view aliases the worker's
        // scratch buffer, so `MakeStringCompact` falls through to the
        // `OwnedString` arena path.
        return MakeStringCompact(stringValue, fileBegin, fileSize, ownedArena);
    }
    return loglib::CompactLogValue::MakeMonostate();
}

template <class Value>
loglib::CompactLogValue ExtractRawJsonValue(
    Value &value, bool sourceIsStable, const char *fileBegin, size_t fileSize, std::string &ownedArena
)
{
    std::string_view rawJson;
    if (!value.raw_json().get(rawJson))
    {
        while (!rawJson.empty() &&
               (rawJson.back() == ' ' || rawJson.back() == '\n' || rawJson.back() == '\r' || rawJson.back() == '\t'))
        {
            rawJson.remove_suffix(1);
        }
        if (sourceIsStable)
        {
            return MakeStringCompact(rawJson, fileBegin, fileSize, ownedArena);
        }
        // Padded-scratch fallback: copy into the per-batch arena.
        const uint64_t offset = ownedArena.size();
        ownedArena.append(rawJson.data(), rawJson.size());
        return loglib::CompactLogValue::MakeOwnedString(offset, static_cast<uint32_t>(rawJson.size()));
    }
    return loglib::CompactLogValue::MakeMonostate();
}

/**
 * The simdjson type seen last for each key, so the type queries run only on the first value of a key. The cached number
 * type only picks the fast path to try. A fast path accepts a value only when the value's own text has that type, so a
 * key gets the same cell types on every worker.
 */
struct ParseCache
{
    std::vector<simdjson::ondemand::json_type> keyTypes;
    std::vector<simdjson::ondemand::number_type> numberTypes;
    std::vector<bool> hasKeyType;
    std::vector<bool> hasNumberType;
};

void EnsureCacheCapacity(ParseCache &cache, KeyId id)
{
    const size_t needed = static_cast<size_t>(id) + 1;
    // Geometric growth keeps the wide-log first-batch cost amortised O(K).
    auto growTo = [needed](auto &vec, auto fill) {
        if (vec.size() >= needed)
        {
            return;
        }
        const size_t target = std::max(needed, vec.size() * 2);
        vec.resize(target, fill);
    };
    growTo(cache.keyTypes, simdjson::ondemand::json_type::null);
    growTo(cache.hasKeyType, false);
    growTo(cache.numberTypes, simdjson::ondemand::number_type::signed_integer);
    growTo(cache.hasNumberType, false);
}

void ParseJsonLine(
    std::vector<loglib::CompactLogValue> &result,
    simdjson::ondemand::object &object,
    KeyIndex &keys,
    ParseCache &cache,
    bool sourceIsStable,
    internal::PerWorkerKeyCache &keyCache,
    const char *fileBegin,
    size_t fileSize,
    std::string &ownedArena,
    simdjson::error_code &outError
)
{
    result.clear();
    result.reserve(INITIAL_OBJECT_FIELD_CAPACITY);
    outError = simdjson::SUCCESS;

    for (auto field : object)
    {
        if (field.error())
        {
            outError = field.error();
            return;
        }
        const FastFieldKey fk = ExtractFieldKey(field);
        if (!fk.isView && fk.owned.empty())
        {
            outError = simdjson::STRING_ERROR;
            return;
        }

        const KeyId keyId = internal::InternKeyVia(fk.isView ? fk.view : std::string_view(fk.owned), keys, keyCache);
        EnsureCacheCapacity(cache, keyId);

        auto value = field.value();

        if (cache.hasKeyType[keyId])
        {
            const auto type = cache.keyTypes[keyId];
            switch (type)
            {
            case simdjson::ondemand::json_type::boolean:
            {
                bool b = false;
                if (!value.get(b))
                {
                    InsertSorted(result, keyId, loglib::CompactLogValue::MakeBool(b));
                    continue;
                }
                break;
            }
            case simdjson::ondemand::json_type::number:
            {
                if (cache.hasNumberType[keyId])
                {
                    switch (cache.numberTypes[keyId])
                    {
                    case simdjson::ondemand::number_type::signed_integer:
                    {
                        int64_t i = 0;
                        if (!value.get(i))
                        {
                            InsertSorted(result, keyId, loglib::CompactLogValue::MakeInt64(i));
                            continue;
                        }
                        break;
                    }
                    case simdjson::ondemand::number_type::unsigned_integer:
                    {
                        uint64_t u = 0;
                        if (!value.get(u))
                        {
                            // A value that fits in 64 bits signed is an Int64, as it would be on any other worker.
                            InsertSorted(
                                result,
                                keyId,
                                u > static_cast<uint64_t>(INT64_MAX)
                                    ? loglib::CompactLogValue::MakeUint64(u)
                                    : loglib::CompactLogValue::MakeInt64(static_cast<int64_t>(u))
                            );
                            continue;
                        }
                        break;
                    }
                    case simdjson::ondemand::number_type::floating_point_number:
                    {
                        // get(double) also accepts 5, so the text must show a fraction or an exponent.
                        double d = NAN;
                        std::string_view token;
                        if (!value.raw_json_token().get(token) &&
                            token.find_first_of(".eE") != std::string_view::npos && !value.get(d))
                        {
                            InsertSorted(result, keyId, loglib::CompactLogValue::MakeDouble(d));
                            continue;
                        }
                        break;
                    }
                    default:
                        break;
                    }
                }
                break;
            }
            case simdjson::ondemand::json_type::string:
            {
                const loglib::CompactLogValue stringValue =
                    ExtractStringValue(value, sourceIsStable, fileBegin, fileSize, ownedArena);
                if (stringValue.tag != loglib::CompactTag::Monostate)
                {
                    InsertSorted(result, keyId, stringValue);
                    continue;
                }
                break;
            }
            case simdjson::ondemand::json_type::array:
            case simdjson::ondemand::json_type::object:
            {
                const loglib::CompactLogValue rawValue =
                    ExtractRawJsonValue(value, sourceIsStable, fileBegin, fileSize, ownedArena);
                if (rawValue.tag == loglib::CompactTag::Monostate)
                {
                    // Reading a malformed container has already moved the iterator past its start, so the value
                    // cannot be asked for its type again. The line is an error, as it is on the uncached path.
                    outError = simdjson::TAPE_ERROR;
                    return;
                }
                InsertSorted(result, keyId, rawValue);
                continue;
            }
            case simdjson::ondemand::json_type::null:
                if (value.is_null())
                {
                    InsertSorted(result, keyId, loglib::CompactLogValue::MakeMonostate());
                    continue;
                }
                break;
            default:
                break;
            }
        }

        auto typeResult = value.type();
        if (typeResult.error())
        {
            outError = typeResult.error();
            return;
        }

        const auto type = typeResult.value();
        cache.keyTypes[keyId] = type;
        cache.hasKeyType[keyId] = true;

        switch (type)
        {
        case simdjson::ondemand::json_type::boolean:
        {
            bool b = false;
            if (const auto error = value.get(b); error)
            {
                outError = error;
                return;
            }
            InsertSorted(result, keyId, loglib::CompactLogValue::MakeBool(b));
            break;
        }
        case simdjson::ondemand::json_type::number:
        {
            auto numberType = value.get_number_type();
            if (numberType.error())
            {
                outError = numberType.error();
                return;
            }
            cache.numberTypes[keyId] = numberType.value();
            cache.hasNumberType[keyId] = true;
            switch (numberType.value())
            {
            case simdjson::ondemand::number_type::signed_integer:
            {
                int64_t i = 0;
                if (const auto error = value.get(i); error)
                {
                    outError = error;
                    return;
                }
                InsertSorted(result, keyId, loglib::CompactLogValue::MakeInt64(i));
                break;
            }
            case simdjson::ondemand::number_type::unsigned_integer:
            {
                uint64_t u = 0;
                if (const auto error = value.get(u); error)
                {
                    outError = error;
                    return;
                }
                InsertSorted(result, keyId, loglib::CompactLogValue::MakeUint64(u));
                break;
            }
            case simdjson::ondemand::number_type::floating_point_number:
            {
                double d = NAN;
                if (const auto error = value.get(d); error)
                {
                    outError = error;
                    return;
                }
                InsertSorted(result, keyId, loglib::CompactLogValue::MakeDouble(d));
                break;
            }
            default:
            {
                // An integer too large for 64 bits keeps its exact text.
                const loglib::CompactLogValue rawValue =
                    ExtractRawJsonValue(value, sourceIsStable, fileBegin, fileSize, ownedArena);
                if (rawValue.tag == loglib::CompactTag::Monostate)
                {
                    outError = simdjson::NUMBER_ERROR;
                    return;
                }
                InsertSorted(result, keyId, rawValue);
                break;
            }
            }
            break;
        }
        case simdjson::ondemand::json_type::string:
        {
            const loglib::CompactLogValue stringValue =
                ExtractStringValue(value, sourceIsStable, fileBegin, fileSize, ownedArena);
            if (stringValue.tag == loglib::CompactTag::Monostate)
            {
                outError = simdjson::STRING_ERROR;
                return;
            }
            InsertSorted(result, keyId, stringValue);
            break;
        }
        case simdjson::ondemand::json_type::array:
        case simdjson::ondemand::json_type::object:
        {
            const loglib::CompactLogValue rawValue =
                ExtractRawJsonValue(value, sourceIsStable, fileBegin, fileSize, ownedArena);
            if (rawValue.tag == loglib::CompactTag::Monostate)
            {
                outError = simdjson::TAPE_ERROR;
                return;
            }
            InsertSorted(result, keyId, rawValue);
            break;
        }
        case simdjson::ondemand::json_type::null:
        default:
        {
            bool isNull = false;
            if (const auto error = value.is_null().get(isNull); error || !isNull)
            {
                outError = error ? error : simdjson::N_ATOM_ERROR;
                return;
            }
            InsertSorted(result, keyId, loglib::CompactLogValue::MakeMonostate());
            break;
        }
        }
    }
}

/** The scratch memory of one JSON parse worker. */
struct JsonWorkerState
{
    simdjson::ondemand::parser parser;
    std::string linePadded;
    size_t maxLineSize = 0;
    ParseCache cache;
};

/** @brief Stage B: decodes the JSON lines of one line-aligned batch into compact lines. */
void DecodeJsonBatch(
    const internal::ByteRange &batch,
    internal::WorkerScratch<JsonWorkerState> &worker,
    KeyIndex &keys,
    FileLineSource &source,
    std::span<const internal::TimeColumnSpec> timeColumns,
    internal::ParsedPipelineBatch &parsed
)
{
    parsed.batchIndex = batch.batchIndex;

    const char *cursor = batch.bytesBegin;
    const char *end = batch.bytesEnd;
    const char *fileEnd = batch.fileEnd;
    const char *fileBegin = source.StableBytes().data();

    const auto batchBytes = static_cast<size_t>(end - cursor);
    const size_t estimatedLines = (batchBytes / 64) + 1;
    parsed.lines.reserve(estimatedLines);
    parsed.localLineOffsets.reserve(estimatedLines);

    size_t relativeLineNumber = 1;
    // One scratch for the whole batch; each row copies it into the batch's field arena.
    std::vector<loglib::CompactLogValue> values;

    while (cursor < end)
    {
        const char *lineStart = cursor;
        const char *newline = static_cast<const char *>(memchr(cursor, '\n', static_cast<size_t>(end - cursor)));
        const char *lineEnd = newline ? newline : end;
        cursor = (newline != nullptr) ? newline + 1 : end;

        std::string_view line(lineStart, static_cast<size_t>(lineEnd - lineStart));
        if (!line.empty() && line.back() == '\r')
        {
            line.remove_suffix(1);
        }

        // `RawLine` subtracts 1 (the '\n'); for an unterminated final
        // line push `fileSize + 1` so we don't lop off the last char.
        const uint64_t nextOffset = static_cast<uint64_t>(cursor - fileBegin) + (newline == nullptr ? 1u : 0u);
        parsed.localLineOffsets.push_back(nextOffset);

        if (line.empty())
        {
            relativeLineNumber++;
            continue;
        }

        try
        {
            // mmap fast path needs SIMDJSON_PADDING bytes of slack past
            // lineEnd; otherwise fall back to a padded scratch copy.
            const auto remaining = static_cast<size_t>(fileEnd - lineEnd);
            const bool sourceIsStable = remaining >= simdjson::SIMDJSON_PADDING;
            auto result =
                sourceIsStable ? worker.user.parser.iterate(line.data(), line.size(), line.size() + remaining) : [&]() {
                    const size_t needed = line.size() + simdjson::SIMDJSON_PADDING;
                    if (line.size() > worker.user.maxLineSize || worker.user.linePadded.size() < needed)
                    {
                        worker.user.maxLineSize = std::max(worker.user.maxLineSize, line.size());
                        worker.user.linePadded.resize(
                            worker.user.maxLineSize + simdjson::SIMDJSON_PADDING + LINE_PADDED_EXTRA_SLACK_BYTES
                        );
                    }
                    std::memcpy(worker.user.linePadded.data(), line.data(), line.size());
                    std::memset(worker.user.linePadded.data() + line.size(), 0, simdjson::SIMDJSON_PADDING);
                    return worker.user.parser.iterate(
                        worker.user.linePadded.data(), line.size(), worker.user.linePadded.size()
                    );
                }();
            if (result.error())
            {
                parsed.errors.push_back(
                    internal::ParsedLineError{
                        .relativeLine = relativeLineNumber, .body = std::string(simdjson::error_message(result.error()))
                    }
                );
                relativeLineNumber++;
                continue;
            }

            auto object = result.get_object();
            if (object.error())
            {
                parsed.errors.push_back(
                    internal::ParsedLineError{.relativeLine = relativeLineNumber, .body = "Not a JSON object."}
                );
                relativeLineNumber++;
                continue;
            }

            auto objectValue = object.value();
            const auto fileSize = static_cast<size_t>(fileEnd - fileBegin);
            simdjson::error_code parseError = simdjson::SUCCESS;
            ParseJsonLine(
                values,
                objectValue,
                keys,
                worker.user.cache,
                sourceIsStable,
                worker.keyCache,
                fileBegin,
                fileSize,
                parsed.ownedStringsArena,
                parseError
            );
            if (parseError)
            {
                parsed.errors.push_back(
                    internal::ParsedLineError{
                        .relativeLine = relativeLineNumber,
                        .body = std::string(simdjson::error_message(parseError)),
                    }
                );
                relativeLineNumber++;
                continue;
            }

            // 0-based intra-batch offset; Stage C shifts by the
            // running cursor so `FileLineSource::RawLine(LineId())` indexes
            // `mLineOffsets` correctly. `relativeLineNumber` stays
            // 1-based for human-facing error messages.
            LogLine logLine(values, parsed.fields, source, relativeLineNumber - 1);

            parsed.lines.push_back(std::move(logLine));

            // Inline promotion: values are still hot in L1.
            // `OwnedString` payloads still reference the per-batch
            // arena (Stage C rebases later), so pass it explicitly.
            worker.PromoteTimestamps(parsed.lines.back(), timeColumns, std::string_view(parsed.ownedStringsArena));
        }
        catch (const std::exception &e)
        {
            parsed.errors.push_back(
                internal::ParsedLineError{.relativeLine = relativeLineNumber, .body = std::string(e.what())}
            );
        }

        relativeLineNumber++;
    }

    parsed.totalLineCount = relativeLineNumber - 1;
}

} // namespace

bool JsonParser::IsValidBytes(std::string_view sniffBuffer) const
{
    std::size_t cursor = 0;
    while (cursor < sniffBuffer.size())
    {
        const internal::ProbeLine probe = internal::NextProbeLine(sniffBuffer, cursor);
        cursor = probe.nextOffset;
        if (probe.line.empty())
        {
            continue;
        }
        // padded_string owns a copy with the padding simdjson reads past the end. simdjson::pad would count trailing
        // whitespace of the line as padding and let the parser read beyond the string.
        const simdjson::padded_string padded(probe.line);
        simdjson::ondemand::parser parser;
        auto doc = parser.iterate(padded);
        return !doc.get_object().error();
    }
    return false;
}

namespace
{

/**
 * The JSON decoder of the streaming parse loop. It owns the simdjson parser, a padded copy of the line, and the per-key
 * type cache, so successive lines reuse them. A stream line is always copied, because a producer's buffer has no
 * padding after the line.
 */
class JsonLineDecoder
{
public:
    JsonLineDecoder() = default;

    /**
     * Decodes one line into compact values whose OwnedString payloads point into outOwnedArena. It returns Emit for a
     * row, and an empty object still gives a row with no fields. On a parse failure it returns Error with the message
     * in errorOut. It never returns Skip, because JSON has no header line.
     */
    internal::LineDecodeResult DecodeCompact(
        std::string_view line,
        KeyIndex &keys,
        internal::PerWorkerKeyCache &keyCache,
        std::vector<loglib::CompactLogValue> &out,
        std::string &outOwnedArena,
        std::string &errorOut
    )
    {
        out.clear();
        outOwnedArena.clear();
        if (line.empty())
        {
            return internal::LineDecodeResult::Emit;
        }

        try
        {
            const size_t needed = line.size() + simdjson::SIMDJSON_PADDING;
            if (line.size() > mMaxLineSize || mLinePadded.size() < needed)
            {
                mMaxLineSize = std::max(mMaxLineSize, line.size());
                mLinePadded.resize(mMaxLineSize + simdjson::SIMDJSON_PADDING + LINE_PADDED_EXTRA_SLACK_BYTES);
            }
            std::memcpy(mLinePadded.data(), line.data(), line.size());
            std::memset(mLinePadded.data() + line.size(), 0, simdjson::SIMDJSON_PADDING);

            auto result = mParser.iterate(mLinePadded.data(), line.size(), mLinePadded.size());
            if (result.error())
            {
                errorOut = std::string(simdjson::error_message(result.error()));
                return internal::LineDecodeResult::Error;
            }

            auto object = result.get_object();
            if (object.error())
            {
                errorOut = "Not a JSON object.";
                return internal::LineDecodeResult::Error;
            }

            auto objectValue = object.value();
            // sourceIsStable=false: every string is copied into the
            // owned arena. fileBegin/fileSize=0 prevents
            // `MakeStringCompact` from mistaking a padded-buffer view
            // for an mmap slice.
            simdjson::error_code parseError = simdjson::SUCCESS;
            ParseJsonLine(
                out,
                objectValue,
                keys,
                mCache,
                /* sourceIsStable = */ false,
                keyCache,
                /* fileBegin = */ nullptr,
                /* fileSize = */ 0,
                outOwnedArena,
                parseError
            );
            if (parseError)
            {
                errorOut = std::string(simdjson::error_message(parseError));
                return internal::LineDecodeResult::Error;
            }
            return internal::LineDecodeResult::Emit;
        }
        catch (const std::exception &e)
        {
            errorOut = std::string(e.what());
            return internal::LineDecodeResult::Error;
        }
    }

private:
    simdjson::ondemand::parser mParser;
    std::string mLinePadded;
    size_t mMaxLineSize = 0;
    ParseCache mCache;
};

static_assert(
    internal::CompactLineDecoder<JsonLineDecoder>, "JsonLineDecoder must satisfy the CompactLineDecoder concept"
);

} // namespace

void JsonParser::ParseStreaming(StreamLineSource &source, LogParseSink &sink, ParserOptions options) const
{
    JsonLineDecoder decoder;
    internal::RunStreamingParseLoop(source, decoder, sink, options);
}

void JsonParser::ParseStreaming(FileLineSource &source, LogParseSink &sink, ParserOptions options) const
{
    const std::span<const char> bytes = source.StableBytes();
    const size_t batchSize = options.batchSizeBytes != 0 ? options.batchSizeBytes : DEFAULT_PARSE_BATCH_BYTES;

    const char *fileBegin = bytes.data();
    const size_t fileSize = bytes.size();
    const char *fileEnd = (fileBegin != nullptr) ? fileBegin + fileSize : nullptr;
    auto stageA = internal::MakeLineAlignedBatcher(fileBegin, fileEnd, batchSize);

    FileLineSource *sourcePtr = &source;
    auto stageB = [sourcePtr](
                      internal::ByteRange token,
                      internal::WorkerScratch<JsonWorkerState> &worker,
                      KeyIndex &keys,
                      std::span<const internal::TimeColumnSpec> timeColumns,
                      internal::ParsedPipelineBatch &parsed
                  ) { DecodeJsonBatch(token, worker, keys, *sourcePtr, timeColumns, parsed); };

    internal::RunStaticParserPipeline<internal::ByteRange, JsonWorkerState>(source, sink, options, stageA, stageB);
}

} // namespace loglib
