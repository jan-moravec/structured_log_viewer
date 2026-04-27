#include "loglib/json_parser.hpp"

#include "buffering_sink.hpp"
#include "parser_pipeline.hpp"

#include "loglib/log_configuration.hpp"
#include "loglib/log_file.hpp"
#include "loglib/log_line.hpp"
#include "loglib/log_processing.hpp"

#include <date/date.h>
#include <fmt/format.h>
#include <glaze/glaze.hpp>

#include <simdjson.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace loglib
{

namespace
{

/**
 * @brief Returns a `std::string_view` over a JSON field key with the surrounding
 *        quotes stripped, taking the fast path when the bytes between the quotes
 *        contain no backslash. Falls back to `field.unescaped_key()` (which
 *        materialises an owning copy via simdjson's reusable buffer) when the
 *        bytes contain at least one backslash.
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

KeyId InternFieldKey(const FastFieldKey &fk, KeyIndex &keys, detail::PerWorkerKeyCache *cache, bool useCache)
{
    if (!fk.isView)
    {
        return keys.GetOrInsert(fk.owned);
    }
    return detail::InternKeyVia(fk.view, keys, cache, useCache);
}

/// Field-count threshold above which `InsertSorted` switches from a linear back-scan to
/// `std::lower_bound`. Below the threshold the back-scan wins because the
/// "newly-assigned id is the largest so far" pattern collapses the body to a single
/// comparison; above it `[wide]`-style 30-column rows benefit from the binary search.
constexpr size_t kInsertSortedLowerBoundThreshold = 8;

void InsertSorted(std::vector<std::pair<KeyId, LogValue>> &out, KeyId id, LogValue value)
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
                prev->second = std::move(value);
                return;
            }
            it = prev;
        }
        out.emplace(it, id, std::move(value));
        return;
    }

    auto it = std::lower_bound(out.begin(), out.end(), id, [](const std::pair<KeyId, LogValue> &lhs, KeyId rhs) {
        return lhs.first < rhs;
    });
    if (it != out.end() && it->first == id)
    {
        it->second = std::move(value);
        return;
    }
    out.emplace(it, id, std::move(value));
}

template <class Value>
LogValue ExtractStringValue(Value &value, bool sourceIsStable)
{
    // The string-view fast path is only safe when the bytes outlive the LogLine — i.e.
    // when simdjson is iterating directly over the mmap. The padded-scratch fallback
    // path returns views that dangle as soon as the next line overwrites them, so we
    // must materialise an owned std::string in that case.
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
        while (!rawJson.empty() && (rawJson.back() == ' ' || rawJson.back() == '\n' || rawJson.back() == '\r' ||
                                    rawJson.back() == '\t'))
        {
            rawJson.remove_suffix(1);
        }
        if (sourceIsStable)
        {
            return LogValue{rawJson};
        }
        return LogValue{std::string(rawJson)};
    }
    return LogValue{std::monostate{}};
}

void EnsureCacheCapacity(JsonParser::ParseCache &cache, KeyId id)
{
    const size_t needed = static_cast<size_t>(id) + 1;
    if (cache.keyTypes.size() < needed)
    {
        cache.keyTypes.resize(needed, simdjson::ondemand::json_type::null);
        cache.hasKeyType.resize(needed, false);
    }
    if (cache.numberTypes.size() < needed)
    {
        cache.numberTypes.resize(needed, simdjson::ondemand::number_type::signed_integer);
        cache.hasNumberType.resize(needed, false);
    }
}

std::vector<std::pair<KeyId, LogValue>> ParseJsonLine(
    simdjson::ondemand::object &object,
    KeyIndex &keys,
    JsonParser::ParseCache &cache,
    bool sourceIsStable,
    detail::PerWorkerKeyCache *keyCache,
    bool useKeyCache
)
{
    std::vector<std::pair<KeyId, LogValue>> result;
    result.reserve(16);

    for (auto field : object)
    {
        FastFieldKey fk = ExtractFieldKey(field);
        if (!fk.isView && fk.owned.empty())
        {
            continue;
        }

        const KeyId keyId = InternFieldKey(fk, keys, keyCache, useKeyCache);
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

/// JSON-specific per-worker scratch carried by the shared harness's `WorkerScratch`.
struct JsonWorkerState
{
    simdjson::ondemand::parser parser;
    std::string linePadded;
    size_t maxLineSize = 0;
    JsonParser::ParseCache cache;
};

/// Stage A token: a contiguous byte range of the mmap that contains an integer number
/// of complete log lines.
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
    bool useThreadLocalKeyCache,
    bool useParseCache,
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

        parsed.localLineOffsets.push_back(static_cast<uint64_t>(cursor - fileBegin));

        if (line.empty())
        {
            relativeLineNumber++;
            continue;
        }

        try
        {
            // simdjson reads directly from the mmap when there is at least SIMDJSON_PADDING
            // bytes of slack between line.end() and the file tail; otherwise we fall back to
            // a per-worker padded copy. Only the mmap path can safely emit string_view
            // alternatives in LogValue.
            const size_t remaining = static_cast<size_t>(fileEnd - lineEnd);
            const bool sourceIsStable = remaining >= simdjson::SIMDJSON_PADDING;
            auto result = sourceIsStable
                              ? worker.user.parser.iterate(line.data(), line.size(), line.size() + remaining)
                              : [&]() {
                                    const size_t needed = line.size() + simdjson::SIMDJSON_PADDING;
                                    if (line.size() > worker.user.maxLineSize ||
                                        worker.user.linePadded.size() < needed)
                                    {
                                        worker.user.maxLineSize = std::max(worker.user.maxLineSize, line.size());
                                        worker.user.linePadded.resize(
                                            worker.user.maxLineSize + simdjson::SIMDJSON_PADDING + 64
                                        );
                                    }
                                    std::memcpy(worker.user.linePadded.data(), line.data(), line.size());
                                    std::memset(
                                        worker.user.linePadded.data() + line.size(),
                                        0,
                                        simdjson::SIMDJSON_PADDING
                                    );
                                    return worker.user.parser.iterate(
                                        worker.user.linePadded.data(),
                                        line.size(),
                                        worker.user.linePadded.size()
                                    );
                                }();
            if (result.error())
            {
                parsed.errors.push_back(fmt::format(
                    "Error on line {}: {}",
                    relativeLineNumber,
                    simdjson::error_message(result.error())
                ));
                relativeLineNumber++;
                continue;
            }

            auto object = result.get_object();
            if (object.error())
            {
                parsed.errors.push_back(fmt::format("Error on line {}: Not a JSON object.", relativeLineNumber));
                relativeLineNumber++;
                continue;
            }

            auto objectValue = object.value();
            (void)useParseCache;
            auto values = ParseJsonLine(
                objectValue, keys, worker.user.cache, sourceIsStable, &worker.keyCache, useThreadLocalKeyCache
            );

            LogFileReference fileRef(logFile, 0);
            LogLine logLine(std::move(values), keys, std::move(fileRef));
            logLine.FileReference().SetLineNumber(relativeLineNumber);

            parsed.lines.push_back(std::move(logLine));

            // Per PRD §4.3.2, time-column promotion is the harness's hook, but applied
            // inline (per line, hot in L1) rather than as a per-batch post-decode walk:
            // the [stream_to_table] benchmark is sensitive to the second-walk cache loss.
            worker.PromoteTimestamps(parsed.lines.back(), timeColumns);
        }
        catch (const std::exception &e)
        {
            parsed.errors.push_back(fmt::format("Error on line {}: {}", relativeLineNumber, e.what()));
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

ParseResult JsonParser::Parse(const std::filesystem::path &file) const
{
    return Parse(file, JsonParserOptions{});
}

ParseResult JsonParser::Parse(const std::filesystem::path &file, JsonParserOptions options) const
{
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
    return ParseJsonLine(object, keys, cache, sourceIsStable, keyCache, useKeyCache);
}

void JsonParser::ParseStreaming(LogFile &file, StreamingLogSink &sink, JsonParserOptions options) const
{
    detail::PipelineHarnessOptions harnessOpts;
    harnessOpts.threads = options.threads;
    harnessOpts.batchSizeBytes = options.batchSizeBytes;
    harnessOpts.ntokens = options.ntokens;
    harnessOpts.configuration = options.configuration;
    harnessOpts.stopToken = options.stopToken;
    harnessOpts.timings = options.timings;

    const size_t batchSize = options.batchSizeBytes != 0
                                 ? options.batchSizeBytes
                                 : detail::PipelineHarnessOptions::kDefaultBatchSizeBytes;
    const bool useThreadLocalKeyCache = options.useThreadLocalKeyCache;
    const bool useParseCache = options.useParseCache;

    LogFile *filePtr = &file;
    const char *fileBegin = file.Data();
    const size_t fileSize = file.Size();
    const char *fileEnd = (fileBegin != nullptr) ? fileBegin + fileSize : nullptr;
    const char *cursor = fileBegin;
    uint64_t batchIndex = 0;

    auto stageA = [cursor, fileEnd, batchSize, batchIndex, fileBegin](JsonByteRange &out) mutable -> bool {
        (void)fileBegin;
        if (cursor >= fileEnd)
        {
            return false;
        }
        const char *batchBegin = cursor;
        const char *target = std::min(cursor + batchSize, fileEnd);
        if (target < fileEnd)
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

    auto stageB = [filePtr, useThreadLocalKeyCache, useParseCache](
                      JsonByteRange token,
                      detail::WorkerScratch<JsonWorkerState> &worker,
                      KeyIndex &keys,
                      std::span<const detail::TimeColumnSpec> timeColumns,
                      detail::ParsedPipelineBatch &parsed
                  ) {
        DecodeJsonBatch(
            token, worker, keys, *filePtr, timeColumns, useThreadLocalKeyCache, useParseCache, parsed
        );
    };

    detail::RunParserPipeline<JsonByteRange, JsonWorkerState>(
        file, sink, harnessOpts, stageA, stageB
    );
}

} // namespace loglib
