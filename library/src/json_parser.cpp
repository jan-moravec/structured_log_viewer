#include "loglib/json_parser.hpp"

#include "loglib/internal/parser_options.hpp"
#include "loglib/internal/parser_pipeline.hpp"
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

template <class Value> LogValue ExtractStringValue(Value &value, bool sourceIsStable)
{
    // string_view path is only safe when simdjson iterates directly over the
    // mmap; the padded-scratch fallback yields views that dangle on the next line.
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

template <class Value> LogValue ExtractRawJsonValue(Value &value, bool sourceIsStable)
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
            return LogValue{rawJson};
        }
        return LogValue{std::string(rawJson)};
    }
    return LogValue{std::monostate{}};
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
    ParseCache &cache,
    bool sourceIsStable,
    detail::PerWorkerKeyCache *keyCache
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

        // `LogFile::GetLine` derives the line length as `stopOffset - startOffset - 1`,
        // where the `- 1` is the trailing '\n' byte. When the final line of the file
        // is unterminated we still owe `GetLine` that compensating byte, so push the
        // virtual `fileSize + 1` sentinel instead of `fileSize`. Otherwise the last
        // character of the line gets lopped off when `GetLine` is later called.
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
            auto values = ParseJsonLine(objectValue, keys, worker.user.cache, sourceIsStable, &worker.keyCache);

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
            worker.PromoteTimestamps(parsed.lines.back(), timeColumns);
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

void JsonParser::ParseStreaming(LogFile &file, StreamingLogSink &sink, ParserOptions options) const
{
    ParseStreaming(file, sink, std::move(options), internal::AdvancedParserOptions{});
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
