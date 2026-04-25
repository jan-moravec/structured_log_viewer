#include "loglib/json_parser.hpp"

#include "buffering_sink.hpp"

#include "loglib/log_configuration.hpp"
#include "loglib/log_file.hpp"
#include "loglib/log_line.hpp"
#include "loglib/log_processing.hpp"

#include <date/date.h>
#include <fmt/format.h>
#include <glaze/glaze.hpp>

#include <oneapi/tbb/enumerable_thread_specific.h>
#include <oneapi/tbb/global_control.h>
#include <oneapi/tbb/parallel_pipeline.h>

#include <tsl/robin_map.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace loglib
{

namespace
{

/**
 * @brief Returns a `std::string_view` over a JSON field key with the
 *        surrounding quotes stripped, taking the fast path when the bytes
 *        between the quotes contain no backslash.
 *
 * Falls back to `field.unescaped_key()` (which materialises an owning
 * `std::string` via simdjson's reusable buffer) when the bytes contain at
 * least one backslash. This matches the contract from PRD req. 4.1.15b: the
 * caller can decide whether to keep the view (zero-copy, points into the mmap
 * for the duration of the parse) or to materialise an owned copy.
 *
 * Returned in @p outView and @p outOwned: at most one of the two is populated.
 */
struct FastFieldKey
{
    bool isView = false;
    std::string_view view;
    std::string owned;
};

template <class Field>
FastFieldKey ExtractFieldKey(Field &field)
{
    FastFieldKey result;

    // field.key() returns a raw_json_string covering the JSON-source key including its
    // surrounding quotes. raw() exposes the underlying char* span; we strip the quotes and,
    // for the common (no escape) case, return a view directly into the mmap. This avoids the
    // unescape pass and the std::string materialisation that field.unescaped_key() does.
    simdjson::ondemand::raw_json_string raw;
    if (field.key().get(raw))
    {
        return result;
    }
    const char *rawData = raw.raw();
    if (rawData != nullptr)
    {
        // raw() returns a pointer into the input; the string is unterminated and runs up to
        // the closing quote. We don't have a length directly, so scan for the closing quote
        // manually. Backslash inside the run forces the slow path because the closing quote
        // could be escaped.
        const char *p = rawData;
        bool sawBackslash = false;
        while (*p != '"' || sawBackslash)
        {
            if (*p == '\\')
            {
                sawBackslash = !sawBackslash;
            }
            else
            {
                sawBackslash = false;
            }
            ++p;
        }
        const std::string_view inner(rawData, static_cast<size_t>(p - rawData));
        if (inner.find('\\') == std::string_view::npos)
        {
            result.isView = true;
            result.view = inner;
            return result;
        }
    }

    std::string_view unescaped;
    if (!field.unescaped_key().get(unescaped))
    {
        result.isView = false;
        result.owned = std::string(unescaped);
    }
    return result;
}

KeyId InternKey(const FastFieldKey &fk, KeyIndex &keys)
{
    return fk.isView ? keys.GetOrInsert(fk.view) : keys.GetOrInsert(fk.owned);
}

/**
 * @brief Transparent hash adapter for the per-worker key cache.
 *
 * `tsl::robin_map`'s heterogeneous-lookup overloads are SFINAE-gated on the
 * Hash and KeyEqual types both declaring an `is_transparent` typedef. We
 * route every overload through `std::hash<std::string_view>` so the bucket
 * a `std::string_view` query lands in is identical to the bucket the
 * matching `std::string` was stored under at insert time, regardless of
 * which overload the standard library implementation provides for
 * `std::hash<std::string>` (PRD §4.1.3, §6.2).
 */
struct TransparentStringHash
{
    using is_transparent = void;

    size_t operator()(std::string_view sv) const noexcept
    {
        return std::hash<std::string_view>{}(sv);
    }
    size_t operator()(const std::string &s) const noexcept
    {
        return std::hash<std::string_view>{}(s);
    }
    size_t operator()(const char *s) const noexcept
    {
        return std::hash<std::string_view>{}(std::string_view(s));
    }
};

/// Transparent equality adapter; `std::equal_to<>` would also work, but
/// keeping the type local makes the cache type alias self-contained.
struct TransparentStringEqual
{
    using is_transparent = void;

    bool operator()(std::string_view lhs, std::string_view rhs) const noexcept
    {
        return lhs == rhs;
    }
    bool operator()(std::string_view lhs, const std::string &rhs) const noexcept
    {
        return lhs == rhs;
    }
    bool operator()(const std::string &lhs, std::string_view rhs) const noexcept
    {
        return lhs == rhs;
    }
    bool operator()(const std::string &lhs, const std::string &rhs) const noexcept
    {
        return lhs == rhs;
    }
};

} // namespace

namespace detail
{

/**
 * @brief Per-worker key string -> KeyId cache backing PRD §4.1.
 *
 * Wrapped in a struct (rather than a bare `using` alias) so the public
 * `JsonParser` header can forward-declare it as an opaque type and pass it
 * by pointer through `ParseLine`'s parameter list without dragging in
 * `tsl/robin_map.h` or the transparent-hash adapters above.
 *
 * `tsl::robin_map` was picked per PRD §6.2: it is header-only (already
 * available via the `robin_map` FetchContent dep), supports heterogeneous
 * lookup natively when both `Hash::is_transparent` and
 * `KeyEqual::is_transparent` are present, and outperforms
 * `std::unordered_map` by roughly 2× for the small (~5–50 entries) caches
 * we expect per worker.
 *
 * On a cache hit `find(std::string_view)` performs zero heap allocations
 * (PRD req. 4.1.3); on a miss the worker calls `KeyIndex::GetOrInsert`
 * once, then writes back into the cache so subsequent lookups for the same
 * key in the same worker are O(1) and zero-alloc.
 */
struct PerWorkerKeyCache
{
    tsl::robin_map<std::string, KeyId, TransparentStringHash, TransparentStringEqual> map;
};

} // namespace detail

namespace
{

/**
 * @brief Routes a per-field key lookup through the per-worker cache when
 *        the option is enabled and the key was emitted as a `string_view`
 *        into the mmap (the fast path).
 *
 * Cache-eligible inputs (`fk.isView == true` and `useCache == true`) check
 * the per-worker `tsl::robin_map` via heterogeneous `find(string_view)` —
 * no `std::string` is materialised on the hit path. On a miss we fall
 * through to `KeyIndex::GetOrInsert(fk.view)` exactly once and then write
 * the new mapping back into the local cache. The owned-key slow path
 * (`!fk.isView`, e.g. JSON-escaped keys) skips the cache entirely: those
 * are rare enough (PRD M5: ≥ 99 % fast-path fraction) that the extra cache
 * entry would not pay for itself, and they already paid the unescape +
 * `std::string` allocation in `ExtractFieldKey` (PRD req. 4.1.4 / 4.1.6).
 */
KeyId InternKeyVia(const FastFieldKey &fk, KeyIndex &keys, detail::PerWorkerKeyCache *cache, bool useCache)
{
    if (!useCache || cache == nullptr)
    {
        return InternKey(fk, keys);
    }
    if (!fk.isView)
    {
        return keys.GetOrInsert(fk.owned);
    }
    if (auto it = cache->map.find(fk.view); it != cache->map.end())
    {
        return it->second;
    }
    const KeyId id = keys.GetOrInsert(fk.view);
    cache->map.emplace(std::string(fk.view), id);
    return id;
}

/**
 * @brief Inserts (id, value) into @p out preserving ascending-KeyId order.
 *
 * Hot path: the simdjson document iterator is forced to walk fields in
 * document order, but the canonical KeyIndex assigns ids in first-seen order
 * across the entire file, so `out` does not stay sorted on its own. Each
 * insertion does a linear scan from the back; for realistic field counts this
 * beats `std::lower_bound` due to better branch prediction and the typical
 * "newly-assigned id is the largest so far" pattern that keeps the loop body
 * to a single comparison.
 */
void InsertSorted(std::vector<std::pair<KeyId, LogValue>> &out, KeyId id, LogValue value)
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
            // Last write wins on duplicate keys, mirroring the previous LogMap insert behaviour.
            prev->second = std::move(value);
            return;
        }
        it = prev;
    }
    out.emplace(it, id, std::move(value));
}

template <class Value>
LogValue ExtractStringValue(Value &value, bool sourceIsStable)
{
    // raw_json_token includes the surrounding quotes for a JSON string token. We can only
    // strip them and emit a view directly if the underlying bytes outlive the LogLine — i.e.
    // when the parser is iterating directly over the mmap (PRD req. 4.1.6 / 4.1.15a). When
    // the caller fed simdjson a padded scratch copy (the SIMDJSON_PADDING fallback path), the
    // returned view points into that scratch buffer and would dangle as soon as the next line
    // overwrites it; in that case we must materialise an owned std::string.
    if (sourceIsStable)
    {
        std::string_view rawToken(value.raw_json_token());
        if (rawToken.size() >= 2 && rawToken.front() == '"' && rawToken.back() == '"')
        {
            std::string_view inner = rawToken.substr(1, rawToken.size() - 2);
            if (inner.find('\\') == std::string_view::npos)
            {
                return LogValue{inner};
            }
        }
    }

    std::string_view stringValue;
    if (!value.get_string().get(stringValue))
    {
        return LogValue{std::string(stringValue)};
    }
    return LogValue{std::monostate{}};
}

template <class Value>
LogValue ExtractRawJsonValue(Value &value, bool sourceIsStable)
{
    std::string_view rawJson;
    if (!value.raw_json().get(rawJson))
    {
        // Trim trailing whitespace simdjson keeps on raw_json output for arrays/objects.
        while (!rawJson.empty() && (rawJson.back() == ' ' || rawJson.back() == '\n' || rawJson.back() == '\r' ||
                                    rawJson.back() == '\t'))
        {
            rawJson.remove_suffix(1);
        }
        // Same lifetime contract as ExtractStringValue: the view is only safe to keep when it
        // points into the mmap. If the parser is reading from a padded scratch buffer we make
        // an owning copy so the LogValue is detached from the per-line scratch lifetime.
        if (sourceIsStable)
        {
            return LogValue{rawJson};
        }
        return LogValue{std::string(rawJson)};
    }
    return LogValue{std::monostate{}};
}

} // namespace

bool JsonParser::IsValid(const std::filesystem::path &file) const
{
    std::ifstream stream(file);
    if (!stream.is_open())
    {
        return false;
    }

    // Find the first non-empty line; leading blank lines are common in hand-edited logs and
    // should not cause the parser to reject the whole file.
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

ParseResult JsonParser::Parse(const std::filesystem::path &file) const
{
    // Defaulted-options entry point. Routes through the explicit-options
    // overload so there is exactly one synchronous parse body (PRD req. 4.4.31).
    return Parse(file, JsonParserOptions{});
}

ParseResult JsonParser::Parse(const std::filesystem::path &file, JsonParserOptions options) const
{
    // Legacy synchronous API. Implement on top of ParseStreaming + an in-process
    // BufferingSink so there is exactly one parser body to test and benchmark
    // (PRD req. 4.4.31). The sink owns the LogFile until it is folded into the
    // final LogData via TakeData().
    if (!std::filesystem::exists(file))
    {
        throw std::runtime_error(fmt::format("File '{}' does not exist.", file.string()));
    }
    if (std::filesystem::file_size(file) == 0)
    {
        throw std::runtime_error(fmt::format("File '{}' is empty.", file.string()));
    }

    auto logFile = std::make_unique<LogFile>(file);
    LogFile *logFilePtr = logFile.get();
    BufferingSink sink(std::move(logFile));

    ParseStreaming(*logFilePtr, sink, std::move(options));

    LogData data = sink.TakeData();
    std::vector<std::string> errors = sink.TakeErrors();
    return ParseResult{std::move(data), std::move(errors)};
}

namespace
{

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

} // namespace

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

std::vector<std::pair<KeyId, LogValue>> JsonParser::ParseLine(
    simdjson::ondemand::object &object,
    KeyIndex &keys,
    ParseCache &cache,
    bool sourceIsStable,
    detail::PerWorkerKeyCache *keyCache,
    bool useKeyCache
)
{
    std::vector<std::pair<KeyId, LogValue>> result;
    // Reserve roughly the number of fields we typically see; growth is cheap because the
    // pair is small.
    result.reserve(16);

    for (auto field : object)
    {
        FastFieldKey fk = ExtractFieldKey(field);
        if (!fk.isView && fk.owned.empty())
        {
            // unescape failed; skip the field rather than emit a synthetic empty key.
            continue;
        }

        const KeyId keyId = InternKeyVia(fk, keys, keyCache, useKeyCache);
        cache.EnsureCapacity(keyId);

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
                    InsertSorted(result, keyId, LogValue{b});
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
                            InsertSorted(result, keyId, LogValue{i});
                            continue;
                        }
                        break;
                    }
                    case simdjson::ondemand::number_type::unsigned_integer: {
                        uint64_t u;
                        if (!value.get(u))
                        {
                            InsertSorted(result, keyId, LogValue{u});
                            continue;
                        }
                        break;
                    }
                    case simdjson::ondemand::number_type::floating_point_number: {
                        double d;
                        if (!value.get(d))
                        {
                            InsertSorted(result, keyId, LogValue{d});
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
                LogValue stringValue = ExtractStringValue(value, sourceIsStable);
                if (!std::holds_alternative<std::monostate>(stringValue))
                {
                    InsertSorted(result, keyId, std::move(stringValue));
                    continue;
                }
                break;
            }
            case simdjson::ondemand::json_type::array:
            case simdjson::ondemand::json_type::object: {
                LogValue rawValue = ExtractRawJsonValue(value, sourceIsStable);
                if (!std::holds_alternative<std::monostate>(rawValue))
                {
                    InsertSorted(result, keyId, std::move(rawValue));
                    continue;
                }
                break;
            }
            case simdjson::ondemand::json_type::null:
                if (value.is_null())
                {
                    InsertSorted(result, keyId, LogValue{std::monostate{}});
                    continue;
                }
                break;
            default:
                break;
            }
        }

        // Cache miss / cache mismatch: fall through to typed parsing and prime the cache.
        auto typeResult = value.type();
        if (typeResult.error())
        {
            InsertSorted(result, keyId, LogValue{std::monostate{}});
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
                InsertSorted(result, keyId, LogValue{b});
            }
            else
            {
                InsertSorted(result, keyId, LogValue{std::monostate{}});
            }
            break;
        }
        case simdjson::ondemand::json_type::number: {
            auto numberType = value.get_number_type();
            if (numberType.error())
            {
                InsertSorted(result, keyId, LogValue{std::monostate{}});
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
                    InsertSorted(result, keyId, LogValue{i});
                }
                else
                {
                    InsertSorted(result, keyId, LogValue{std::monostate{}});
                }
                break;
            }
            case simdjson::ondemand::number_type::unsigned_integer: {
                uint64_t u;
                if (!value.get(u))
                {
                    InsertSorted(result, keyId, LogValue{u});
                }
                else
                {
                    InsertSorted(result, keyId, LogValue{std::monostate{}});
                }
                break;
            }
            case simdjson::ondemand::number_type::floating_point_number: {
                double d;
                if (!value.get(d))
                {
                    InsertSorted(result, keyId, LogValue{d});
                }
                else
                {
                    InsertSorted(result, keyId, LogValue{std::monostate{}});
                }
                break;
            }
            default:
                InsertSorted(result, keyId, LogValue{std::monostate{}});
                break;
            }
            break;
        }
        case simdjson::ondemand::json_type::string: {
            LogValue stringValue = ExtractStringValue(value, sourceIsStable);
            InsertSorted(result, keyId, std::move(stringValue));
            break;
        }
        case simdjson::ondemand::json_type::array:
        case simdjson::ondemand::json_type::object: {
            LogValue rawValue = ExtractRawJsonValue(value, sourceIsStable);
            InsertSorted(result, keyId, std::move(rawValue));
            break;
        }
        case simdjson::ondemand::json_type::null:
            InsertSorted(result, keyId, LogValue{std::monostate{}});
            break;
        default:
            InsertSorted(result, keyId, LogValue{std::monostate{}});
            break;
        }
    }

    return result;
}

/**
 * @brief Streaming-pipeline implementation detail for `JsonParser`.
 *
 * Bundled as a friend struct (declared in `json_parser.hpp`) so that the
 * Stage B parsing body can reach the private nested `ParseCache` and the
 * private static `ParseLine` helper without exposing them on the public
 * `JsonParser` surface. Everything inside lives only in this translation
 * unit; nothing here is part of the binary interface.
 */
struct JsonParser::StreamingDetail
{
    /**
     * @brief Stage A token: a contiguous byte range of the mmap that
     *        contains an integer number of complete log lines.
     *
     * Stage A advances a cursor in `serial_in_order` mode and emits one of
     * these per pipeline tick. `bytesBegin` and `bytesEnd` are pointers into
     * the mmap (lifetime tied to the LogFile, which outlives the pipeline).
     * `firstLineNumber` is the 1-based absolute line number of the first
     * line in the batch, used by Stage B to stamp each LogLine's
     * LogFileReference.
     */
    struct PipelineBatch
    {
        uint64_t batchIndex = 0;
        const char *bytesBegin = nullptr;
        const char *bytesEnd = nullptr;
        const char *fileEnd = nullptr;
        size_t firstLineNumber = 1;
    };

    /**
     * @brief Stage B token: per-line parse output for a single PipelineBatch.
     *
     * The pipeline shuttles these from Stage B (parallel) to Stage C
     * (serial_in_order); ordering is reasserted by
     * `tbb::filter_mode::serial_in_order` before the appender sees them.
     */
    struct ParsedPipelineBatch
    {
        uint64_t batchIndex = 0;
        std::vector<LogLine> lines;
        std::vector<uint64_t> localLineOffsets; // start-of-line byte offsets within the file
        std::vector<std::string> errors;
        size_t firstLineNumber = 1;
        size_t totalLineCount = 0; // lines + errors + skipped-empty
    };

    /**
     * @brief Per-worker scratch held in oneTBB's enumerable_thread_specific.
     *
     * Each Stage B worker holds one of these to amortise the cost of: (a)
     * the simdjson on-demand parser instance, (b) the padded scratch buffer
     * used when the trailing tail of the mmap doesn't have SIMDJSON_PADDING
     * bytes of slack, (c) the per-KeyId type-cache vector, and (d) the
     * per-worker key string -> `KeyId` cache (PRD §4.1) that fronts the
     * canonical `KeyIndex` and absorbs the per-call `std::string`
     * materialisation cost via heterogeneous `find(string_view)` on a
     * `tsl::robin_map`. Cache entries survive the move-construction that
     * `enumerable_thread_specific` performs when growing its per-thread
     * slot table (covered by the `[key_cache][move]` test).
     *
     * The per-worker timestamp helper (`LastValidTimestampParse`) is
     * intentionally *not* held here yet: Stage B in-pipeline timestamp
     * promotion (PRD req. 4.2.21) is wired up in task 3.0 (PRD §4.2a) once
     * `BackfillTimestampColumn` is extracted from `log_processing.cpp`.
     */
    struct WorkerState
    {
        simdjson::ondemand::parser parser;
        std::string linePadded;
        JsonParser::ParseCache cache;
        detail::PerWorkerKeyCache keyCache;
    };

    /**
     * @brief Walks @p batch's bytes and parses each non-empty line into a
     *        `LogLine`.
     *
     * Encapsulates the per-batch work of Stage B so we can unit-test the
     * parsing body separately from the pipeline harness if needed. The
     * per-worker timestamp-promotion fast path lands once
     * `BackfillTimestampColumn` is extracted in task 3.8/4.6.
     */
    static ParsedPipelineBatch ParseBatchBody(
        const PipelineBatch &batch,
        WorkerState &worker,
        KeyIndex &keys,
        LogFile &logFile,
        const LogConfiguration *configuration,
        bool useThreadLocalKeyCache,
        bool useParseCache
    );
};

JsonParser::StreamingDetail::ParsedPipelineBatch JsonParser::StreamingDetail::ParseBatchBody(
    const PipelineBatch &batch,
    WorkerState &worker,
    KeyIndex &keys,
    LogFile &logFile,
    const LogConfiguration *configuration,
    bool useThreadLocalKeyCache,
    bool useParseCache
)
{
    (void)configuration; // Stage B in-pipeline timestamp promotion lands in task 4.6/3.8.

    ParsedPipelineBatch parsed;
    parsed.batchIndex = batch.batchIndex;
    parsed.firstLineNumber = batch.firstLineNumber;

    const char *cursor = batch.bytesBegin;
    const char *end = batch.bytesEnd;
    const char *fileEnd = batch.fileEnd;
    const char *fileBegin = logFile.Data();

    // Estimate ~32 bytes per line is too aggressive for big logs; ~64 is closer for prod logs.
    const size_t batchBytes = static_cast<size_t>(end - cursor);
    const size_t estimatedLines = batchBytes / 64 + 1;
    parsed.lines.reserve(estimatedLines);
    parsed.localLineOffsets.reserve(estimatedLines);

    size_t absoluteLineNumber = batch.firstLineNumber;

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

        // Always record the offset, including for empty lines, so the LogFile's mLineOffsets
        // table matches the legacy parser's behaviour (one entry per consumed line). The
        // stored value is the offset of the *next* line (i.e. one past the trailing newline),
        // matching the sentinel convention `LogFile::CreateReference` uses.
        parsed.localLineOffsets.push_back(static_cast<uint64_t>(cursor - fileBegin));

        if (line.empty())
        {
            absoluteLineNumber++;
            continue;
        }

        try
        {
            // The parser is reading directly from the mmap when there is at least
            // SIMDJSON_PADDING bytes of slack between line.end() and the file tail; otherwise
            // we fall back to a per-worker padded copy. Only the mmap path can safely emit
            // string_view alternatives in LogValue (PRD req. 4.1.6 / 4.1.15a).
            const size_t remaining = static_cast<size_t>(fileEnd - lineEnd);
            const bool sourceIsStable = remaining >= simdjson::SIMDJSON_PADDING;
            auto result = sourceIsStable
                              ? worker.parser.iterate(line.data(), line.size(), line.size() + remaining)
                              : worker.parser.iterate(simdjson::pad(worker.linePadded.assign(line)));
            if (result.error())
            {
                parsed.errors.push_back(fmt::format(
                    "Error on line {}: {}",
                    absoluteLineNumber,
                    simdjson::error_message(result.error())
                ));
                absoluteLineNumber++;
                continue;
            }

            auto object = result.get_object();
            if (object.error())
            {
                parsed.errors.push_back(fmt::format("Error on line {}: Not a JSON object.", absoluteLineNumber));
                absoluteLineNumber++;
                continue;
            }

            auto objectValue = object.value();
            (void)useParseCache; // ParseCache is always-on for now; opt-out lands in 6.x
            auto values = JsonParser::ParseLine(
                objectValue, keys, worker.cache, sourceIsStable, &worker.keyCache, useThreadLocalKeyCache
            );

            // Placeholder line number; Stage C overwrites once it reasserts ordering.
            LogFileReference fileRef(logFile, 0);
            LogLine logLine(std::move(values), keys, std::move(fileRef));
            logLine.FileReference().SetLineNumber(absoluteLineNumber);

            parsed.lines.push_back(std::move(logLine));
        }
        catch (const std::exception &e)
        {
            parsed.errors.push_back(fmt::format("Error on line {}: {}", absoluteLineNumber, e.what()));
        }

        absoluteLineNumber++;
    }

    parsed.totalLineCount = absoluteLineNumber - batch.firstLineNumber;
    return parsed;
}

void JsonParser::ParseStreaming(LogFile &file, StreamingLogSink &sink, JsonParserOptions options) const
{
    sink.OnStarted();

    // Cooperative cancellation entry-check: if the caller already requested stop before we ever
    // started, fire OnFinished(true) and bail without doing any work (PRD req. 4.2.22a).
    if (options.stopToken.stop_requested())
    {
        sink.OnFinished(true);
        return;
    }

    const char *fileBegin = file.Data();
    const size_t fileSize = file.Size();
    if (fileSize == 0 || fileBegin == nullptr)
    {
        sink.OnFinished(false);
        return;
    }
    const char *fileEnd = fileBegin + fileSize;

    // Resolve thread/parallelism settings. Worker count is bounded by both the user request
    // and our default ceiling; ntokens defaults to 2× workers so Stage B can stay busy while
    // Stage A and Stage C do their (much lighter) bookkeeping.
    unsigned int effectiveThreads = options.threads != 0
                                        ? options.threads
                                        : std::min(std::thread::hardware_concurrency(),
                                                   static_cast<unsigned int>(JsonParserOptions::kDefaultMaxThreads));
    if (effectiveThreads == 0)
    {
        effectiveThreads = 1;
    }
    const size_t ntokens = options.ntokens != 0 ? options.ntokens : static_cast<size_t>(2 * effectiveThreads);
    const size_t batchSize = options.batchSizeBytes != 0 ? options.batchSizeBytes
                                                         : JsonParserOptions::kDefaultBatchSizeBytes;

    // Pre-grow the file's line offset table to roughly the expected number of lines so the
    // Stage C appender (or the calling sink, in the streaming case) does not pay a quadratic
    // realloc tax on big files. Estimate is intentionally generous — overshooting is cheap.
    file.ReserveLineOffsets(fileSize / 100);

    // Borrow the canonical KeyIndex from the sink (PRD req. 4.1.2/2a). Every Stage B worker
    // routes GetOrInsert/KeyOf through this single instance, and Stage C reads its
    // high-water-mark slice directly to populate StreamedBatch::newKeys. The sink owns the
    // KeyIndex's lifetime; we never make a parallel copy that would have to be reconciled.
    KeyIndex &keys = sink.Keys();
    using PipelineBatch = StreamingDetail::PipelineBatch;
    using ParsedPipelineBatch = StreamingDetail::ParsedPipelineBatch;
    using WorkerState = StreamingDetail::WorkerState;
    oneapi::tbb::enumerable_thread_specific<WorkerState> workers;

    const LogConfiguration *configurationPtr = options.configuration.get();
    const bool useThreadLocalKeyCache = options.useThreadLocalKeyCache;
    const bool useParseCache = options.useParseCache;

    // Stage A: serial, in-order. Walks the mmap at batchSize granularity, advancing to the
    // next newline so each batch contains an integer number of lines. Tracks the absolute line
    // number of the first line in the next batch so Stage C can stamp LogFileReferences
    // without an extra accumulator.
    const char *cursor = fileBegin;
    uint64_t batchIndex = 0;
    size_t nextLineNumber = 1;
    std::stop_token stopToken = options.stopToken;

    auto stageA = [&](oneapi::tbb::flow_control &fc) -> PipelineBatch {
        if (cursor >= fileEnd || stopToken.stop_requested())
        {
            fc.stop();
            return PipelineBatch{};
        }

        const char *batchBegin = cursor;
        const char *target = std::min(cursor + batchSize, fileEnd);
        if (target < fileEnd)
        {
            // Advance to next newline so each batch is an integer number of lines. If no
            // newline exists between target and fileEnd, the batch absorbs the rest of the
            // file (handles the "single line larger than batch" case).
            const char *newline = static_cast<const char *>(memchr(target, '\n', static_cast<size_t>(fileEnd - target)));
            cursor = (newline != nullptr) ? newline + 1 : fileEnd;
        }
        else
        {
            cursor = fileEnd;
        }

        PipelineBatch out;
        out.batchIndex = batchIndex++;
        out.bytesBegin = batchBegin;
        out.bytesEnd = cursor;
        out.fileEnd = fileEnd;
        out.firstLineNumber = nextLineNumber;

        // Pre-count this batch's newlines so Stage A keeps an accurate line-number running
        // total. memchr is cheap enough that doing it twice (here and in Stage B) is not
        // a measurable cost in practice; if it becomes one we can hoist it from Stage B.
        size_t lineCount = 0;
        for (const char *p = batchBegin; p < cursor; ++p)
        {
            const char *nl = static_cast<const char *>(memchr(p, '\n', static_cast<size_t>(cursor - p)));
            if (nl == nullptr)
            {
                ++lineCount; // trailing line without '\n'
                break;
            }
            ++lineCount;
            p = nl;
        }
        nextLineNumber += lineCount;
        return out;
    };

    // Stage B: parallel. Per-worker simdjson + KeyIndex interning + ParseCache lookups.
    auto stageB = [&](PipelineBatch batch) -> ParsedPipelineBatch {
        WorkerState &worker = workers.local();
        return StreamingDetail::ParseBatchBody(
            batch, worker, keys, file, configurationPtr, useThreadLocalKeyCache, useParseCache
        );
    };

    // Stage C: serial, in-order. Aggregates per-pipeline-batch state into a sink-visible
    // StreamedBatch, flushing whenever either of the two coalescing thresholds (PRD req.
    // 4.3.26) trips:
    //   - kStreamFlushLines   : line count accumulated since the last flush
    //   - kStreamFlushInterval: wall-clock elapsed since the last flush
    // Both are intentionally small enough that a fast UI gets sub-100 ms time-to-first-row
    // updates while still amortising sink-side bookkeeping over 1 000-line groups in steady
    // state. The newKeys slice is computed lazily (only at flush time) so a key that appears
    // and disappears within a single coalesced window is still reported exactly once.
    //
    // We start the high-water mark from the sink's current key count so a sink that
    // pre-populated its KeyIndex (e.g. via a previous parse on the same model) does not see
    // those keys repeated as "new" in batch 0.
    constexpr size_t kStreamFlushLines = 1000;
    constexpr auto kStreamFlushInterval = std::chrono::milliseconds(50);

    StreamedBatch pending;
    bool pendingPrimed = false;
    size_t prevKeyCount = keys.Size();
    auto lastFlush = std::chrono::steady_clock::now();

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
        if (!force && pending.lines.size() < kStreamFlushLines &&
            (std::chrono::steady_clock::now() - lastFlush) < kStreamFlushInterval)
        {
            return;
        }
        emitNewKeysInto(pending);
        sink.OnBatch(std::move(pending));
        pending = StreamedBatch{};
        pendingPrimed = false;
        lastFlush = std::chrono::steady_clock::now();
    };

    auto stageC = [&](ParsedPipelineBatch parsed) {
        if (!pendingPrimed)
        {
            pending.firstLineNumber = parsed.firstLineNumber;
            pendingPrimed = true;
        }
        if (!parsed.lines.empty())
        {
            pending.lines.reserve(pending.lines.size() + parsed.lines.size());
            std::move(parsed.lines.begin(), parsed.lines.end(), std::back_inserter(pending.lines));
        }
        if (!parsed.localLineOffsets.empty())
        {
            pending.localLineOffsets.reserve(pending.localLineOffsets.size() + parsed.localLineOffsets.size());
            std::move(
                parsed.localLineOffsets.begin(),
                parsed.localLineOffsets.end(),
                std::back_inserter(pending.localLineOffsets)
            );
        }
        if (!parsed.errors.empty())
        {
            pending.errors.reserve(pending.errors.size() + parsed.errors.size());
            std::move(parsed.errors.begin(), parsed.errors.end(), std::back_inserter(pending.errors));
        }
        flushPending(false);
    };

    // RAII-scope an explicit oneTBB parallelism cap so we do not balloon thread usage on
    // 64-core hosts; users can opt into more via JsonParserOptions::threads.
    //
    // Cancellation latency contract (PRD req. 4.2.22a/b): Stage A polls the stop_token on
    // every batch boundary, so once the user requests cancellation the pipeline will drain
    // at most `ntokens` Stage B tokens (each carrying ~`batchSizeBytes` worth of work) before
    // shutdown. Worst-case wasted work is therefore bounded by `ntokens * batchSizeBytes`
    // bytes (default ≈ 16 * 1 MiB = 16 MiB on an 8-core host), independent of the input file
    // size. The Stage C coalescing flush above does not extend this bound — it operates only
    // on output already produced by Stage B.
    oneapi::tbb::global_control gc(
        oneapi::tbb::global_control::max_allowed_parallelism, static_cast<size_t>(effectiveThreads)
    );

    oneapi::tbb::parallel_pipeline(
        ntokens,
        oneapi::tbb::make_filter<void, PipelineBatch>(oneapi::tbb::filter_mode::serial_in_order, stageA) &
            oneapi::tbb::make_filter<PipelineBatch, ParsedPipelineBatch>(oneapi::tbb::filter_mode::parallel, stageB) &
            oneapi::tbb::make_filter<ParsedPipelineBatch, void>(oneapi::tbb::filter_mode::serial_in_order, stageC)
    );

    // Final flush before OnFinished. The PRD (req. 4.3.26a) requires that the sink see at
    // least one final OnBatch even if it carries no rows — so the sink can finalise its state
    // (e.g. emit dataChanged, update progress labels) under a single contract. Force-flush
    // any in-flight pending batch first, then if nothing was flushed this turn (e.g. because
    // the file was empty or the parse was cancelled before Stage A produced any tokens) emit
    // a synthetic terminal batch carrying the tail newKeys snapshot.
    if (pendingPrimed || keys.Size() > prevKeyCount)
    {
        if (!pendingPrimed)
        {
            pending.firstLineNumber = nextLineNumber;
            pendingPrimed = true;
        }
        flushPending(true);
    }
    else
    {
        StreamedBatch tail;
        tail.firstLineNumber = nextLineNumber;
        sink.OnBatch(std::move(tail));
    }

    sink.OnFinished(stopToken.stop_requested());
}

} // namespace loglib
