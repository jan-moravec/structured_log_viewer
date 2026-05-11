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
/// must rebuild the predicate when a *selected* value gains a new id
/// (typically signalled by `LogTable::EnumDictionaries()` growth and
/// the GUI's `enumColumnsChanged` connection). Out-of-range ids reach
/// the bitset miss branch and reject: by invariant, any id past the
/// bitset is for an unselected value.
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
    /// String-side fallback used when the column isn't yet promoted
    /// to enum (no `DictRef` slot to query). Transparent hashing lets
    /// the predicate look up a `std::string_view` without materialising
    /// a `std::string`.
    std::unordered_set<std::string, internal::TransparentStringHash, internal::TransparentStringEqual> mSelectedStrings;
    bool mFastPathArmed = false;
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
/// taking a Qt dependency. The callback runs on whatever thread the
/// row walker uses.
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
