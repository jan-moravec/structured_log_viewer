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
        // Empty selection: every row is rejected by `MatchesRow` via
        // the `mEmptySelection` sentinel.
        mEmptySelection = true;
        return;
    }

    // Dedupe the input span before resolving. Two reasons:
    //   1. `mAllResolved` is keyed on *distinct* selected values, so a
    //      caller passing duplicates (e.g. a UI that doesn't dedupe its
    //      multi-select model) can't skew the counter relative to a
    //      caller that does pre-dedupe.
    //   2. The string-set fallback and the bitset are already
    //      idempotent under duplicate insert / set, so the dedupe also
    //      keeps the work bounded.
    const std::unordered_set<std::string_view, internal::TransparentStringHash, internal::TransparentStringEqual>
        distinct(selectedValues.begin(), selectedValues.end());

    if (dictionary == nullptr)
    {
        // No dictionary: nothing to resolve against, so every distinct
        // selected value goes into the string-set fallback.
        mSelectedStrings.reserve(distinct.size());
        for (const std::string_view value : distinct)
        {
            mSelectedStrings.emplace(value);
        }
        return;
    }

    // Indexed by id; values interned past `Size()` later route through
    // the past-bitset branch in `MatchesRow`. When `mAllResolved` is
    // true that branch rejects directly; otherwise we fall back to the
    // string set so a stale predicate still matches newly-interned
    // selected values.
    mSelectedIds.assign(static_cast<size_t>(dictionary->Size()), false);
    size_t resolvedCount = 0;
    for (const std::string_view value : distinct)
    {
        const EnumValueId id = dictionary->Find(value);
        if (id == INVALID_ENUM_VALUE_ID)
        {
            // Unresolved at construction: keep the string so a future
            // (post-rebuild or stale-predicate) eval still matches it.
            mSelectedStrings.emplace(value);
            continue;
        }
        const auto idx = static_cast<size_t>(id);
        if (idx >= mSelectedIds.size())
        {
            // Defensive: dictionary `Find` returned an id past the
            // snapshot it handed us (concurrent growth between
            // `Size()` and `Find`). Treat as unresolved.
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
            if (mAllResolved)
            {
                // Past the bitset and every selected value resolved at
                // construction: provably unselected.
                return false;
            }
            // Past the bitset but some selected values were unresolved
            // at construction (stale predicate / newly-interned value):
            // fall through to the string-set check below.
        }
        // Slot isn't a `DictRef` (column not yet promoted, or the slot
        // is monostate/numeric). Fall through to the string-set path.
    }

    if (mSelectedStrings.empty())
    {
        // Armed + fully resolved + slot non-`DictRef`: nothing else to
        // try. Rejecting matches the strings-side-of-the-fence shape
        // (a non-string slot is never in a string set).
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
        // Inverted range: the user picked an `end < begin` window.
        // Reject every row rather than producing implementation-defined
        // behaviour from the per-type compares below.
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
