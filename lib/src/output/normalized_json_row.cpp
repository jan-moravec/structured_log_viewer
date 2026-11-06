#include "loglib/output/internal/normalized_json_row.hpp"

#include "loglib/core/compact_log_value.hpp"
#include "loglib/core/key_index.hpp"
#include "loglib/core/log_line.hpp"
#include "loglib/core/log_value.hpp"
#include "loglib/table/log_table.hpp"

#include <date/date.h>
#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace loglib::internal
{
namespace
{

constexpr unsigned char JSON_CONTROL_CHAR_LIMIT = 0x20U;
constexpr std::size_t ROW_RESERVE_BYTES = 512;

void AppendJsonEscaped(std::string &out, std::string_view input)
{
    // Reserve for the worst `\u00xx` expansion when escaping is needed.
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
        case '"':
            out.append("\\\"");
            break;
        case '\\':
            out.append("\\\\");
            break;
        case '\b':
            out.append("\\b");
            break;
        case '\f':
            out.append("\\f");
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

/// Append an ISO-8601 UTC JSON string, or `null` if formatting fails.
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

namespace
{
constexpr std::uint32_t UNRANKED = UINT32_MAX;
} // namespace

NormalizedJsonRowWriter::NormalizedJsonRowWriter(const LogTable &table)
    : mKeys(&table.Data().Keys()), mRankByKey(mKeys->Size(), UNRANKED), mKeyPrefix(mKeys->Size())
{
    std::uint32_t next = 0;
    for (const Column &column : table.Configuration().Configuration().columns)
    {
        for (const std::string &key : column.keys)
        {
            const KeyId id = mKeys->Find(key);
            if (id != INVALID_KEY_ID && mRankByKey[id] == UNRANKED)
            {
                mRankByKey[id] = next++;
            }
        }
    }
}

const std::string &NormalizedJsonRowWriter::KeyPrefix(KeyId id)
{
    if (id >= mKeyPrefix.size())
    {
        mKeyPrefix.resize(static_cast<std::size_t>(id) + 1);
    }
    std::string &prefix = mKeyPrefix[id];
    if (prefix.empty())
    {
        prefix.push_back('"');
        AppendJsonEscaped(prefix, mKeys->KeyOf(id));
        prefix.append("\":");
    }
    return prefix;
}

void NormalizedJsonRowWriter::Append(const LogLine &line, std::string &out)
{
    mRow.clear();
    for (const auto &[keyId, slot] : line.CompactValues())
    {
        if (slot.tag != CompactTag::Monostate)
        {
            mRow.push_back(RowField{.id = keyId, .slot = &slot});
        }
    }

    // Rows of one table mostly have the same keys, so the order of the previous row usually fits.
    const bool sameKeys = std::ranges::equal(mRow, mPreviousIds, {}, &RowField::id);
    if (!sameKeys)
    {
        mPreviousIds.clear();
        for (const RowField &field : mRow)
        {
            mPreviousIds.push_back(field.id);
        }
        mOrder.resize(mRow.size());
        std::iota(mOrder.begin(), mOrder.end(), std::uint32_t{0});
        const auto rankOf = [this](KeyId id) { return id < mRankByKey.size() ? mRankByKey[id] : UNRANKED; };
        std::ranges::sort(mOrder, [this, &rankOf](std::uint32_t a, std::uint32_t b) {
            const KeyId idA = mRow[a].id;
            const KeyId idB = mRow[b].id;
            const std::uint32_t rankA = rankOf(idA);
            const std::uint32_t rankB = rankOf(idB);
            if (rankA != rankB)
            {
                return rankA < rankB;
            }
            return rankA == UNRANKED && mKeys->KeyOf(idA) < mKeys->KeyOf(idB);
        });
    }

    // Keep a useful minimum capacity for callers that reuse the buffer.
    if (out.capacity() < ROW_RESERVE_BYTES)
    {
        out.reserve(ROW_RESERVE_BYTES);
    }
    out.push_back('{');
    bool first = true;
    for (const std::uint32_t position : mOrder)
    {
        const RowField &field = mRow[position];
        const LogValue value = field.slot->Materialise(line.Source(), line.LineId(), field.id);
        if (std::holds_alternative<std::monostate>(value))
        {
            continue;
        }
        if (!first)
        {
            out.push_back(',');
        }
        first = false;
        out.append(KeyPrefix(field.id));
        AppendValue(out, value);
    }
    out.push_back('}');
}

} // namespace loglib::internal
