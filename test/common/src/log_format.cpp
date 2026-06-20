#include <test_common/log_format.hpp>

#include <glaze/glaze.hpp>

#include <algorithm>
#include <string>
#include <string_view>

namespace test_common
{

namespace
{

// A value is bare-safe in logfmt when it contains no byte that would end the
// token or need escaping. Mirrors `loglib::BareValueIsSafe` in
// `library/src/parsers/logfmt_parser.cpp` (kept in sync by the round-trip
// test); duplicated here so `test_common` stays loglib-free.
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
// Mirrors `loglib::AppendQuotedString` in the logfmt parser.
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
        // Null round-trips through logfmt as an empty value (`key=`).
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
        // Numbers serialize to the same bare token in JSON and logfmt.
        out.append(CompactJson(value));
        return;
    }
    // Array or object: embed compact JSON as a single quoted logfmt string so
    // the wide-row field count matches the JSON serialization.
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
                    // Non-object records have no key=value structure; fall back
                    // to compact JSON (the generators only ever emit objects).
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
                    out.append(key);
                    out.push_back('=');
                    AppendLogfmtValue(out, value);
                }
                return out;
            },
    };
}

} // namespace test_common
