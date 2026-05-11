#pragma once

#include "loglib/enum_dictionary.hpp"
#include "loglib/internal/transparent_string_hash.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <variant>
#include <vector>

namespace loglib
{

class LogTable;

/// Multi-select equality predicate for `LogConfiguration::Type::Enumeration`
/// columns. Resolves selected strings against the column's dictionary at
/// construction time. The hot path is one `EnumValueId` lookup + a
/// `vector<bool>` test. Rows whose slot is encoded as a non-`DictRef`
/// (column not yet promoted) fall back to a transparent-hash string set.
///
/// The bitset reflects the dictionary state at construction. Callers
/// *should* rebuild the predicate when a selected value gains a new id
/// (typically signalled by `LogTable::EnumDictionaries()` growth via
/// the GUI's `enumColumnsChanged` connection). When every selected
/// value resolved at construction time (`mAllResolved`), an id past the
/// bitset is provably for an unselected value and the predicate
/// rejects directly. Otherwise the predicate falls through to the
/// `mSelectedStrings` set so a stale predicate still produces correct
/// matches against newly-interned selected values.
///
/// Threading: construction is single-threaded today. The GUI thread
/// is the sole reader/writer of the dictionary -- `LogTable::AppendBatch`
/// runs on the GUI thread (via the queued connection from
/// `QtStreamingLogSink`), and `LogFilterModel::SetFilterRules` is
/// only called from filter-submission slots that also live on the GUI
/// thread. The defensive past-bitset branch + `mAllResolved` accounting
/// keep the predicate correct even if a future caller resolves the
/// selection from a worker thread that races with dictionary growth;
/// they have no cost on the serial path.
class EnumRowPredicate
{
public:
    EnumRowPredicate(
        size_t columnIndex, std::span<const std::string_view> selectedValues, const EnumDictionary *dictionary
    );

    EnumRowPredicate(const EnumRowPredicate &) = delete;
    EnumRowPredicate &operator=(const EnumRowPredicate &) = delete;
    EnumRowPredicate(EnumRowPredicate &&) noexcept = default;
    EnumRowPredicate &operator=(EnumRowPredicate &&) noexcept = default;
    ~EnumRowPredicate() = default;

    [[nodiscard]] bool MatchesRow(const LogTable &table, size_t row) const;

    /// True iff the constructor managed to resolve at least one
    /// selected value to an id. Mirrors `EnumFilterRule::mFastPathArmed`.
    [[nodiscard]] bool IsFastPathArmed() const noexcept
    {
        return mFastPathArmed;
    }

private:
    size_t mColumnIndex = 0;
    /// Indexed by `EnumValueId`; size matches the dictionary at
    /// construction time. Empty when no dictionary was supplied.
    std::vector<bool> mSelectedIds;
    /// String-side fallback. Populated only for selected values that
    /// did *not* resolve to an id at construction (or when no
    /// dictionary was supplied) so the common "armed and fully
    /// resolved" path skips the `unordered_set` allocation + K hash
    /// inserts. The fallback covers two cases at match time:
    /// (a) the column isn't yet promoted (slot is not a `DictRef`),
    /// and (b) the predicate is stale and the id falls past the
    /// bitset for a value that *is* selected but wasn't resolved at
    /// construction (`mAllResolved == false`).
    std::unordered_set<std::string, internal::TransparentStringHash, internal::TransparentStringEqual> mSelectedStrings;
    bool mFastPathArmed = false;
    /// True iff every selected value resolved to an id at construction.
    /// Lets the past-bitset branch in `MatchesRow` short-circuit to
    /// `false` instead of paying for a string-set lookup.
    bool mAllResolved = false;
    /// True iff the constructor was given an empty selection. In that
    /// case every row is rejected by `MatchesRow` without consulting
    /// the bitset / string set. Exposed as a named sentinel rather
    /// than inferring it from `mSelectedIds.empty() && mSelectedStrings.empty()`
    /// so a future field addition cannot accidentally invalidate the
    /// inference.
    bool mEmptySelection = false;
};

/// Inclusive time-range predicate. `mBegin`/`mEnd` are microseconds
/// since the UNIX epoch (matching `loglib::TimeStamp::time_since_epoch()`).
/// An inverted range (`begin > end`) rejects every row.
class TimeRangeRowPredicate
{
public:
    TimeRangeRowPredicate(size_t columnIndex, int64_t begin, int64_t end);

    TimeRangeRowPredicate(const TimeRangeRowPredicate &) = default;
    TimeRangeRowPredicate &operator=(const TimeRangeRowPredicate &) = default;
    TimeRangeRowPredicate(TimeRangeRowPredicate &&) noexcept = default;
    TimeRangeRowPredicate &operator=(TimeRangeRowPredicate &&) noexcept = default;
    ~TimeRangeRowPredicate() = default;

    [[nodiscard]] bool MatchesRow(const LogTable &table, size_t row) const;

private:
    size_t mColumnIndex = 0;
    int64_t mBegin = 0;
    int64_t mEnd = 0;
};

/// String predicate that defers the actual match to a caller-supplied
/// callback. Lets the GUI keep Qt-flavoured regex / wildcard semantics
/// (e.g. capturing a `QRegularExpression` in the lambda) without lib
/// taking a Qt dependency.
///
/// Thread-safety: evaluated on the GUI thread by
/// `LogFilterModel::filterAcceptsRow`. The captured callback's
/// reentrancy is the caller's responsibility if it is reused across
/// threads in the future; `QRegularExpression` in particular JIT-
/// compiles its pattern lazily on the first `match()` call, so cross-
/// thread reuse requires pre-priming the regex (the GUI builder does
/// this once at construction time).
class CallbackStringRowPredicate
{
public:
    using MatchFn = std::function<bool(std::string_view)>;

    CallbackStringRowPredicate(size_t columnIndex, MatchFn match);

    CallbackStringRowPredicate(const CallbackStringRowPredicate &) = default;
    CallbackStringRowPredicate &operator=(const CallbackStringRowPredicate &) = default;
    CallbackStringRowPredicate(CallbackStringRowPredicate &&) noexcept = default;
    CallbackStringRowPredicate &operator=(CallbackStringRowPredicate &&) noexcept = default;
    ~CallbackStringRowPredicate() = default;

    [[nodiscard]] bool MatchesRow(const LogTable &table, size_t row) const;

private:
    size_t mColumnIndex = 0;
    MatchFn mMatch;
};

/// Closed-set union of every concrete row predicate. Lets the GUI
/// proxy store a `std::vector<RowPredicate>` directly (no heap
/// allocation per rule, no virtual dispatch on the per-row hot path)
/// while keeping each predicate a regular value type.
using RowPredicate = std::variant<EnumRowPredicate, TimeRangeRowPredicate, CallbackStringRowPredicate>;

/// Dispatch helper. Three predicates -> a `std::visit` resolves to a
/// direct call to the concrete `MatchesRow` member at compile time.
[[nodiscard]] inline bool MatchesRow(const RowPredicate &predicate, const LogTable &table, size_t row)
{
    return std::visit([&table, row](const auto &concrete) { return concrete.MatchesRow(table, row); }, predicate);
}

} // namespace loglib
