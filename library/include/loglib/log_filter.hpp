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
#include <vector>

namespace loglib
{

class LogTable;

/// Abstract per-row predicate evaluated by the GUI proxy
/// (`LogFilterModel::filterAcceptsRow`) and by future off-thread row
/// walkers. Stateless apart from construction-time inputs so a single
/// instance can serve many rows without locking.
class RowPredicate
{
public:
    virtual ~RowPredicate() = default;

    RowPredicate(const RowPredicate &) = delete;
    RowPredicate &operator=(const RowPredicate &) = delete;
    RowPredicate(RowPredicate &&) = delete;
    RowPredicate &operator=(RowPredicate &&) = delete;

    [[nodiscard]] virtual bool MatchesRow(const LogTable &table, size_t row) const = 0;

protected:
    RowPredicate() = default;
};

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
class EnumRowPredicate : public RowPredicate
{
public:
    EnumRowPredicate(
        size_t columnIndex,
        std::span<const std::string_view> selectedValues,
        const EnumDictionary *dictionary
    );

    [[nodiscard]] bool MatchesRow(const LogTable &table, size_t row) const override;

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
};

/// Inclusive time-range predicate. `mBegin`/`mEnd` are microseconds
/// since the UNIX epoch (matching `loglib::TimeStamp::time_since_epoch()`).
class TimeRangeRowPredicate : public RowPredicate
{
public:
    TimeRangeRowPredicate(size_t columnIndex, int64_t begin, int64_t end);

    [[nodiscard]] bool MatchesRow(const LogTable &table, size_t row) const override;

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
/// Thread-safety: the callback runs on whatever thread the row walker
/// uses, and a single predicate can be evaluated concurrently from
/// multiple walkers. The callback itself is the caller's responsibility
/// to make re-entrant. In particular, Qt's `QRegularExpression` JIT-
/// compiles its pattern lazily on the first `match()` call and is only
/// thread-safe *after* that first compile; callers wiring a regex
/// across threads must pre-prime it (or wrap the call site in a mutex).
class CallbackStringRowPredicate : public RowPredicate
{
public:
    using MatchFn = std::function<bool(std::string_view)>;

    CallbackStringRowPredicate(size_t columnIndex, MatchFn match);

    [[nodiscard]] bool MatchesRow(const LogTable &table, size_t row) const override;

private:
    size_t mColumnIndex = 0;
    MatchFn mMatch;
};

} // namespace loglib
