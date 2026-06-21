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

// A CSV cell is bare-safe when it contains no byte that would end the cell
// or trigger RFC-4180 quoting. Mirrors `loglib::BareCellIsSafe` in
// `library/src/parsers/csv_parser.cpp`; duplicated here so `test_common`
// stays loglib-free. Drift is caught by `[csv_parser][round_trip]` tests.
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
        // Null renders as the empty cell -- `CsvParser` omits empty
        // cells from the row, matching its "absent key == monostate"
        // contract.
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
        // Numbers serialise to the same bare token in JSON and CSV.
        out.append(CompactJson(value));
        return;
    }
    // Array or object: embed compact JSON as a single quoted CSV cell so
    // the wide-row column count matches the JSON serialisation. The
    // parser sees this as an opaque string (lossy escape hatch, mirrors
    // logfmt's treatment of nesting).
    AppendCsvQuoted(out, CompactJson(value));
}

// Find @p key inside @p record (which must be an object). Returns a
// pointer to the value, or nullptr when the key is absent.
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
                // Prefer the explicit `paramSchema` arg so callers that
                // pass `Csv()` (no captured schema) + a schema to
                // `TestStructuredLogFile` still get a header. Fall back
                // to the captured one for callers that wired the schema
                // into the factory.
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
                    // Headerless mode: walk the record's lex order.
                    // Produces CSV-shaped rows but no header line, so
                    // `loglib::CsvParser::IsValid` rejects the result.
                    // Real fixtures should pass a non-empty schema.
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
                    // Missing key -> empty cell (handled by the comma
                    // delimiter logic above).
                }
                return out;
            },
    };
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
