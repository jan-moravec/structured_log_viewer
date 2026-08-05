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
    // Peek once to size the reserve before the append loop. A pure
    // `size + 2` reserve is a lower bound that guarantees no growth
    // only on the zero-escape common case; strings with control
    // bytes or embedded quotes/backslashes would otherwise trigger
    // one or two reallocations per value on top of the per-row
    // 512-byte reserve. The scan is cheap and the loop runs once
    // per string value across every exported row.
    //
    // `* 6` covers the pessimistic `\u00xx` expansion for control
    // bytes; the two other escape classes (`\"`, `\\`) only double,
    // but a single scan cannot distinguish them cheaply so we pay
    // the wider reserve when *any* escape is present.
    constexpr std::size_t ESCAPE_WORST_CASE_MULTIPLIER = 6U;
    bool hasEscape = false;
    for (const char c : input)
    {
        const auto ch = static_cast<unsigned char>(c);
        if (ch < JSON_CONTROL_CHAR_LIMIT || ch == '"' || ch == '\\')
        {
            hasEscape = true;
            break;
        }
    }
    const std::size_t worstCase = hasEscape ? input.size() * ESCAPE_WORST_CASE_MULTIPLIER : input.size();
    out.reserve(out.size() + worstCase + 2);
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

/// Serialize @p timestamp as an ISO-8601 UTC JSON string, including
/// its surrounding quotes. Out-of-range values that `date::format`
/// cannot render fall back to unquoted `null` so round-trip readers
/// see a missing timestamp instead of a bogus string.
///
/// The `savedSize` snapshot + `resize` on the exception path is
/// load-bearing: without it, an in-flight failure would leave a
/// dangling opening `"` in @p out. The row's closing `}` would then
/// land inside a string literal, invalidating not just this row but
/// every JSONL line downstream. The rollback pattern keeps the
/// per-row output well-formed even when a single field cannot be
/// serialised.
void AppendTimestampJson(std::string &out, TimeStamp timestamp)
{
    const std::size_t savedSize = out.size();
    try
    {
        const date::sys_time<std::chrono::microseconds> time{timestamp.time_since_epoch()};
        out.push_back('"');
        out.append(date::format("%FT%T", time));
        out.push_back('Z');
        out.push_back('"');
    }
    catch (const std::exception &)
    {
        out.resize(savedSize);
        out.append("null");
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
                AppendTimestampJson(out, arg);
            }
        },
        value
    );
}

} // namespace

void SerializeNormalizedJsonRow(const LogLine &line, const KeyIndex &keys, std::string &out)
{
    // Reserve on the first call against a fresh (or `clear()`ed)
    // buffer so hot loops that reuse @p out do not pay a growth
    // penalty on their first-few rows before capacity stabilises.
    if (out.capacity() < ROW_RESERVE_BYTES)
    {
        out.reserve(ROW_RESERVE_BYTES);
    }
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
}

std::string SerializeNormalizedJsonRow(const LogLine &line, const KeyIndex &keys)
{
    std::string out;
    SerializeNormalizedJsonRow(line, keys, out);
    return out;
}

} // namespace loglib::internal
