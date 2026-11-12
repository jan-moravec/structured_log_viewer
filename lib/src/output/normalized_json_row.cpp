#include "loglib/output/internal/normalized_json_row.hpp"

#include "loglib/core/compact_log_value.hpp"
#include "loglib/core/key_index.hpp"
#include "loglib/core/log_line.hpp"
#include "loglib/core/log_value.hpp"
#include "loglib/table/log_table.hpp"
#include "loglib/time/internal/timestamp_formatter.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <numeric>
#include <optional>
#include <string_view>
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
    // Most text has nothing to escape, so each clean run goes out in one call.
    std::size_t runStart = 0;
    for (std::size_t i = 0; i < input.size(); ++i)
    {
        const auto ch = static_cast<unsigned char>(input[i]);
        if (ch >= JSON_CONTROL_CHAR_LIMIT && ch != '"' && ch != '\\')
        {
            continue;
        }
        out.append(input.data() + runStart, i - runStart);
        runStart = i + 1;
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
            fmt::format_to(std::back_inserter(out), "\\u{:04x}", static_cast<unsigned>(ch));
            break;
        }
    }
    out.append(input.data() + runStart, input.size() - runStart);
}

/** Appends the text in quotes with the characters that JSON requires escaped. */
void AppendJsonString(std::string &out, std::string_view text)
{
    out.push_back('"');
    AppendJsonEscaped(out, text);
    out.push_back('"');
}

/** Appends the time as an ISO 8601 UTC string in quotes, or null if it cannot be formatted. */
void AppendTimestampJson(std::string &out, TimestampFormatter &formatter, TimeStamp timestamp)
{
    const std::size_t savedSize = out.size();
    try
    {
        out.push_back('"');
        formatter.Append(out, timestamp);
        out.push_back('Z');
        out.push_back('"');
    }
    catch (const std::exception &)
    {
        out.resize(savedSize);
        out.append("null");
    }
}

/** Room for any 64-bit integer in decimal, with its sign. */
constexpr std::size_t INTEGER_TEXT_CAPACITY = 24;

/** Appends a whole number in decimal. */
template <typename Integer> void AppendInteger(std::string &out, Integer value)
{
    std::array<char, INTEGER_TEXT_CAPACITY> digits{};
    const auto result = std::to_chars(digits.data(), digits.data() + digits.size(), value);
    out.append(digits.data(), result.ptr);
}

/** Appends a floating point number as JSON: null if it is not finite, and a ".0" if it would read as an integer. */
void AppendDouble(std::string &out, double value)
{
    if (!std::isfinite(value))
    {
        out.append("null");
        return;
    }
    const std::size_t start = out.size();
    fmt::format_to(std::back_inserter(out), "{}", value);
    const std::string_view emitted(out.data() + start, out.size() - start);
    if (emitted.find_first_of(".eE") == std::string_view::npos)
    {
        out.append(".0");
    }
}

} // namespace

namespace
{
constexpr std::uint32_t UNRANKED = UINT32_MAX;
} // namespace

NormalizedJsonRowWriter::NormalizedJsonRowWriter(const LogTable &table)
    : mKeys(&table.Data().Keys()),
      mRankByKey(mKeys->Size(), UNRANKED),
      mKeyPrefix(mKeys->Size()),
      mTimestampFormatter(TimeZoneContext::Utc(), "%FT%T", FractionPrecision::Microseconds)
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

bool NormalizedJsonRowWriter::AppendSlotValue(std::string &out, const LogLine &line, const RowField &field)
{
    const CompactLogValue &slot = *field.slot;
    switch (slot.tag)
    {
    case CompactTag::Monostate:
        return false;
    case CompactTag::MmapSlice:
    {
        const std::optional<std::string_view> bytes = line.PeekStringView(slot);
        if (!bytes.has_value())
        {
            return false;
        }
        AppendJsonString(out, *bytes);
        return true;
    }
    case CompactTag::OwnedString:
        AppendJsonString(out, line.PeekStringView(slot).value_or(std::string_view{}));
        return true;
    case CompactTag::DictRef:
    {
        const LogValue value = slot.Materialise(line.Source(), line.LineId(), field.id);
        const auto *text = std::get_if<std::string_view>(&value);
        if (text == nullptr)
        {
            return false;
        }
        AppendJsonString(out, *text);
        return true;
    }
    case CompactTag::Int64:
        AppendInteger(out, static_cast<std::int64_t>(slot.payload));
        return true;
    case CompactTag::Uint64:
        AppendInteger(out, slot.payload);
        return true;
    case CompactTag::Double:
        AppendDouble(out, std::bit_cast<double>(slot.payload));
        return true;
    case CompactTag::Bool:
        out.append(slot.payload != 0 ? "true" : "false");
        return true;
    case CompactTag::Timestamp:
        AppendTimestampJson(
            out, mTimestampFormatter, TimeStamp{std::chrono::microseconds{static_cast<std::int64_t>(slot.payload)}}
        );
        return true;
    }
    return false;
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
        const std::size_t before = out.size();
        if (!first)
        {
            out.push_back(',');
        }
        out.append(KeyPrefix(field.id));
        if (!AppendSlotValue(out, line, field))
        {
            // A value that cannot be read, such as an unresolved string, leaves the field out.
            out.resize(before);
            continue;
        }
        first = false;
    }
    out.push_back('}');
}

} // namespace loglib::internal
