// `loglib` has no Qt in its include chain, so the TBB-before-Qt
// ordering that `app/` needs doesn't apply here.
#include "loglib/log_filter.hpp"

#include "loglib/log_table.hpp"
#include "loglib/log_value.hpp"

#include <oneapi/tbb/blocked_range.h>
#include <oneapi/tbb/enumerable_thread_specific.h>
#include <oneapi/tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

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
    // of caller-side dedup, and so the bitset / string-set work stays
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

    // Indexed by id; ids past `Size()` later go through the
    // past-bitset branch in `MatchesRow`.
    mSelectedIds.assign(static_cast<size_t>(dictionary->Size()), false);
    size_t resolvedCount = 0;
    for (const std::string_view value : distinct)
    {
        const EnumValueId id = dictionary->Find(value);
        if (id == INVALID_ENUM_VALUE_ID)
        {
            // Unresolved -> string-set fallback so the post-rebuild
            // path still matches.
            mSelectedStrings.emplace(value);
            continue;
        }
        const auto idx = static_cast<size_t>(id);
        if (idx >= mSelectedIds.size())
        {
            // Defensive: id past the snapshot we sized against
            // (concurrent dict growth between `Size()` and `Find`).
            // Treat as unresolved.
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
                // An id past the bitset can only exist because the
                // dictionary grew after we sized against it. Growth
                // mints ids only for new values, and `mAllResolved`
                // says every selected string already resolved at
                // construction, so the new value cannot be selected.
                return false;
            }
            // Past the bitset with a stale predicate: fall through
            // to the string set. Some selected values were unresolved
            // at construction, so this id may still correspond to one
            // of them once the bytes are compared.
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
                // Clamp negative bounds to 0 so e.g. `[-1, 100]`
                // still matches positive values.
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

NumericRangeRowPredicate::NumericRangeRowPredicate(
    size_t columnIndex, std::optional<double> minValue, std::optional<double> maxValue
)
    : mColumnIndex(columnIndex), mMin(minValue), mMax(maxValue)
{
    // `NaN` bounds collapse to "unbounded on that side" so callers can
    // pass a parsed-but-malformed UI input without silently producing
    // an always-reject filter. Mirrors the IEEE-754 ordering: nothing
    // compares either side of NaN, so a real NaN bound rejects every
    // row -- almost certainly not what the user meant.
    if (mMin.has_value() && std::isnan(*mMin))
    {
        mMin.reset();
    }
    if (mMax.has_value() && std::isnan(*mMax))
    {
        mMax.reset();
    }
}

bool NumericRangeRowPredicate::MatchesRow(const LogTable &table, size_t row) const
{
    const LogValue value = table.GetValue(row, mColumnIndex);
    return std::visit(
        [this](const auto &alt) -> bool {
            using T = std::decay_t<decltype(alt)>;
            // MSVC warns C4702 if an `if constexpr` branch returns early
            // because the trailing common code becomes unreachable for
            // that instantiation. Collect into an optional and exit
            // once after the `if constexpr` chain instead.
            std::optional<double> asDouble;
            if constexpr (std::is_same_v<T, double>)
            {
                if (!std::isnan(alt))
                {
                    asDouble = alt;
                }
            }
            else if constexpr (std::is_same_v<T, int64_t> || std::is_same_v<T, uint64_t>)
            {
                // Above 2^53 the cast loses precision; documented in
                // the header. Tests cover the small/medium-range cases.
                asDouble = static_cast<double>(alt);
            }
            if (!asDouble.has_value())
            {
                return false;
            }
            const double numeric = *asDouble;
            if (mMin.has_value() && numeric < *mMin)
            {
                return false;
            }
            if (mMax.has_value() && numeric > *mMax)
            {
                return false;
            }
            return true;
        },
        value
    );
}

BoolRowPredicate::BoolRowPredicate(size_t columnIndex, bool includeTrue, bool includeFalse)
    : mColumnIndex(columnIndex), mIncludeTrue(includeTrue), mIncludeFalse(includeFalse)
{
}

bool BoolRowPredicate::MatchesRow(const LogTable &table, size_t row) const
{
    if (!mIncludeTrue && !mIncludeFalse)
    {
        return false;
    }
    const LogValue value = table.GetValue(row, mColumnIndex);
    if (const auto *b = std::get_if<bool>(&value); b != nullptr)
    {
        return *b ? mIncludeTrue : mIncludeFalse;
    }
    return false;
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
    // One-walk path: `GetValueOrFormatted` resolves the slot once and
    // either returns its bytes directly (mmap-aliased / dict-resolved
    // string slots) or formats numeric/time slots into the
    // `thread_local` buffer. The old two-call shape walked the line
    // twice (`GetValue` + `GetFormattedValue`) for every non-string
    // column hit.
    //
    // `thread_local` is safe under `tbb::parallel_for`: each TBB
    // worker has its own buffer. Re-entrancy within a thread is fine
    // because `mMatch` doesn't call back into `MatchesRow`.
    thread_local std::string buffer;
    const std::string_view bytes = table.GetValueOrFormatted(row, mColumnIndex, buffer);
    return mMatch(bytes);
}

std::vector<size_t> FilterAcceptedRows(const LogTable &table, std::span<const RowPredicate> predicates)
{
    const size_t rowCount = table.RowCount();
    std::vector<size_t> accepted;

    if (predicates.empty())
    {
        // Identity case: hand back `[0, rowCount)` so callers can
        // share one code path with the filtered case. The bottleneck
        // is filter + predicate, so the sequential fill is cheap.
        accepted.resize(rowCount);
        std::iota(accepted.begin(), accepted.end(), size_t{0});
        return accepted;
    }

    if (rowCount == 0)
    {
        return accepted;
    }

    // Parallel filter pass: each TBB worker drains a `blocked_range`
    // into its thread-local bucket. We coalesce the buckets and sort
    // the result so callers get rows in ascending order. Buckets grow
    // proportional to `rowCount / num_threads`, so the final
    // single-threaded sort is cheap.
    tbb::enumerable_thread_specific<std::vector<size_t>> buckets;
    tbb::parallel_for(
        tbb::blocked_range<size_t>(0, rowCount),
        [&table, predicates, &buckets](const tbb::blocked_range<size_t> &range) {
            auto &local = buckets.local();
            local.reserve(local.size() + range.size());
            for (size_t row = range.begin(); row != range.end(); ++row)
            {
                const bool keep = std::ranges::all_of(predicates, [&table, row](const RowPredicate &predicate) {
                    return MatchesRow(predicate, table, row);
                });
                if (keep)
                {
                    local.push_back(row);
                }
            }
        }
    );

    size_t total = 0;
    for (const auto &bucket : buckets)
    {
        total += bucket.size();
    }
    accepted.reserve(total);
    for (const auto &bucket : buckets)
    {
        accepted.insert(accepted.end(), bucket.begin(), bucket.end());
    }
    std::ranges::sort(accepted);
    return accepted;
}

} // namespace loglib
