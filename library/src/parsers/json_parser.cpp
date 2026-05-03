#include "loglib/parsers/json_parser.hpp"

#include "loglib/bytes_producer.hpp"
#include "loglib/file_line_source.hpp"
#include "loglib/internal/compact_log_value.hpp"
#include "loglib/internal/line_decoder.hpp"
#include "loglib/internal/advanced_parser_options.hpp"
#include "loglib/internal/static_parser_pipeline.hpp"
#include "loglib/internal/streaming_parse_loop.hpp"
#include "loglib/internal/timestamp_promotion.hpp"
#include "loglib/log_configuration.hpp"
#include "loglib/log_file.hpp"
#include "loglib/log_line.hpp"
#include "loglib/log_processing.hpp"
#include "loglib/stream_line_source.hpp"

#include <date/date.h>
#include <fmt/format.h>
#include <glaze/glaze.hpp>

#include <simdjson.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace loglib
{

namespace
{

/// JSON field key extracted via the no-escape fast path (`isView == true`)
/// or the `unescaped_key()` slow path (`owned`).
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

    if (escaped.find('\\') == std::string_view::npos)
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

/// Crossover at which `InsertSorted` switches from a linear back-scan to
/// `std::lower_bound`. Tuned for the `[wide]` benchmark.
constexpr size_t kInsertSortedLowerBoundThreshold = 8;

void InsertSorted(std::vector<std::pair<KeyId, internal::CompactLogValue>> &out, KeyId id, internal::CompactLogValue value)
{
    if (out.size() < kInsertSortedLowerBoundThreshold)
    {
        auto it = out.end();
        while (it != out.begin())
        {
            auto prev = it - 1;
            if (prev->first < id)
            {
                break;
            }
            if (prev->first == id)
            {
                prev->second = value;
                return;
            }
            it = prev;
        }
        out.emplace(it, id, value);
        return;
    }

    auto it = std::lower_bound(
        out.begin(),
        out.end(),
        id,
        [](const std::pair<KeyId, internal::CompactLogValue> &lhs, KeyId rhs) { return lhs.first < rhs; }
    );
    if (it != out.end() && it->first == id)
    {
        it->second = value;
        return;
    }
    out.emplace(it, id, value);
}

/// Promote a string view into a compact value. When @p sv aliases the
/// mmap (i.e. lies inside `[fileBegin, fileBegin + fileSize)`), the
/// resulting tag is `MmapSlice` (zero copy). Otherwise the bytes are
/// appended to @p ownedArena and the tag is `OwnedString`.
internal::CompactLogValue MakeStringCompact(
    std::string_view sv, const char *fileBegin, size_t fileSize, std::string &ownedArena
)
{
    if (sv.data() >= fileBegin && sv.data() + sv.size() <= fileBegin + fileSize)
    {
        const auto offset = static_cast<uint64_t>(sv.data() - fileBegin);
        return internal::CompactLogValue::MakeMmapSlice(offset, static_cast<uint32_t>(sv.size()));
    }
    const auto offset = static_cast<uint64_t>(ownedArena.size());
    ownedArena.append(sv.data(), sv.size());
    return internal::CompactLogValue::MakeOwnedString(offset, static_cast<uint32_t>(sv.size()));
}

template <class Value>
internal::CompactLogValue ExtractStringValue(
    Value &value, bool sourceIsStable, const char *fileBegin, size_t fileSize, std::string &ownedArena
)
{
    // string_view path is only safe when simdjson iterates directly over the
    // mmap; the padded-scratch fallback yields views that dangle on the next line.
    // `raw_json_token()`/`raw_json()` aliasing into source is pinned to
    // simdjson v4.6.3 (see `cmake/FetchDependencies.cmake`); re-verify on bump
    // — the `[allocations]` benchmark's fast-path fraction is the regression signal.
    if (sourceIsStable)
    {
        std::string_view rawToken(value.raw_json_token());
        if (rawToken.size() >= 2 && rawToken.front() == '"' && rawToken.back() == '"')
        {
            std::string_view inner = rawToken.substr(1, rawToken.size() - 2);
            if (inner.find('\\') == std::string_view::npos)
            {
                return MakeStringCompact(inner, fileBegin, fileSize, ownedArena);
            }
        }
    }

    std::string_view stringValue;
    if (!value.get_string().get(stringValue))
    {
        // `get_string()` on the padded-scratch fallback yields bytes inside
        // the worker's scratch buffer (not the mmap), so `MakeStringCompact`
        // will fall through to the `OwnedString` arena path. Same as the
        // pre-Phase-1 `LogValue{std::string(stringValue)}` semantics.
        return MakeStringCompact(stringValue, fileBegin, fileSize, ownedArena);
    }
    return internal::CompactLogValue::MakeMonostate();
}

template <class Value>
internal::CompactLogValue ExtractRawJsonValue(
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
        // Padded-scratch fallback: `rawJson` aliases the worker's scratch
        // buffer, copy into the per-batch arena.
        const auto offset = static_cast<uint64_t>(ownedArena.size());
        ownedArena.append(rawJson.data(), rawJson.size());
        return internal::CompactLogValue::MakeOwnedString(offset, static_cast<uint32_t>(rawJson.size()));
    }
    return internal::CompactLogValue::MakeMonostate();
}

/// Per-key simdjson type cache so `value.type()` / `get_number_type()` fire
/// only on the first occurrence of each key.
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

std::vector<std::pair<KeyId, internal::CompactLogValue>> ParseJsonLine(
    simdjson::ondemand::object &object,
    KeyIndex &keys,
    ParseCache &cache,
    bool sourceIsStable,
    internal::PerWorkerKeyCache *keyCache,
    const char *fileBegin,
    size_t fileSize,
    std::string &ownedArena
)
{
    std::vector<std::pair<KeyId, internal::CompactLogValue>> result;
    result.reserve(16);

    for (auto field : object)
    {
        FastFieldKey fk = ExtractFieldKey(field);
        if (!fk.isView && fk.owned.empty())
        {
            continue;
        }

        const KeyId keyId = fk.isView ? internal::InternKeyVia(fk.view, keys, keyCache) : keys.GetOrInsert(fk.owned);
        EnsureCacheCapacity(cache, keyId);

        auto value = field.value();

        if (cache.hasKeyType[keyId])
        {
            const auto type = cache.keyTypes[keyId];
            switch (type)
            {
            case simdjson::ondemand::json_type::boolean: {
                bool b;
                if (!value.get(b))
                {
                    InsertSorted(result, keyId, internal::CompactLogValue::MakeBool(b));
                    continue;
                }
                break;
            }
            case simdjson::ondemand::json_type::number: {
                if (cache.hasNumberType[keyId])
                {
                    switch (cache.numberTypes[keyId])
                    {
                    case simdjson::ondemand::number_type::signed_integer: {
                        int64_t i;
                        if (!value.get(i))
                        {
                            InsertSorted(result, keyId, internal::CompactLogValue::MakeInt64(i));
                            continue;
                        }
                        break;
                    }
                    case simdjson::ondemand::number_type::unsigned_integer: {
                        uint64_t u;
                        if (!value.get(u))
                        {
                            InsertSorted(result, keyId, internal::CompactLogValue::MakeUint64(u));
                            continue;
                        }
                        break;
                    }
                    case simdjson::ondemand::number_type::floating_point_number: {
                        double d;
                        if (!value.get(d))
                        {
                            InsertSorted(result, keyId, internal::CompactLogValue::MakeDouble(d));
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
            case simdjson::ondemand::json_type::string: {
                internal::CompactLogValue stringValue =
                    ExtractStringValue(value, sourceIsStable, fileBegin, fileSize, ownedArena);
                if (stringValue.tag != internal::CompactTag::Monostate)
                {
                    InsertSorted(result, keyId, stringValue);
                    continue;
                }
                break;
            }
            case simdjson::ondemand::json_type::array:
            case simdjson::ondemand::json_type::object: {
                internal::CompactLogValue rawValue =
                    ExtractRawJsonValue(value, sourceIsStable, fileBegin, fileSize, ownedArena);
                if (rawValue.tag != internal::CompactTag::Monostate)
                {
                    InsertSorted(result, keyId, rawValue);
                    continue;
                }
                break;
            }
            case simdjson::ondemand::json_type::null:
                if (value.is_null())
                {
                    InsertSorted(result, keyId, internal::CompactLogValue::MakeMonostate());
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
            InsertSorted(result, keyId, internal::CompactLogValue::MakeMonostate());
            continue;
        }

        const auto type = typeResult.value();
        cache.keyTypes[keyId] = type;
        cache.hasKeyType[keyId] = true;

        switch (type)
        {
        case simdjson::ondemand::json_type::boolean: {
            bool b;
            if (!value.get(b))
            {
                InsertSorted(result, keyId, internal::CompactLogValue::MakeBool(b));
            }
            else
            {
                InsertSorted(result, keyId, internal::CompactLogValue::MakeMonostate());
            }
            break;
        }
        case simdjson::ondemand::json_type::number: {
            auto numberType = value.get_number_type();
            if (numberType.error())
            {
                InsertSorted(result, keyId, internal::CompactLogValue::MakeMonostate());
                break;
            }
            cache.numberTypes[keyId] = numberType.value();
            cache.hasNumberType[keyId] = true;
            switch (numberType.value())
            {
            case simdjson::ondemand::number_type::signed_integer: {
                int64_t i;
                if (!value.get(i))
                {
                    InsertSorted(result, keyId, internal::CompactLogValue::MakeInt64(i));
                }
                else
                {
                    InsertSorted(result, keyId, internal::CompactLogValue::MakeMonostate());
                }
                break;
            }
            case simdjson::ondemand::number_type::unsigned_integer: {
                uint64_t u;
                if (!value.get(u))
                {
                    InsertSorted(result, keyId, internal::CompactLogValue::MakeUint64(u));
                }
                else
                {
                    InsertSorted(result, keyId, internal::CompactLogValue::MakeMonostate());
                }
                break;
            }
            case simdjson::ondemand::number_type::floating_point_number: {
                double d;
                if (!value.get(d))
                {
                    InsertSorted(result, keyId, internal::CompactLogValue::MakeDouble(d));
                }
                else
                {
                    InsertSorted(result, keyId, internal::CompactLogValue::MakeMonostate());
                }
                break;
            }
            default:
                InsertSorted(result, keyId, internal::CompactLogValue::MakeMonostate());
                break;
            }
            break;
        }
        case simdjson::ondemand::json_type::string: {
            internal::CompactLogValue stringValue =
                ExtractStringValue(value, sourceIsStable, fileBegin, fileSize, ownedArena);
            InsertSorted(result, keyId, stringValue);
            break;
        }
        case simdjson::ondemand::json_type::array:
        case simdjson::ondemand::json_type::object: {
            internal::CompactLogValue rawValue =
                ExtractRawJsonValue(value, sourceIsStable, fileBegin, fileSize, ownedArena);
            InsertSorted(result, keyId, rawValue);
            break;
        }
        case simdjson::ondemand::json_type::null:
            InsertSorted(result, keyId, internal::CompactLogValue::MakeMonostate());
            break;
        default:
            InsertSorted(result, keyId, internal::CompactLogValue::MakeMonostate());
            break;
        }
    }

    return result;
}

void EncodeLogValue(glz::generic_sorted_u64 &json, const std::string &key, const LogValue &value)
{
    std::visit(
        [&json, &key](const auto &val) {
            using T = std::decay_t<decltype(val)>;
            if constexpr (std::is_same_v<T, std::monostate>)
            {
                json[key] = nullptr;
            }
            else if constexpr (std::is_same_v<T, TimeStamp>)
            {
                json[key] = TimeStampToDateTimeString(val);
            }
            else if constexpr (std::is_same_v<T, std::string_view>)
            {
                json[key] = std::string(val);
            }
            else
            {
                json[key] = val;
            }
        },
        value
    );
}

std::string SerializeJson(const glz::generic_sorted_u64 &json)
{
    std::string result;
    const auto error = glz::write_json(json, result);
    if (error)
    {
        throw std::runtime_error(fmt::format("Failed to serialize JSON: {}", glz::format_error(error)));
    }
    return result;
}

/// JSON-specific per-worker scratch.
struct JsonWorkerState
{
    simdjson::ondemand::parser parser;
    std::string linePadded;
    size_t maxLineSize = 0;
    ParseCache cache;
};

/// Stage A token: a contiguous byte range of the mmap covering complete lines.
struct JsonByteRange
{
    uint64_t batchIndex = 0;
    const char *bytesBegin = nullptr;
    const char *bytesEnd = nullptr;
    const char *fileEnd = nullptr;
};

void DecodeJsonBatch(
    const JsonByteRange &batch,
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
    const char *fileBegin = source.File().Data();

    const size_t batchBytes = static_cast<size_t>(end - cursor);
    const size_t estimatedLines = batchBytes / 64 + 1;
    parsed.lines.reserve(estimatedLines);
    parsed.localLineOffsets.reserve(estimatedLines);

    size_t relativeLineNumber = 1;

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

        // `GetLine` subtracts 1 (the '\n') from the slice length; for an
        // unterminated final line, push `fileSize + 1` as the sentinel so
        // the last character is not lopped off.
        const uint64_t nextOffset = static_cast<uint64_t>(cursor - fileBegin) + (newline == nullptr ? 1u : 0u);
        parsed.localLineOffsets.push_back(nextOffset);

        if (line.empty())
        {
            relativeLineNumber++;
            continue;
        }

        try
        {
            // mmap fast path requires SIMDJSON_PADDING bytes of slack past lineEnd;
            // otherwise fall back to a per-worker padded copy.
            const size_t remaining = static_cast<size_t>(fileEnd - lineEnd);
            const bool sourceIsStable = remaining >= simdjson::SIMDJSON_PADDING;
            auto result =
                sourceIsStable ? worker.user.parser.iterate(line.data(), line.size(), line.size() + remaining) : [&]() {
                    const size_t needed = line.size() + simdjson::SIMDJSON_PADDING;
                    if (line.size() > worker.user.maxLineSize || worker.user.linePadded.size() < needed)
                    {
                        worker.user.maxLineSize = std::max(worker.user.maxLineSize, line.size());
                        worker.user.linePadded.resize(worker.user.maxLineSize + simdjson::SIMDJSON_PADDING + 64);
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
                    internal::ParsedLineError{relativeLineNumber, std::string(simdjson::error_message(result.error()))}
                );
                relativeLineNumber++;
                continue;
            }

            auto object = result.get_object();
            if (object.error())
            {
                parsed.errors.push_back(internal::ParsedLineError{relativeLineNumber, "Not a JSON object."});
                relativeLineNumber++;
                continue;
            }

            auto objectValue = object.value();
            const size_t fileSize = static_cast<size_t>(fileEnd - fileBegin);
            auto values = ParseJsonLine(
                objectValue,
                keys,
                worker.user.cache,
                sourceIsStable,
                &worker.keyCache,
                fileBegin,
                fileSize,
                parsed.ownedStringsArena
            );

            // Stamp the 0-based offset of this line within the batch so that
            // `LogFile::GetLine(LineId())` indexes the correct row in
            // `mLineOffsets` after Stage C shifts by the running cursor.
            // `relativeLineNumber` itself stays 1-based for the human-facing
            // error messages above.
            LogLine logLine(std::move(values), keys, source, relativeLineNumber - 1);

            parsed.lines.push_back(std::move(logLine));

            // Promote inline so the freshly-written values are still hot in L1.
            // The line's `OwnedString` payloads still index into the per-batch
            // arena (Stage C rebases later), so pass that arena explicitly.
            worker.PromoteTimestamps(parsed.lines.back(), timeColumns, std::string_view(parsed.ownedStringsArena));
        }
        catch (const std::exception &e)
        {
            parsed.errors.push_back(internal::ParsedLineError{relativeLineNumber, std::string(e.what())});
        }

        relativeLineNumber++;
    }

    parsed.totalLineCount = relativeLineNumber - 1;
}

} // namespace

bool JsonParser::IsValid(const std::filesystem::path &file) const
{
    std::ifstream stream(file);
    if (!stream.is_open())
    {
        return false;
    }

    std::string line;
    while (std::getline(stream, line))
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        if (!line.empty())
        {
            simdjson::ondemand::parser parser;
            auto doc = parser.iterate(simdjson::pad(line));
            return !doc.get_object().error();
        }
    }

    return false;
}

std::string JsonParser::ToString(const LogLine &line) const
{
    const auto values = line.IndexedValues();
    if (values.empty())
    {
        return "{}";
    }

    glz::generic_sorted_u64 json;

    const auto &keys = line.Keys();
    for (const auto &entry : values)
    {
        EncodeLogValue(json, std::string(keys.KeyOf(entry.first)), entry.second);
    }

    return SerializeJson(json);
}

std::string JsonParser::ToString(const LogMap &values) const
{
    if (values.empty())
    {
        return "{}";
    }

    glz::generic_sorted_u64 json;
    for (const auto &[key, value] : values)
    {
        EncodeLogValue(json, key, value);
    }

    return SerializeJson(json);
}

namespace
{

/// JSON record decoder for the format-agnostic streaming pipeline.
/// Owns the simdjson parser, padded-line scratch buffer (the streaming
/// path always falls back to the padded-copy shape because
/// `BytesProducer::Read` does not promise the `simdjson::SIMDJSON_PADDING`
/// byte slack the mmap fast path relies on), and the per-key type
/// cache so successive lines reuse the same allocations. Satisfies
/// `internal::CompactLineDecoder` (the unified `LogLine`-emitting
/// pipeline `RunStreamingParseLoop`).
class JsonLineDecoder
{
public:
    JsonLineDecoder() = default;

    /// `CompactLineDecoder` concept entry point — fills @p out with
    /// pre-built `CompactLogValue`s whose `OwnedString` payloads carry
    /// offsets into @p outOwnedArena, so the pipeline can `AppendLine`
    /// the arena into the `StreamLineSource` and resolve fields
    /// through the source's `ResolveOwnedBytes` afterwards. The
    /// streaming pipeline filters blank lines before getting here, so
    /// the @p line precondition is "non-empty after `\r` trimming".
    /// On parse error returns false and writes a human-facing message
    /// to @p errorOut (the pipeline composes the absolute "Error on
    /// line N: ..." wrapper).
    bool DecodeCompact(
        std::string_view line,
        KeyIndex &keys,
        internal::PerWorkerKeyCache *keyCache,
        std::vector<std::pair<KeyId, internal::CompactLogValue>> &out,
        std::string &outOwnedArena,
        std::string &errorOut
    )
    {
        out.clear();
        outOwnedArena.clear();
        if (line.empty())
        {
            return true;
        }

        try
        {
            const size_t needed = line.size() + simdjson::SIMDJSON_PADDING;
            if (line.size() > mMaxLineSize || mLinePadded.size() < needed)
            {
                mMaxLineSize = std::max(mMaxLineSize, line.size());
                mLinePadded.resize(mMaxLineSize + simdjson::SIMDJSON_PADDING + 64);
            }
            std::memcpy(mLinePadded.data(), line.data(), line.size());
            std::memset(mLinePadded.data() + line.size(), 0, simdjson::SIMDJSON_PADDING);

            auto result = mParser.iterate(mLinePadded.data(), line.size(), mLinePadded.size());
            if (result.error())
            {
                errorOut = std::string(simdjson::error_message(result.error()));
                return false;
            }

            auto object = result.get_object();
            if (object.error())
            {
                errorOut = "Not a JSON object.";
                return false;
            }

            auto objectValue = object.value();
            // `sourceIsStable = false`: the streaming caller does not
            // give the parser a stable view of the source bytes, so
            // every string is copied into @p outOwnedArena. `fileBegin = nullptr` /
            // `fileSize = 0` ensures `MakeStringCompact` cannot mistake
            // a padded-buffer view for a mmap slice.
            out = ParseJsonLine(
                objectValue,
                keys,
                mCache,
                /* sourceIsStable = */ false,
                keyCache,
                /* fileBegin = */ nullptr,
                /* fileSize = */ 0,
                outOwnedArena
            );
            return true;
        }
        catch (const std::exception &e)
        {
            errorOut = std::string(e.what());
            return false;
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
    // The unified streaming path: emits `LogLine`s tagged with @p
    // source so `LogTable` can hold them in a single row vector
    // alongside the static-file `LogLine`s. The `JsonLineDecoder`
    // satisfies `CompactLineDecoder` (the unified shape that produces
    // pre-built `CompactLogValue`s + a per-line owned arena), and
    // `RunStreamingParseLoop` move-transfers each arena into
    // the source on every line so the resulting `LogLine`'s
    // `OwnedString` payloads resolve against the source's canonical
    // arena from the moment of construction.
    JsonLineDecoder decoder;
    internal::RunStreamingParseLoop(source, decoder, sink, options);
}

void JsonParser::ParseStreaming(FileLineSource &source, LogParseSink &sink, ParserOptions options) const
{
    ParseStreaming(source, sink, std::move(options), internal::AdvancedParserOptions{});
}

void JsonParser::ParseStreaming(
    FileLineSource &source, LogParseSink &sink, ParserOptions options, internal::AdvancedParserOptions advanced
) const
{
    LogFile &file = source.File();
    const size_t batchSize = advanced.batchSizeBytes != 0 ? advanced.batchSizeBytes
                                                          : internal::AdvancedParserOptions::kDefaultBatchSizeBytes;

    const char *fileBegin = file.Data();
    const size_t fileSize = file.Size();
    const char *fileEnd = (fileBegin != nullptr) ? fileBegin + fileSize : nullptr;
    const char *cursor = fileBegin;
    uint64_t batchIndex = 0;

    auto stageA = [cursor, fileEnd, batchSize, batchIndex](JsonByteRange &out) mutable -> bool {
        if (cursor >= fileEnd)
        {
            return false;
        }
        const char *batchBegin = cursor;
        // Size-bounded advance: `cursor + batchSize` is UB past one-past-end.
        const size_t remaining = static_cast<size_t>(fileEnd - cursor);
        const size_t advance = std::min(batchSize, remaining);
        const char *target = cursor + advance;
        if (advance < remaining)
        {
            const char *newline =
                static_cast<const char *>(memchr(target, '\n', static_cast<size_t>(fileEnd - target)));
            cursor = (newline != nullptr) ? newline + 1 : fileEnd;
        }
        else
        {
            cursor = fileEnd;
        }

        out.batchIndex = batchIndex++;
        out.bytesBegin = batchBegin;
        out.bytesEnd = cursor;
        out.fileEnd = fileEnd;
        return true;
    };

    FileLineSource *sourcePtr = &source;
    auto stageB = [sourcePtr](
                      JsonByteRange token,
                      internal::WorkerScratch<JsonWorkerState> &worker,
                      KeyIndex &keys,
                      std::span<const internal::TimeColumnSpec> timeColumns,
                      internal::ParsedPipelineBatch &parsed
                  ) { DecodeJsonBatch(token, worker, keys, *sourcePtr, timeColumns, parsed); };

    internal::RunStaticParserPipeline<JsonByteRange, JsonWorkerState>(source, sink, options, advanced, stageA, stageB);
}

} // namespace loglib
