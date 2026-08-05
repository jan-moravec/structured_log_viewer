#include "loglib/internal/normalized_json_row.hpp"

#include "loglib/internal/compact_log_value.hpp"
#include "loglib/key_index.hpp"
#include "loglib/log_line.hpp"
#include "loglib/log_value.hpp"

#include <date/date.h>
#include <fmt/format.h>

#include <cmath>
#include <cstdint>
#include <string_view>
#include <type_traits>
#include <variant>

namespace loglib::internal
{
namespace
{

constexpr unsigned char JSON_CONTROL_CHAR_LIMIT = 0x20U;
constexpr std::size_t ROW_RESERVE_BYTES = 512;

void AppendJsonEscaped(std::string &out, std::string_view input)
{
    // Reserve for the common case (no control bytes). A wide-row export
    // otherwise triggers repeated small growths on top of the per-row
    // 512-byte reserve, and this hot loop runs once per string value.
    out.reserve(out.size() + input.size() + 2);
    for (const char c : input)
    {
        const auto ch = static_cast<unsigned char>(c);
        switch (ch)
        {
        case '"': out.append("\\\""); break;
        case '\\': out.append("\\\\"); break;
        case '\b': out.append("\\b"); break;
        case '\f': out.append("\\f"); break;
        case '\n': out.append("\\n"); break;
        case '\r': out.append("\\r"); break;
        case '\t': out.append("\\t"); break;
        default:
            if (ch < JSON_CONTROL_CHAR_LIMIT)
            {
                fmt::format_to(std::back_inserter(out), "\\u{:04x}", static_cast<unsigned>(ch));
            }
            else
            {
                out.push_back(c);
            }
            break;
        }
    }
}

void AppendTimestamp(std::string &out, TimeStamp timestamp)
{
    try
    {
        const date::sys_time<std::chrono::microseconds> time{timestamp.time_since_epoch()};
        out.append(date::format("%FT%T", time));
        out.push_back('Z');
    }
    catch (const std::exception &)
    {
        fmt::format_to(std::back_inserter(out), "{}", timestamp.time_since_epoch().count());
    }
}

void AppendValue(std::string &out, const LogValue &value)
{
    std::visit(
        [&out]<typename T>(const T &arg) {
            if constexpr (std::is_same_v<T, std::monostate>)
            {
                out.append("null");
            }
            else if constexpr (std::is_same_v<T, bool>)
            {
                out.append(arg ? "true" : "false");
            }
            else if constexpr (std::is_same_v<T, std::int64_t> || std::is_same_v<T, std::uint64_t>)
            {
                fmt::format_to(std::back_inserter(out), "{}", arg);
            }
            else if constexpr (std::is_same_v<T, double>)
            {
                if (!std::isfinite(arg))
                {
                    out.append("null");
                    return;
                }
                const std::size_t start = out.size();
                fmt::format_to(std::back_inserter(out), "{}", arg);
                const std::string_view emitted(out.data() + start, out.size() - start);
                if (emitted.find_first_of(".eE") == std::string_view::npos)
                {
                    out.append(".0");
                }
            }
            else if constexpr (std::is_same_v<T, std::string_view> || std::is_same_v<T, std::string>)
            {
                out.push_back('"');
                AppendJsonEscaped(out, std::string_view(arg));
                out.push_back('"');
            }
            else if constexpr (std::is_same_v<T, TimeStamp>)
            {
                out.push_back('"');
                AppendTimestamp(out, arg);
                out.push_back('"');
            }
        },
        value
    );
}

} // namespace

std::string SerializeNormalizedJsonRow(const LogLine &line, const KeyIndex &keys)
{
    std::string out;
    out.reserve(ROW_RESERVE_BYTES);
    out.push_back('{');
    bool first = true;
    for (const auto &[keyId, slot] : line.CompactValues())
    {
        if (slot.tag == CompactTag::Monostate)
        {
            continue;
        }
        LogValue value = slot.Materialise(line.Source(), line.LineId(), keyId);
        if (std::holds_alternative<std::monostate>(value))
        {
            continue;
        }
        if (!first)
        {
            out.push_back(',');
        }
        first = false;
        out.push_back('"');
        AppendJsonEscaped(out, keys.KeyOf(keyId));
        out.append("\":");
        AppendValue(out, value);
    }
    out.push_back('}');
    return out;
}

} // namespace loglib::internal
