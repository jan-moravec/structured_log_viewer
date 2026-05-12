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
    if (selectedValues.empty())
    {
        mEmptySelection = true;
        return;
    }

    // Dedupe so `mAllResolved` is keyed on distinct values regardless
    // of caller-side dedup, and so the bitset/string-set work stays
    // bounded.
    const std::unordered_set<std::string_view, internal::TransparentStringHash, internal::TransparentStringEqual>
        distinct(selectedValues.begin(), selectedValues.end());

    if (dictionary == nullptr)
    {
        mSelectedStrings.reserve(distinct.size());
        for (const std::string_view value : distinct)
        {
            mSelectedStrings.emplace(value);
        }
        return;
    }

    // Indexed by id; ids past `Size()` later route through the
    // past-bitset branch in `MatchesRow`.
    mSelectedIds.assign(static_cast<size_t>(dictionary->Size()), false);
    size_t resolvedCount = 0;
    for (const std::string_view value : distinct)
    {
        const EnumValueId id = dictionary->Find(value);
        if (id == INVALID_ENUM_VALUE_ID)
        {
            // Unresolved -> string-set fallback so post-rebuild evals
            // still match.
            mSelectedStrings.emplace(value);
            continue;
        }
        const auto idx = static_cast<size_t>(id);
        if (idx >= mSelectedIds.size())
        {
            // Defensive: id past the snapshot we sized against (concurrent
            // dict growth between `Size()` and `Find`). Treat as unresolved.
            mSelectedStrings.emplace(value);
            continue;
        }
        mSelectedIds[idx] = true;
        mFastPathArmed = true;
        ++resolvedCount;
    }
    mAllResolved = resolvedCount == distinct.size();
}

bool EnumRowPredicate::MatchesRow(const LogTable &table, size_t row) const
{
    if (mEmptySelection)
    {
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
            if (mAllResolved)
            {
                // Past the bitset, fully resolved -> provably unselected.
                // Soundness: an id past the bitset can only exist because
                // the dictionary grew after we sized against it. Growth
                // only mints ids for values that were not yet in the
                // dictionary, so the new value was never observed when
                // the predicate was built. `mAllResolved` means every
                // selected string did resolve at construction; therefore
                // the just-minted value cannot have been in our selection.
                return false;
            }
            // Past the bitset with stale predicate; fall through to
            // the string set. (Some selected values were unresolved at
            // construction, so an unseen id might correspond to one of
            // them once we compare the bytes.)
        }
        // Slot isn't a `DictRef`; fall through to the string set.
    }

    if (mSelectedStrings.empty())
    {
        return false;
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
    if (mBegin > mEnd)
    {
        return false;
    }
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
                // Clamp negative bounds to 0 so e.g. `[-1, 100]` still
                // matches positive values.
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
    // Try the cheap string-view path; fall back to formatted bytes
    // (numeric/time slots formatted via the column's `printFormat`).
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
