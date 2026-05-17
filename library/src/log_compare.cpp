// `loglib` has no Qt in its include chain, so the TBB / Qt `emit`
// collision that bites `app/` doesn't apply -- normal include order
// is fine.
#include "loglib/log_compare.hpp"

#include "loglib/log_configuration.hpp"
#include "loglib/log_level.hpp"
#include "loglib/log_table.hpp"
#include "loglib/log_value.hpp"

#include <oneapi/tbb/blocked_range.h>
#include <oneapi/tbb/parallel_for.h>
#include <oneapi/tbb/parallel_sort.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace loglib
{

EnumDictRank::EnumDictRank(const EnumDictionary &dictionary)
{
    const auto &values = dictionary.Values();
    const size_t size = values.size();
    mIdToRank.resize(size);
    // Sort indices by value bytes, then invert: `rank[order[k]] = k`.
    // `std::iota`, not `std::ranges::iota` (C++23, missing on
    // AppleClang 17 libc++).
    std::vector<uint16_t> order(size);
    std::iota(order.begin(), order.end(), uint16_t{0});
    std::ranges::sort(order, [&values](uint16_t a, uint16_t b) {
        return std::string_view(values[a]) < std::string_view(values[b]);
    });
    for (size_t rank = 0; rank < order.size(); ++rank)
    {
        mIdToRank[order[rank]] = static_cast<uint16_t>(rank);
    }
}

uint16_t EnumDictRank::RankOf(EnumValueId id) const noexcept
{
    const auto idx = static_cast<size_t>(id);
    if (idx >= mIdToRank.size())
    {
        // Id minted after the last rebuild sorts after every known value.
        return static_cast<uint16_t>(mIdToRank.size());
    }
    return mIdToRank[idx];
}

uint16_t EnumDictRank::DictSize() const noexcept
{
    assert(mIdToRank.size() <= std::numeric_limits<uint16_t>::max() && "EnumDictRank exceeds uint16_t capacity");
    return static_cast<uint16_t>(mIdToRank.size());
}

namespace
{

/// Three-way compare on a totally-ordered primitive.
template <class T> int ThreeWay(T lhs, T rhs) noexcept
{
    if (lhs < rhs)
    {
        return -1;
    }
    if (rhs < lhs)
    {
        return 1;
    }
    return 0;
}

/// Three-way `double` compare; NaN sinks to the tail (NaN==NaN).
int ThreeWayDouble(double lhs, double rhs) noexcept
{
    const bool lhsNan = std::isnan(lhs);
    const bool rhsNan = std::isnan(rhs);
    if (lhsNan && rhsNan)
    {
        return 0;
    }
    if (lhsNan)
    {
        return 1;
    }
    if (rhsNan)
    {
        return -1;
    }
    return ThreeWay(lhs, rhs);
}

LogValue LoadValue(const LogTable &table, size_t row, size_t column)
{
    return table.GetValue(row, column);
}

/// Byte-wise compare. Two string slots compare directly; mixed types
/// fall back to formatted bytes (deterministic order without taking a
/// position on `numeric < string`). Caller handles monostate.
///
/// The mixed-type fallback is dead code on the production hot path:
/// `LogConfiguration::Type` pins which slot variants a column holds,
/// and the typed `Compare*` helpers consume them before we get here.
/// It exists so unit / corruption tests still see deterministic
/// output, and so a future type whose `CompareRows` dispatch falls
/// through to `CompareString` keeps working without a per-call
/// special case.
int CompareLogValuesBytewise(const LogTable &table, size_t lhsRow, size_t rhsRow, size_t column)
{
    const LogValue lhs = LoadValue(table, lhsRow, column);
    const LogValue rhs = LoadValue(table, rhsRow, column);

    auto asStringView = [](const LogValue &v) -> std::optional<std::string_view> {
        if (const auto *sv = std::get_if<std::string_view>(&v); sv != nullptr)
        {
            return *sv;
        }
        if (const auto *s = std::get_if<std::string>(&v); s != nullptr)
        {
            return std::string_view(*s);
        }
        return std::nullopt;
    };

    const auto lhsSv = asStringView(lhs);
    const auto rhsSv = asStringView(rhs);
    if (lhsSv.has_value() && rhsSv.has_value())
    {
        return ThreeWay(*lhsSv, *rhsSv);
    }

    // Format both sides through the column's `printFormat`. Buffers
    // are `thread_local` so steady-state allocation is zero. Not
    // re-entrant -- the debug guard below catches accidental nesting.
#ifndef NDEBUG
    // MSVC rejects `static` inside local classes, so the counter
    // lives at function scope and the RAII guard takes it by reference.
    thread_local int sDepth = 0;
    class ReentryGuard
    {
    public:
        explicit ReentryGuard(int &d)
            : mDepth(d)
        {
            assert(mDepth == 0 && "CompareLogValuesBytewise is not re-entrant (thread_local buffers)");
            ++mDepth;
        }
        ~ReentryGuard()
        {
            --mDepth;
        }
        ReentryGuard(const ReentryGuard &) = delete;
        ReentryGuard(ReentryGuard &&) = delete;
        ReentryGuard &operator=(const ReentryGuard &) = delete;
        ReentryGuard &operator=(ReentryGuard &&) = delete;

    private:
        int &mDepth;
    };
    const ReentryGuard guard(sDepth);
#endif
    thread_local std::string lhsFormatted;
    thread_local std::string rhsFormatted;
    lhsFormatted = table.GetFormattedValue(lhsRow, column);
    rhsFormatted = table.GetFormattedValue(rhsRow, column);
    return ThreeWay(std::string_view(lhsFormatted), std::string_view(rhsFormatted));
}

/// monostate vs monostate -> 0; monostate vs other -> +1 (monostate
/// sorts to the tail). `nullopt` when neither side is monostate.
std::optional<int> CompareMonostateOrder(const LogValue &lhs, const LogValue &rhs)
{
    const bool lhsEmpty = std::holds_alternative<std::monostate>(lhs);
    const bool rhsEmpty = std::holds_alternative<std::monostate>(rhs);
    if (lhsEmpty && rhsEmpty)
    {
        return 0;
    }
    if (lhsEmpty)
    {
        return 1;
    }
    if (rhsEmpty)
    {
        return -1;
    }
    return std::nullopt;
}

/// Shared "extract to T, three-way on extracted, unextracted joins
/// the tail bucket" shape. Routing monostate through `extract`
/// (rather than a parallel short-circuit) makes monostate, NaN-in-Int,
/// stray-string-in-Floating, etc. all compare equal pairwise -- the
/// tail-bucket invariant the header promises.
template <class Extract, class Compare>
int CompareTyped(const LogValue &lhs, const LogValue &rhs, Extract extract, Compare cmp)
{
    const bool lhsMono = std::holds_alternative<std::monostate>(lhs);
    const bool rhsMono = std::holds_alternative<std::monostate>(rhs);
    const auto lhsX = lhsMono ? std::nullopt : extract(lhs);
    const auto rhsX = rhsMono ? std::nullopt : extract(rhs);
    if (lhsX.has_value() && rhsX.has_value())
    {
        return cmp(*lhsX, *rhsX);
    }
    if (lhsX.has_value())
    {
        return -1;
    }
    if (rhsX.has_value())
    {
        return 1;
    }
    return 0;
}

int CompareBool(const LogValue &lhs, const LogValue &rhs)
{
    auto toBool = [](const LogValue &v) -> std::optional<bool> {
        if (const auto *b = std::get_if<bool>(&v); b != nullptr)
        {
            return *b;
        }
        return std::nullopt;
    };
    // `false < true` follows `int(false) < int(true)`; non-bool slots
    // join the tail bucket via `CompareTyped`.
    return CompareTyped(lhs, rhs, toBool, [](bool a, bool b) { return ThreeWay(a, b); });
}

int CompareInteger(const LogValue &lhs, const LogValue &rhs)
{
    auto toInt = [](const LogValue &v) -> std::optional<int64_t> {
        if (const auto *i = std::get_if<int64_t>(&v); i != nullptr)
        {
            return *i;
        }
        if (const auto *u = std::get_if<uint64_t>(&v); u != nullptr)
        {
            // Order-preserving clamp: oversized uints sort at INT64_MAX.
            constexpr auto MAX = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
            return *u > MAX ? std::numeric_limits<int64_t>::max() : static_cast<int64_t>(*u);
        }
        if (const auto *d = std::get_if<double>(&v); d != nullptr)
        {
            // NaN -> tail. `static_cast<int64_t>(NaN)` is UB; clamp
            // finite extremes to INT64 limits instead of casting them.
            if (std::isnan(*d))
            {
                return std::nullopt;
            }
            if (*d >= static_cast<double>(std::numeric_limits<int64_t>::max()))
            {
                return std::numeric_limits<int64_t>::max();
            }
            if (*d <= static_cast<double>(std::numeric_limits<int64_t>::min()))
            {
                return std::numeric_limits<int64_t>::min();
            }
            return static_cast<int64_t>(*d);
        }
        return std::nullopt;
    };

    return CompareTyped(lhs, rhs, toInt, [](int64_t a, int64_t b) { return ThreeWay(a, b); });
}

int CompareFloating(const LogValue &lhs, const LogValue &rhs)
{
    auto toDouble = [](const LogValue &v) -> std::optional<double> {
        if (const auto *d = std::get_if<double>(&v); d != nullptr)
        {
            return *d;
        }
        if (const auto *i = std::get_if<int64_t>(&v); i != nullptr)
        {
            return static_cast<double>(*i);
        }
        if (const auto *u = std::get_if<uint64_t>(&v); u != nullptr)
        {
            return static_cast<double>(*u);
        }
        return std::nullopt;
    };

    return CompareTyped(lhs, rhs, toDouble, [](double a, double b) { return ThreeWayDouble(a, b); });
}

int CompareTime(const LogValue &lhs, const LogValue &rhs)
{
    auto toMicros = [](const LogValue &v) -> std::optional<int64_t> {
        if (const auto *t = std::get_if<TimeStamp>(&v); t != nullptr)
        {
            return t->time_since_epoch().count();
        }
        if (const auto *i = std::get_if<int64_t>(&v); i != nullptr)
        {
            return *i;
        }
        if (const auto *u = std::get_if<uint64_t>(&v); u != nullptr)
        {
            // Match `CompareInteger` / `TimeRangeRowPredicate`:
            // order-preserving clamp, not wraparound.
            constexpr auto MAX = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
            return *u > MAX ? std::numeric_limits<int64_t>::max() : static_cast<int64_t>(*u);
        }
        return std::nullopt;
    };
    return CompareTyped(lhs, rhs, toMicros, [](int64_t a, int64_t b) { return ThreeWay(a, b); });
}

int CompareEnum(const LogTable &table, size_t lhsRow, size_t rhsRow, size_t column, const EnumDictRank *rank)
{
    const auto lhsId = table.GetEnumValueId(lhsRow, column);
    const auto rhsId = table.GetEnumValueId(rhsRow, column);

    if (lhsId.has_value() && rhsId.has_value() && rank != nullptr)
    {
        return ThreeWay(rank->RankOf(*lhsId), rank->RankOf(*rhsId));
    }
    if (lhsId.has_value() && rhsId.has_value() && rank == nullptr)
    {
        // No rank table: byte-wise fallback. Hit only in the
        // promote -> first-rebuild transition.
        return CompareLogValuesBytewise(table, lhsRow, rhsRow, column);
    }

    // One or both sides are non-`DictRef` (monostate, unpromoted
    // string, wrong-type, over-cap-length). Tail-bucket invariant:
    // every non-`DictRef` slot sorts strictly after every
    // `DictRef`-resolved slot and equal to every other non-`DictRef`
    // slot. Mirrors `SortPermutationByColumn`'s sentinel collapse so
    // the streaming-insert comparator and the bulk re-sort agree on
    // placement. Pinned by `TestCompareEnumNonDictRefSlotsAllTailEqual`.
    if (!lhsId.has_value() && !rhsId.has_value())
    {
        return 0;
    }
    if (!lhsId.has_value())
    {
        return 1;
    }
    return -1;
}

int CompareString(const LogTable &table, size_t lhsRow, size_t rhsRow, size_t column)
{
    const LogValue lhs = LoadValue(table, lhsRow, column);
    const LogValue rhs = LoadValue(table, rhsRow, column);
    if (const auto order = CompareMonostateOrder(lhs, rhs); order.has_value())
    {
        return *order;
    }
    return CompareLogValuesBytewise(table, lhsRow, rhsRow, column);
}

/// `Type::Level` rows compare by canonical `LogLevel` ordinal (Trace
/// < Debug < ... < Fatal). Slots that don't resolve to a canonical
/// level (monostate, unmapped string, missing cache) join the tail
/// bucket -- same invariant `CompareEnum` enforces.
int CompareLevel(const LogTable &table, size_t lhsRow, size_t rhsRow, size_t column)
{
    const auto lhsLevel = table.GetLevelForRow(lhsRow, column);
    const auto rhsLevel = table.GetLevelForRow(rhsRow, column);
    if (lhsLevel.has_value() && rhsLevel.has_value())
    {
        return ThreeWay(static_cast<uint8_t>(*lhsLevel), static_cast<uint8_t>(*rhsLevel));
    }
    if (!lhsLevel.has_value() && !rhsLevel.has_value())
    {
        return 0;
    }
    if (!lhsLevel.has_value())
    {
        return 1;
    }
    return -1;
}

} // namespace

int CompareRows(
    const LogTable &table, size_t lhsRow, size_t rhsRow, size_t columnIndex, const EnumDictRank *rankForEnumColumn
)
{
    if (lhsRow == rhsRow)
    {
        return 0;
    }
    const auto &columns = table.Configuration().Configuration().columns;
    if (columnIndex >= columns.size())
    {
        return 0;
    }
    const LogConfiguration::Type type = columns[columnIndex].type;

    switch (type)
    {
    case LogConfiguration::Type::Boolean:
        return CompareBool(LoadValue(table, lhsRow, columnIndex), LoadValue(table, rhsRow, columnIndex));
    case LogConfiguration::Type::Integer:
        return CompareInteger(LoadValue(table, lhsRow, columnIndex), LoadValue(table, rhsRow, columnIndex));
    case LogConfiguration::Type::Floating:
    case LogConfiguration::Type::Number:
        return CompareFloating(LoadValue(table, lhsRow, columnIndex), LoadValue(table, rhsRow, columnIndex));
    case LogConfiguration::Type::Time:
        return CompareTime(LoadValue(table, lhsRow, columnIndex), LoadValue(table, rhsRow, columnIndex));
    case LogConfiguration::Type::Enumeration:
        return CompareEnum(table, lhsRow, rhsRow, columnIndex, rankForEnumColumn);
    case LogConfiguration::Type::Level:
        return CompareLevel(table, lhsRow, rhsRow, columnIndex);
    case LogConfiguration::Type::String:
    case LogConfiguration::Type::Any:
    case LogConfiguration::Type::Unknown:
    default:
        return CompareString(table, lhsRow, rhsRow, columnIndex);
    }
}

std::vector<size_t> SortPermutationByColumn(
    const LogTable &table,
    std::span<const size_t> logRows,
    size_t columnIndex,
    bool ascending,
    const EnumDictRank *rankForEnumColumn
)
{
    const size_t n = logRows.size();
    std::vector<size_t> permutation(n);
    // `std::iota`, not `std::ranges::iota` (C++23, missing on
    // AppleClang 17 libc++).
    std::iota(permutation.begin(), permutation.end(), size_t{0});
    if (n <= 1)
    {
        return permutation;
    }

    const auto &columns = table.Configuration().Configuration().columns;
    const bool columnInRange = columnIndex < columns.size();
    const bool isEnum = columnInRange && columns[columnIndex].type == LogConfiguration::Type::Enumeration;
    const bool isLevel = columnInRange && columns[columnIndex].type == LogConfiguration::Type::Level;

    // Fast path: enum column with a precomputed rank table. Pre-
    // materialise a `uint16_t` rank per row in parallel; the sort
    // comparator is then a branch-free integer compare with input-
    // index tie-break. Eliminates the per-compare `GetEnumValueId`
    // walk that dominates `CompareRows` on the enum path.
    if (isEnum && rankForEnumColumn != nullptr)
    {
        // Sentinel: a slot that doesn't resolve to a `DictRef` (or an
        // id past the rank table) sorts after every ranked id. Matches
        // the tail-bucket invariant on `CompareRows`.
        const uint16_t sentinel = rankForEnumColumn->DictSize();
        std::vector<uint16_t> rankForRow(n);
        tbb::parallel_for(
            tbb::blocked_range<size_t>(0, n),
            [&table, columnIndex, rankForEnumColumn, sentinel, &logRows, &rankForRow](
                const tbb::blocked_range<size_t> &range
            ) {
                for (size_t i = range.begin(); i != range.end(); ++i)
                {
                    const auto id = table.GetEnumValueId(logRows[i], columnIndex);
                    rankForRow[i] = id.has_value() ? rankForEnumColumn->RankOf(*id) : sentinel;
                }
            }
        );

        // `tbb::parallel_sort` is not stable, but the input-index
        // tie-break gives a strict total order -- effectively
        // `std::stable_sort` semantics for callers that rely on
        // insertion-order ties.
        if (ascending)
        {
            tbb::parallel_sort(permutation.begin(), permutation.end(), [&rankForRow](size_t a, size_t b) {
                if (rankForRow[a] != rankForRow[b])
                {
                    return rankForRow[a] < rankForRow[b];
                }
                return a < b;
            });
        }
        else
        {
            // Descending: invert the primary order, keep the
            // secondary (input index) ascending. Matches legacy
            // semantics where ties always resolved to source-row
            // ascending regardless of the user's chosen direction.
            tbb::parallel_sort(permutation.begin(), permutation.end(), [&rankForRow](size_t a, size_t b) {
                if (rankForRow[a] != rankForRow[b])
                {
                    return rankForRow[a] > rankForRow[b];
                }
                return a < b;
            });
        }
        return permutation;
    }

    // Level fast path: pre-materialise canonical `LogLevel` ordinals
    // per row in parallel. Unresolved slots get an out-of-range
    // sentinel so they sort after every resolved level, matching the
    // `CompareLevel` tail-bucket invariant. The rank cache is hoisted
    // once outside the parallel loop -- inside the loop the canonical
    // rank lookup collapses to an indexed `ranks[id]` read, skipping
    // the per-row `mLevelRankCache.find(header)` walk that
    // `GetLevelForRow` would do.
    if (isLevel)
    {
        constexpr auto SENTINEL = static_cast<uint8_t>(std::numeric_limits<uint8_t>::max());
        std::vector<uint8_t> rankForRow(n);
        const std::vector<LogLevel> *ranks = table.LevelRankCache(columns[columnIndex].header);
        if (ranks == nullptr)
        {
            // Configured `Type::Level` column with no observations yet
            // (e.g. a saved config loaded before any batches arrive):
            // every row sorts to the sentinel tail bucket. Matches the
            // tail-only output `CompareLevel` would produce in the same
            // state.
            std::fill(rankForRow.begin(), rankForRow.end(), SENTINEL);
        }
        else
        {
            const std::vector<LogLevel> &ranksRef = *ranks;
            tbb::parallel_for(
                tbb::blocked_range<size_t>(0, n),
                [&table, columnIndex, &logRows, &rankForRow, &ranksRef](const tbb::blocked_range<size_t> &range) {
                    for (size_t i = range.begin(); i != range.end(); ++i)
                    {
                        const auto id = table.GetEnumValueId(logRows[i], columnIndex);
                        if (!id.has_value() || static_cast<size_t>(*id) >= ranksRef.size())
                        {
                            rankForRow[i] = SENTINEL;
                            continue;
                        }
                        const LogLevel lvl = ranksRef[static_cast<size_t>(*id)];
                        // `Unknown` is the cache's "raw bytes did not
                        // map" marker; treat it like a missing slot.
                        rankForRow[i] = (lvl == LogLevel::Unknown) ? SENTINEL : static_cast<uint8_t>(lvl);
                    }
                }
            );
        }
        if (ascending)
        {
            tbb::parallel_sort(permutation.begin(), permutation.end(), [&rankForRow](size_t a, size_t b) {
                if (rankForRow[a] != rankForRow[b])
                {
                    return rankForRow[a] < rankForRow[b];
                }
                return a < b;
            });
        }
        else
        {
            tbb::parallel_sort(permutation.begin(), permutation.end(), [&rankForRow](size_t a, size_t b) {
                if (rankForRow[a] != rankForRow[b])
                {
                    return rankForRow[a] > rankForRow[b];
                }
                return a < b;
            });
        }
        return permutation;
    }

    // Generic path: dispatch through `CompareRows` per comparison.
    // Pays the slot-resolution cost on every call but is correct for
    // the non-enum types (`Time`, `Integer`, `Floating`, string),
    // where no cheap pre-materialisation is available. Still benefits
    // from parallel sort.
    if (ascending)
    {
        tbb::parallel_sort(
            permutation.begin(),
            permutation.end(),
            [&table, &logRows, columnIndex, rankForEnumColumn](size_t a, size_t b) {
                const int cmp = CompareRows(table, logRows[a], logRows[b], columnIndex, rankForEnumColumn);
                if (cmp != 0)
                {
                    return cmp < 0;
                }
                return a < b;
            }
        );
    }
    else
    {
        tbb::parallel_sort(
            permutation.begin(),
            permutation.end(),
            [&table, &logRows, columnIndex, rankForEnumColumn](size_t a, size_t b) {
                const int cmp = CompareRows(table, logRows[a], logRows[b], columnIndex, rankForEnumColumn);
                if (cmp != 0)
                {
                    return cmp > 0;
                }
                return a < b;
            }
        );
    }
    return permutation;
}

} // namespace loglib
