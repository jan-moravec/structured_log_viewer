#include <test_common/log_format.hpp>

#include <glaze/glaze.hpp>

#include <algorithm>
#include <cassert>
#include <string>
#include <string_view>

namespace test_common
{

namespace
{

// True when @p value has no byte that would end a logfmt token or need
// escaping. Mirrors `loglib::BareValueIsSafe`; the `[logfmt_parser][round_trip]`
// tests guard against drift between the two copies.
bool BareValueIsSafe(std::string_view value)
{
    if (value.empty())
    {
        return false;
    }
    return std::ranges::all_of(value, [](char c) {
        const auto uc = static_cast<unsigned char>(c);
        return uc > ' ' && uc != '"' && uc != '=' && uc != '\\';
    });
}

// Append @p value as a double-quoted logfmt string with C-style escapes.
// Mirrors `loglib::AppendQuotedString`.
void AppendQuotedString(std::string &out, std::string_view value)
{
    out.push_back('"');
    for (const char c : value)
    {
        switch (c)
        {
        case '"':
            out.append("\\\"");
            break;
        case '\\':
            out.append("\\\\");
            break;
        case '\n':
            out.append("\\n");
            break;
        case '\r':
            out.append("\\r");
            break;
        case '\t':
            out.append("\\t");
            break;
        default:
            out.push_back(c);
            break;
        }
    }
    out.push_back('"');
}

std::string CompactJson(const LogRecord &value)
{
    return glz::write_json(value).value_or("");
}

void AppendLogfmtValue(std::string &out, const LogRecord &value)
{
    if (value.is_null())
    {
        // Null serializes as `key=` (empty value).
        return;
    }
    if (value.is_string())
    {
        const std::string &text = value.get_string();
        if (BareValueIsSafe(text))
        {
            out.append(text);
        }
        else
        {
            AppendQuotedString(out, text);
        }
        return;
    }
    if (value.is_boolean())
    {
        out.append(value.get_boolean() ? "true" : "false");
        return;
    }
    if (value.is_number())
    {
        // Numbers share the bare JSON token form.
        out.append(CompactJson(value));
        return;
    }
    // Array/object: embed compact JSON as a single quoted string so the
    // per-line field count still matches the JSON serialization.
    AppendQuotedString(out, CompactJson(value));
}

} // namespace

LogFormat JsonLines()
{
    return LogFormat{
        .suggestedExtension = ".jsonl",
        .writeHeader = [](const RecordSchema &) { return std::string{}; },
        .writeLine = [](const LogRecord &record) { return CompactJson(record); },
    };
}

LogFormat Logfmt()
{
    return LogFormat{
        .suggestedExtension = ".logfmt",
        .writeHeader = [](const RecordSchema &) { return std::string{}; },
        .writeLine =
            [](const LogRecord &record) {
                if (!record.is_object())
                {
                    // No key=value structure; fall back to compact JSON.
                    return CompactJson(record);
                }
                std::string out;
                bool first = true;
                for (const auto &[key, value] : record.get_object())
                {
                    if (!first)
                    {
                        out.push_back(' ');
                    }
                    first = false;
                    // logfmt has no key-quoting syntax, so keys must be
                    // bare-safe. Catches hand-crafted records in debug builds.
                    assert(BareValueIsSafe(key) && "logfmt keys must be bare-safe");
                    out.append(key);
                    out.push_back('=');
                    AppendLogfmtValue(out, value);
                }
                return out;
            },
    };
}

namespace
{

// Duplicate of `loglib::BareCellIsSafe` so `test_common` stays
// loglib-free; drift is caught by `[csv_parser][round_trip]` tests.
bool CsvCellIsBareSafe(std::string_view value) noexcept
{
    return std::ranges::all_of(value, [](char c) { return c != ',' && c != '"' && c != '\r' && c != '\n'; });
}

void AppendCsvQuoted(std::string &out, std::string_view value)
{
    out.push_back('"');
    for (const char c : value)
    {
        if (c == '"')
        {
            out.append("\"\"");
        }
        else
        {
            out.push_back(c);
        }
    }
    out.push_back('"');
}

void AppendCsvCell(std::string &out, std::string_view value)
{
    if (CsvCellIsBareSafe(value))
    {
        out.append(value.data(), value.size());
    }
    else
    {
        AppendCsvQuoted(out, value);
    }
}

void AppendCsvValue(std::string &out, const LogRecord &value)
{
    if (value.is_null())
    {
        // Empty cell -> `CsvParser` omits it from the row.
        return;
    }
    if (value.is_string())
    {
        AppendCsvCell(out, value.get_string());
        return;
    }
    if (value.is_boolean())
    {
        out.append(value.get_boolean() ? "true" : "false");
        return;
    }
    if (value.is_number())
    {
        out.append(CompactJson(value));
        return;
    }
    // Array / object: embed compact JSON in one quoted cell so wide-row
    // column counts match the JSON serialisation (lossy escape hatch).
    AppendCsvQuoted(out, CompactJson(value));
}

// Returns a pointer to @p key inside @p record, or nullptr if absent
// or @p record is not an object.
const LogRecord *FindObjectField(const LogRecord &record, const std::string &key)
{
    if (!record.is_object())
    {
        return nullptr;
    }
    for (const auto &[k, v] : record.get_object())
    {
        if (k == key)
        {
            return &v;
        }
    }
    return nullptr;
}

} // namespace

LogFormat Csv(RecordSchema schema)
{
    return LogFormat{
        .suggestedExtension = ".csv",
        .writeHeader =
            [capturedSchema = schema](const RecordSchema &paramSchema) {
                // Prefer the caller-supplied schema; fall back to the
                // captured one for factories pre-wired with a schema.
                const RecordSchema &effective = paramSchema.empty() ? capturedSchema : paramSchema;
                if (effective.empty())
                {
                    return std::string{};
                }
                std::string out;
                bool first = true;
                for (const auto &name : effective)
                {
                    if (!first)
                    {
                        out.push_back(',');
                    }
                    first = false;
                    AppendCsvCell(out, name);
                }
                return out;
            },
        .writeLine =
            [capturedSchema = std::move(schema)](const LogRecord &record) {
                if (capturedSchema.empty())
                {
                    // Headerless: walk the record's lex order. Rejected
                    // by `CsvParser::IsValid`; real fixtures pass a schema.
                    if (!record.is_object())
                    {
                        return CompactJson(record);
                    }
                    std::string out;
                    bool first = true;
                    for (const auto &[_key, value] : record.get_object())
                    {
                        if (!first)
                        {
                            out.push_back(',');
                        }
                        first = false;
                        AppendCsvValue(out, value);
                    }
                    return out;
                }
                std::string out;
                bool first = true;
                for (const auto &name : capturedSchema)
                {
                    if (!first)
                    {
                        out.push_back(',');
                    }
                    first = false;
                    if (const auto *value = FindObjectField(record, name); value != nullptr)
                    {
                        AppendCsvValue(out, *value);
                    }
                    // Missing key -> empty cell (delimiter handled above).
                }
                return out;
            },
    };
}

namespace
{

/// Look up @p key on @p record and append its serialized form to @p out.
/// Numbers serialize as compact JSON (so `thread_id=3` not `thread_id=3.0`),
/// strings inline as-is, booleans as `true` / `false`, null/missing as
/// the empty string. Anything else (nested object / array) is rejected
/// in debug builds; the bracketed-regex shape is intentionally flat.
void AppendBracketedField(std::string &out, const LogRecord &record, const std::string &key)
{
    const LogRecord *value = FindObjectField(record, key);
    if (value == nullptr || value->is_null())
    {
        return;
    }
    if (value->is_string())
    {
        out.append(value->get_string());
        return;
    }
    if (value->is_boolean())
    {
        out.append(value->get_boolean() ? "true" : "false");
        return;
    }
    if (value->is_number())
    {
        out.append(CompactJson(*value));
        return;
    }
    // Arrays / objects don't fit the flat bracketed shape.
    assert(false && "BracketedRegex fields must be scalar (string/number/bool/null)");
}

} // namespace

LogFormat BracketedRegex()
{
    return LogFormat{
        .suggestedExtension = ".log",
        .writeHeader = [](const RecordSchema &) { return std::string{}; },
        .writeLine =
            [](const LogRecord &record) {
                std::string out;
                // Format mirrors `BracketedRegexPattern()`. Keep the two in lock-step.
                out.push_back('[');
                AppendBracketedField(out, record, "timestamp");
                out.append("] ");
                AppendBracketedField(out, record, "level");
                out.push_back(' ');
                AppendBracketedField(out, record, "component");
                out.append(" tid=");
                AppendBracketedField(out, record, "thread_id");
                out.append(" | ");
                AppendBracketedField(out, record, "message");
                return out;
            },
    };
}

std::string_view BracketedRegexPattern()
{
    // `timestamp` accepts anything but `]`; `level` / `component` are
    // bare tokens (non-space, non-`|`); `thread_id` is digits-only;
    // `message` greedy-matches the rest of the line. The leading `^`
    // / trailing `$` anchor on the whole line.
    return R"(^\[(?<timestamp>[^\]]+)\] (?<level>\S+) (?<component>\S+) tid=(?<thread_id>\d+) \| (?<message>.*)$)";
}

RecordSchema DeriveSchemaFromRecord(const LogRecord &record)
{
    RecordSchema schema;
    if (!record.is_object())
    {
        return schema;
    }
    for (const auto &[key, _value] : record.get_object())
    {
        schema.emplace_back(key);
    }
    return schema;
}

} // namespace test_common
