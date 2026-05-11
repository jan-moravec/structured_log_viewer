#include "loglib/log_filter.hpp"

#include "loglib/log_table.hpp"
#include "loglib/log_value.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace loglib
{

EnumRowPredicate::EnumRowPredicate(
    size_t columnIndex, std::span<const std::string_view> selectedValues, const EnumDictionary *dictionary
)
    : mColumnIndex(columnIndex)
{
    mSelectedStrings.reserve(selectedValues.size());
    for (const std::string_view value : selectedValues)
    {
        mSelectedStrings.emplace(value);
    }

    if (dictionary == nullptr)
    {
        return;
    }
    // Indexed by id; values interned past `Size()` later are handled by
    // the out-of-range branch in `MatchesRow` (which rejects -- the
    // GUI's `enumColumnsChanged` gate rebuilds the predicate whenever
    // a *selected* value gains a new id, so out-of-range here can only
    // be an unselected value).
    mSelectedIds.assign(static_cast<size_t>(dictionary->Size()), false);
    for (const std::string_view value : selectedValues)
    {
        const EnumValueId id = dictionary->Find(value);
        if (id == INVALID_ENUM_VALUE_ID)
        {
            continue;
        }
        const auto idx = static_cast<size_t>(id);
        if (idx >= mSelectedIds.size())
        {
            continue;
        }
        mSelectedIds[idx] = true;
        mFastPathArmed = true;
    }
}

bool EnumRowPredicate::MatchesRow(const LogTable &table, size_t row) const
{
    if (mSelectedStrings.empty())
    {
        // Empty selection hides every row (matches `EnumFilterRule`).
        return false;
    }

    if (mFastPathArmed)
    {
        if (const auto id = table.GetEnumValueId(row, mColumnIndex); id.has_value())
        {
            const auto idx = static_cast<size_t>(*id);
            if (idx < mSelectedIds.size())
            {
                return mSelectedIds[idx];
            }
            // Past the bitset: provably unselected (see header).
            return false;
        }
        // Slot isn't a `DictRef` (column not yet promoted, or the slot
        // is monostate/numeric). Fall through to the string-set path.
    }

    const LogValue value = table.GetValue(row, mColumnIndex);
    if (const auto *sv = std::get_if<std::string_view>(&value); sv != nullptr)
    {
        return mSelectedStrings.contains(*sv);
    }
    if (const auto *s = std::get_if<std::string>(&value); s != nullptr)
    {
        return mSelectedStrings.contains(*s);
    }
    return false;
}

TimeRangeRowPredicate::TimeRangeRowPredicate(size_t columnIndex, int64_t begin, int64_t end)
    : mColumnIndex(columnIndex), mBegin(begin), mEnd(end)
{
}

bool TimeRangeRowPredicate::MatchesRow(const LogTable &table, size_t row) const
{
    const LogValue value = table.GetValue(row, mColumnIndex);
    return std::visit(
        [this](const auto &alt) -> bool {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, TimeStamp>)
            {
                const int64_t ts = alt.time_since_epoch().count();
                return ts >= mBegin && ts <= mEnd;
            }
            else if constexpr (std::is_same_v<T, int64_t>)
            {
                return alt >= mBegin && alt <= mEnd;
            }
            else if constexpr (std::is_same_v<T, uint64_t>)
            {
                // Comparing across signed/unsigned: clamp `mBegin`/`mEnd`
                // negatives to 0 so e.g. `[-1, 100]` keeps positive values.
                const uint64_t lo = mBegin < 0 ? 0U : static_cast<uint64_t>(mBegin);
                const uint64_t hi = mEnd < 0 ? 0U : static_cast<uint64_t>(mEnd);
                return alt >= lo && alt <= hi;
            }
            else
            {
                return false;
            }
        },
        value
    );
}

CallbackStringRowPredicate::CallbackStringRowPredicate(size_t columnIndex, MatchFn match)
    : mColumnIndex(columnIndex), mMatch(std::move(match))
{
}

bool CallbackStringRowPredicate::MatchesRow(const LogTable &table, size_t row) const
{
    if (!mMatch)
    {
        return false;
    }
    // Try the cheap string-view fast path; fall back to a formatted
    // string copy when the slot is non-string (integer / float / time
    // formatted via the column's `printFormat`).
    const LogValue value = table.GetValue(row, mColumnIndex);
    if (const auto *sv = std::get_if<std::string_view>(&value); sv != nullptr)
    {
        return mMatch(*sv);
    }
    if (const auto *s = std::get_if<std::string>(&value); s != nullptr)
    {
        return mMatch(std::string_view(*s));
    }
    const std::string formatted = table.GetFormattedValue(row, mColumnIndex);
    return mMatch(std::string_view(formatted));
}

} // namespace loglib
