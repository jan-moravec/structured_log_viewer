#include "loglib/json_parser.hpp"

#include "loglib/internal/compact_log_value.hpp"
#include "loglib/internal/parser_options.hpp"
#include "loglib/internal/parser_pipeline.hpp"
#include "loglib/internal/timestamp_promotion.hpp"
#include "loglib/log_configuration.hpp"
#include "loglib/log_file.hpp"
#include "loglib/log_line.hpp"
#include "loglib/log_processing.hpp"
#include "loglib/log_source.hpp"
#include "loglib/stream_log_line.hpp"

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

void InsertSorted(std::vector<std::pair<KeyId, detail::CompactLogValue>> &out, KeyId id, detail::CompactLogValue value)
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
        [](const std::pair<KeyId, detail::CompactLogValue> &lhs, KeyId rhs) { return lhs.first < rhs; }
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
detail::CompactLogValue MakeStringCompact(
    std::string_view sv, const char *fileBegin, size_t fileSize, std::string &ownedArena
)
{
    if (sv.data() >= fileBegin && sv.data() + sv.size() <= fileBegin + fileSize)
    {
        const auto offset = static_cast<uint64_t>(sv.data() - fileBegin);
        return detail::CompactLogValue::MakeMmapSlice(offset, static_cast<uint32_t>(sv.size()));
    }
    const auto offset = static_cast<uint64_t>(ownedArena.size());
    ownedArena.append(sv.data(), sv.size());
    return detail::CompactLogValue::MakeOwnedString(offset, static_cast<uint32_t>(sv.size()));
}

template <class Value>
detail::CompactLogValue ExtractStringValue(
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
    return detail::CompactLogValue::MakeMonostate();
}

template <class Value>
detail::CompactLogValue ExtractRawJsonValue(
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
        return detail::CompactLogValue::MakeOwnedString(offset, static_cast<uint32_t>(rawJson.size()));
    }
    return detail::CompactLogValue::MakeMonostate();
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

std::vector<std::pair<KeyId, detail::CompactLogValue>> ParseJsonLine(
    simdjson::ondemand::object &object,
    KeyIndex &keys,
    ParseCache &cache,
    bool sourceIsStable,
    detail::PerWorkerKeyCache *keyCache,
    const char *fileBegin,
    size_t fileSize,
    std::string &ownedArena
)
{
    std::vector<std::pair<KeyId, detail::CompactLogValue>> result;
    result.reserve(16);

    for (auto field : object)
    {
        FastFieldKey fk = ExtractFieldKey(field);
        if (!fk.isView && fk.owned.empty())
        {
            continue;
        }

        const KeyId keyId = fk.isView ? detail::InternKeyVia(fk.view, keys, keyCache) : keys.GetOrInsert(fk.owned);
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
                    InsertSorted(result, keyId, detail::CompactLogValue::MakeBool(b));
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
                            InsertSorted(result, keyId, detail::CompactLogValue::MakeInt64(i));
                            continue;
                        }
                        break;
                    }
                    case simdjson::ondemand::number_type::unsigned_integer: {
                        uint64_t u;
                        if (!value.get(u))
                        {
                            InsertSorted(result, keyId, detail::CompactLogValue::MakeUint64(u));
                            continue;
                        }
                        break;
                    }
                    case simdjson::ondemand::number_type::floating_point_number: {
                        double d;
                        if (!value.get(d))
                        {
                            InsertSorted(result, keyId, detail::CompactLogValue::MakeDouble(d));
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
                detail::CompactLogValue stringValue =
                    ExtractStringValue(value, sourceIsStable, fileBegin, fileSize, ownedArena);
                if (stringValue.tag != detail::CompactTag::Monostate)
                {
                    InsertSorted(result, keyId, stringValue);
                    continue;
                }
                break;
            }
            case simdjson::ondemand::json_type::array:
            case simdjson::ondemand::json_type::object: {
                detail::CompactLogValue rawValue =
                    ExtractRawJsonValue(value, sourceIsStable, fileBegin, fileSize, ownedArena);
                if (rawValue.tag != detail::CompactTag::Monostate)
                {
                    InsertSorted(result, keyId, rawValue);
                    continue;
                }
                break;
            }
            case simdjson::ondemand::json_type::null:
                if (value.is_null())
                {
                    InsertSorted(result, keyId, detail::CompactLogValue::MakeMonostate());
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
            InsertSorted(result, keyId, detail::CompactLogValue::MakeMonostate());
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
                InsertSorted(result, keyId, detail::CompactLogValue::MakeBool(b));
            }
            else
            {
                InsertSorted(result, keyId, detail::CompactLogValue::MakeMonostate());
            }
            break;
        }
        case simdjson::ondemand::json_type::number: {
            auto numberType = value.get_number_type();
            if (numberType.error())
            {
                InsertSorted(result, keyId, detail::CompactLogValue::MakeMonostate());
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
                    InsertSorted(result, keyId, detail::CompactLogValue::MakeInt64(i));
                }
                else
                {
                    InsertSorted(result, keyId, detail::CompactLogValue::MakeMonostate());
                }
                break;
            }
            case simdjson::ondemand::number_type::unsigned_integer: {
                uint64_t u;
                if (!value.get(u))
                {
                    InsertSorted(result, keyId, detail::CompactLogValue::MakeUint64(u));
                }
                else
                {
                    InsertSorted(result, keyId, detail::CompactLogValue::MakeMonostate());
                }
                break;
            }
            case simdjson::ondemand::number_type::floating_point_number: {
                double d;
                if (!value.get(d))
                {
                    InsertSorted(result, keyId, detail::CompactLogValue::MakeDouble(d));
                }
                else
                {
                    InsertSorted(result, keyId, detail::CompactLogValue::MakeMonostate());
                }
                break;
            }
            default:
                InsertSorted(result, keyId, detail::CompactLogValue::MakeMonostate());
                break;
            }
            break;
        }
        case simdjson::ondemand::json_type::string: {
            detail::CompactLogValue stringValue =
                ExtractStringValue(value, sourceIsStable, fileBegin, fileSize, ownedArena);
            InsertSorted(result, keyId, stringValue);
            break;
        }
        case simdjson::ondemand::json_type::array:
        case simdjson::ondemand::json_type::object: {
            detail::CompactLogValue rawValue =
                ExtractRawJsonValue(value, sourceIsStable, fileBegin, fileSize, ownedArena);
            InsertSorted(result, keyId, rawValue);
            break;
        }
        case simdjson::ondemand::json_type::null:
            InsertSorted(result, keyId, detail::CompactLogValue::MakeMonostate());
            break;
        default:
            InsertSorted(result, keyId, detail::CompactLogValue::MakeMonostate());
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
    detail::WorkerScratch<JsonWorkerState> &worker,
    KeyIndex &keys,
    LogFile &logFile,
    std::span<const detail::TimeColumnSpec> timeColumns,
    detail::ParsedPipelineBatch &parsed
)
{
    parsed.batchIndex = batch.batchIndex;

    const char *cursor = batch.bytesBegin;
    const char *end = batch.bytesEnd;
    const char *fileEnd = batch.fileEnd;
    const char *fileBegin = logFile.Data();

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
                    detail::ParsedLineError{relativeLineNumber, std::string(simdjson::error_message(result.error()))}
                );
                relativeLineNumber++;
                continue;
            }

            auto object = result.get_object();
            if (object.error())
            {
                parsed.errors.push_back(detail::ParsedLineError{relativeLineNumber, "Not a JSON object."});
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

            LogFileReference fileRef(logFile, 0);
            LogLine logLine(std::move(values), keys, std::move(fileRef));
            // Stamp the 0-based offset of this line within the batch so that
            // `LogFile::GetLine(GetLineNumber())` indexes the correct row in
            // `mLineOffsets` after Stage C shifts by the running cursor.
            // `relativeLineNumber` itself stays 1-based for the human-facing
            // error messages above.
            logLine.FileReference().SetLineNumber(relativeLineNumber - 1);

            parsed.lines.push_back(std::move(logLine));

            // Promote inline so the freshly-written values are still hot in L1.
            // The line's `OwnedString` payloads still index into the per-batch
            // arena (Stage C rebases later), so pass that arena explicitly.
            worker.PromoteTimestamps(parsed.lines.back(), timeColumns, std::string_view(parsed.ownedStringsArena));
        }
        catch (const std::exception &e)
        {
            parsed.errors.push_back(detail::ParsedLineError{relativeLineNumber, std::string(e.what())});
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

/// Coalescing thresholds for the non-mmap streaming loop. Smaller than the
/// TBB pipeline's `kStreamFlushLines = 1000` / `kStreamFlushInterval = 50 ms`
/// because the live-tail target is end-to-end latency (≤ 250 ms p50 / ≤ 500
/// ms p95 per PRD §8 success metric 1) rather than million-line throughput.
/// PRD §7 *Batching and latency* allows up to 250 lines / 100 ms here.
constexpr size_t kStreamLoopFlushLines = 250;
constexpr auto kStreamLoopFlushInterval = std::chrono::milliseconds(100);

/// Read buffer size for the non-mmap streaming loop. 64 KiB matches the
/// pre-fill chunk size in `TailingFileSource` (PRD 4.1.6) and is small
/// enough that each `LogSource::Read` returns within a couple of poll
/// ticks even on slow CI runners.
constexpr size_t kStreamReadBufferSize = 64 * 1024;

/// Convert a `CompactLogValue` produced by `ParseJsonLine` (with
/// `sourceIsStable = false`) back into a public `LogValue`. `MmapSlice`
/// can never appear in this branch — the streaming caller passes a null
/// `fileBegin`, so all string-shaped values land in `OwnedString` form
/// referencing offsets in the per-line @p ownedArena.
LogValue MaterialiseStreamCompactValue(const detail::CompactLogValue &value, std::string_view ownedArena)
{
    using detail::CompactTag;
    switch (value.tag)
    {
    case CompactTag::Monostate:
        return LogValue{std::monostate{}};
    case CompactTag::OwnedString: {
        if (static_cast<size_t>(value.payload) + value.aux > ownedArena.size())
        {
            return LogValue{std::string{}};
        }
        return LogValue{std::string(ownedArena.data() + value.payload, value.aux)};
    }
    case CompactTag::Int64:
        return LogValue{static_cast<int64_t>(value.payload)};
    case CompactTag::Uint64:
        return LogValue{value.payload};
    case CompactTag::Double:
        return LogValue{std::bit_cast<double>(value.payload)};
    case CompactTag::Bool:
        return LogValue{value.payload != 0};
    case CompactTag::Timestamp:
        return LogValue{TimeStamp{std::chrono::microseconds{static_cast<int64_t>(value.payload)}}};
    case CompactTag::MmapSlice:
        // Defensive: should not happen in the streaming branch, but a
        // future caller passing a non-null `fileBegin` would produce
        // these. Treat as monostate to avoid pointer-into-source bugs.
        return LogValue{std::monostate{}};
    }
    return LogValue{std::monostate{}};
}

/// Per-line scratch shared by the streaming loop. Holds the simdjson
/// parser, padded-line buffer (the streaming path always falls back to
/// the padded-copy shape because `LogSource::Read` does not promise the
/// `simdjson::SIMDJSON_PADDING` byte slack the mmap fast path relies on),
/// and the parse cache.
struct StreamingLineScratch
{
    simdjson::ondemand::parser parser;
    std::string linePadded;
    size_t maxLineSize = 0;
    ParseCache cache;
    std::string lineOwnedArena;
};

/// Decode a single complete line into a vector of `(KeyId, LogValue)` pairs.
/// Returns false on parse error; on success the @p out vector is populated
/// (sorted by `KeyId`).
bool DecodeStreamLine(
    std::string_view line,
    KeyIndex &keys,
    StreamingLineScratch &scratch,
    detail::PerWorkerKeyCache *keyCache,
    std::vector<std::pair<KeyId, LogValue>> &out,
    std::string &errorOut
)
{
    out.clear();
    if (line.empty())
    {
        return true;
    }

    try
    {
        scratch.lineOwnedArena.clear();
        const size_t needed = line.size() + simdjson::SIMDJSON_PADDING;
        if (line.size() > scratch.maxLineSize || scratch.linePadded.size() < needed)
        {
            scratch.maxLineSize = std::max(scratch.maxLineSize, line.size());
            scratch.linePadded.resize(scratch.maxLineSize + simdjson::SIMDJSON_PADDING + 64);
        }
        std::memcpy(scratch.linePadded.data(), line.data(), line.size());
        std::memset(scratch.linePadded.data() + line.size(), 0, simdjson::SIMDJSON_PADDING);

        auto result = scratch.parser.iterate(scratch.linePadded.data(), line.size(), scratch.linePadded.size());
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
        // `sourceIsStable = false`: the streaming caller does not give the
        // parser a stable view of the source bytes, so every string is
        // copied into `lineOwnedArena` (PRD 4.9.4 — task 2.4 last
        // paragraph). `fileBegin = nullptr` / `fileSize = 0` ensures
        // `MakeStringCompact` cannot mistake a padded-buffer view for a
        // mmap slice.
        auto compactValues = ParseJsonLine(
            objectValue,
            keys,
            scratch.cache,
            /* sourceIsStable = */ false,
            keyCache,
            /* fileBegin = */ nullptr,
            /* fileSize = */ 0,
            scratch.lineOwnedArena
        );

        out.reserve(compactValues.size());
        const std::string_view arenaView(scratch.lineOwnedArena);
        for (const auto &entry : compactValues)
        {
            out.emplace_back(entry.first, MaterialiseStreamCompactValue(entry.second, arenaView));
        }
        return true;
    }
    catch (const std::exception &e)
    {
        errorOut = std::string(e.what());
        return false;
    }
}

} // namespace

void JsonParser::ParseStreaming(LogSource &source, StreamingLogSink &sink, ParserOptions options) const
{
    // Mmap fast path: bypass `LogSource::Read` entirely and drive the TBB
    // pipeline directly over the `LogFile`'s mmap so the static-path
    // `[large]` / `[wide]` / `[allocations]` benchmark headroom is
    // preserved (PRD 4.9.4, task 1.4). Capability detection is the
    // `IsMappedFile()` / `GetMappedLogFile()` virtual pair on `LogSource`;
    // `MappedFileSource` is the only source today that returns true.
    if (source.IsMappedFile())
    {
        if (LogFile *file = source.GetMappedLogFile())
        {
            ParseStreaming(*file, sink, std::move(options), internal::AdvancedParserOptions{});
            return;
        }
    }

    // Non-mmap streaming loop (`TailingFileSource`, future stdin / TCP /
    // UDP). Single-threaded by design — throughput target is thousands of
    // lines/s, not millions, so the TBB pipeline overhead is not warranted
    // and the latency budget (PRD §7 *Batching and latency*) is tighter.
    sink.OnStarted();

    const StopToken stopToken = options.stopToken;
    KeyIndex &keys = sink.Keys();

    // Reuse the existing `BuildTimeColumnSpecs` so the snapshot
    // time-column heuristic (PRD 4.6.2 / OQ-6) carries over to streaming
    // for free.
    const std::vector<detail::TimeColumnSpec> timeColumns =
        detail::BuildTimeColumnSpecs(keys, options.configuration.get());
    std::span<const detail::TimeColumnSpec> timeColumnsSpan(timeColumns);

    detail::WorkerScratchBase promoteScratch;
    promoteScratch.EnsureTimeColumnCapacity(timeColumns.size());

    StreamingLineScratch lineScratch;

    const std::string sourceName = source.DisplayName();

    StreamedBatch pending;
    bool pendingPrimed = false;
    size_t prevKeyCount = keys.Size();
    auto lastFlush = std::chrono::steady_clock::now();
    size_t nextLineNumber = 1;

    auto emitNewKeysInto = [&](StreamedBatch &out) {
        const size_t currentKeyCount = keys.Size();
        if (currentKeyCount > prevKeyCount)
        {
            out.newKeys.reserve(out.newKeys.size() + (currentKeyCount - prevKeyCount));
            for (size_t i = prevKeyCount; i < currentKeyCount; ++i)
            {
                out.newKeys.emplace_back(std::string(keys.KeyOf(static_cast<KeyId>(i))));
            }
            prevKeyCount = currentKeyCount;
        }
    };

    auto flushPending = [&](bool force) {
        const auto now = std::chrono::steady_clock::now();
        const bool sizeReached = pending.streamLines.size() >= kStreamLoopFlushLines;
        const bool intervalReached = (now - lastFlush) >= kStreamLoopFlushInterval;
        const bool hasContent =
            !pending.streamLines.empty() || !pending.errors.empty() || keys.Size() > prevKeyCount;
        if (!force && !sizeReached && !intervalReached)
        {
            return;
        }
        if (!force && !hasContent)
        {
            // No content yet — nothing to coalesce, just skip and let the
            // caller try again. The terminal flush at end-of-stream still
            // emits a final batch (see post-loop branch below).
            lastFlush = now;
            return;
        }
        emitNewKeysInto(pending);
        sink.OnBatch(std::move(pending));
        pending = StreamedBatch{};
        pendingPrimed = false;
        lastFlush = now;
    };

    // In-loop carry buffer for partial reads. PRD §7 *Line buffering*
    // makes the source responsible for its own partial-line buffer too
    // (rotation-discard / Stop-flush rules), but the parser still needs
    // an in-flight boundary inside the chunk it just pulled from `Read`.
    std::string carry;
    std::vector<char> readBuffer(kStreamReadBufferSize);
    std::vector<std::pair<KeyId, LogValue>> lineFields;
    std::string lineError;

    auto processLine = [&](std::string_view line) {
        std::string_view trimmed = line;
        if (!trimmed.empty() && trimmed.back() == '\r')
        {
            trimmed.remove_suffix(1);
        }
        const size_t lineNumber = nextLineNumber;
        ++nextLineNumber;

        if (trimmed.empty())
        {
            return;
        }

        const bool ok =
            DecodeStreamLine(trimmed, keys, lineScratch, &promoteScratch.keyCache, lineFields, lineError);
        if (!ok)
        {
            pending.errors.emplace_back(fmt::format("Error on line {}: {}", lineNumber, std::move(lineError)));
            return;
        }

        StreamLineReference fileRef(sourceName, std::string(trimmed), lineNumber);
        StreamLogLine streamLine(std::move(lineFields), keys, std::move(fileRef));
        // Promote inline so the freshly-written values are still hot in L1
        // (PRD 4.6.2). Mirrors the TBB pipeline's Stage B inline-promote
        // call. `WorkerScratchBase`'s state (last-valid carry, bytes-hit
        // cache) is shared across lines in this single-threaded loop.
        promoteScratch.PromoteTimestamps(streamLine, timeColumnsSpan);

        if (!pendingPrimed)
        {
            pending.firstLineNumber = lineNumber;
            pendingPrimed = true;
        }
        pending.streamLines.push_back(std::move(streamLine));
    };

    bool reachedEof = false;
    while (!reachedEof)
    {
        if (stopToken.stop_requested())
        {
            break;
        }

        const size_t read = source.Read(std::span<char>(readBuffer.data(), readBuffer.size()));
        if (read != 0)
        {
            carry.append(readBuffer.data(), read);
        }
        else
        {
            if (source.IsClosed())
            {
                reachedEof = true;
            }
            else
            {
                // Transient EOF on a live-tail source: park until more
                // bytes arrive, the source is stopped, or the timeout
                // elapses (PRD 4.9.2.ii). Flush pending lines first so
                // they don't sit in `pending` indefinitely.
                flushPending(false);
                source.WaitForBytes(kStreamLoopFlushInterval);
                continue;
            }
        }

        // Drain complete lines from the carry buffer.
        size_t scanStart = 0;
        while (scanStart < carry.size())
        {
            const size_t newlineRel = carry.find('\n', scanStart);
            if (newlineRel == std::string::npos)
            {
                break;
            }
            std::string_view line(carry.data() + scanStart, newlineRel - scanStart);
            processLine(line);
            scanStart = newlineRel + 1;

            if (stopToken.stop_requested())
            {
                break;
            }
        }
        if (scanStart > 0)
        {
            carry.erase(0, scanStart);
        }

        flushPending(false);
    }

    // Trailing partial line. The source's own `Stop()` flush rule (PRD §7
    // *Line buffering*) decides whether to emit this; here we mirror the
    // mmap pipeline's "drain whatever is in carry on EOF" behaviour so a
    // file ending without a final `\n` still surfaces its last record.
    if (!carry.empty() && !stopToken.stop_requested())
    {
        std::string_view line(carry);
        processLine(line);
        carry.clear();
    }

    // Emit at least one final batch to honour the `StreamingLogSink`
    // contract (`OnStarted` -> at least one `OnBatch` -> exactly one
    // `OnFinished`). Drain any pending content unconditionally.
    if (pendingPrimed || !pending.errors.empty() || keys.Size() > prevKeyCount)
    {
        if (!pendingPrimed)
        {
            pending.firstLineNumber = nextLineNumber;
        }
        emitNewKeysInto(pending);
        sink.OnBatch(std::move(pending));
    }
    else
    {
        StreamedBatch tail;
        tail.firstLineNumber = nextLineNumber;
        emitNewKeysInto(tail);
        sink.OnBatch(std::move(tail));
    }

    sink.OnFinished(stopToken.stop_requested());
}

void JsonParser::ParseStreaming(
    LogFile &file, StreamingLogSink &sink, ParserOptions options, internal::AdvancedParserOptions advanced
) const
{
    const size_t batchSize = advanced.batchSizeBytes != 0 ? advanced.batchSizeBytes
                                                          : internal::AdvancedParserOptions::kDefaultBatchSizeBytes;

    LogFile *filePtr = &file;
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

    auto stageB = [filePtr](
                      JsonByteRange token,
                      detail::WorkerScratch<JsonWorkerState> &worker,
                      KeyIndex &keys,
                      std::span<const detail::TimeColumnSpec> timeColumns,
                      detail::ParsedPipelineBatch &parsed
                  ) { DecodeJsonBatch(token, worker, keys, *filePtr, timeColumns, parsed); };

    detail::RunParserPipeline<JsonByteRange, JsonWorkerState>(file, sink, options, advanced, stageA, stageB);
}

} // namespace loglib
