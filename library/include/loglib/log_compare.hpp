#pragma once

#include "loglib/enum_dictionary.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace loglib
{

class LogTable;

/// Precomputed alphabetic-rank table over an `EnumDictionary`. Built
/// once when the GUI's `LogFilterModel` learns the column is an enum;
/// rebuilt on `enumColumnsChanged` ticks that grow the dictionary so
/// the rank covers every interned id.
///
/// The hot path (`CompareRows`) consults `RankOf(id)` to produce a
/// stable, alphabetised ordering without per-comparison string compares
/// over the resolved bytes. Byte-wise (case-sensitive) comparison is
/// used internally; the GUI picker keeps its locale-aware ordering for
/// display, but the table sort is intentionally byte-deterministic so
/// the result is reproducible across locales.
///
/// Indexing past `DictSize()` returns `DictSize()` so newly-minted ids
/// (i.e. dictionary growth between rank rebuilds) sort after every
/// known value rather than triggering an out-of-bounds.
class EnumDictRank
{
public:
    EnumDictRank() = default;

    /// Build the rank table from @p dictionary's current values. Safe
    /// to call repeatedly; the rank reflects the dictionary at this
    /// instant.
    explicit EnumDictRank(const EnumDictionary &dictionary);

    [[nodiscard]] uint16_t RankOf(EnumValueId id) const noexcept;

    [[nodiscard]] uint16_t DictSize() const noexcept;

    [[nodiscard]] bool Empty() const noexcept
    {
        return mIdToRank.empty();
    }

private:
    /// Indexed by `EnumValueId`. `mIdToRank[i]` is the alphabetic rank
    /// of the i-th interned value at construction time.
    std::vector<uint16_t> mIdToRank;
};

/// Three-way row comparator over a single column.
///   < 0 if row a is less than row b
///   = 0 if equal
///   > 0 if greater
///
/// Dispatches on the column's `LogConfiguration::Type`:
///   - `Integer`     - signed compare on the underlying int64_t.
///     uint64_t slots compare via `<` on themselves but treat ints
///     non-negative; negative ints are routed via int64_t compare.
///   - `Floating` / `Number` - double compare; NaN sinks to the end.
///   - `Time`        - int64_t microsecond-since-epoch compare.
///   - `Enumeration` - `EnumDictRank` lookup, then uint16_t compare.
///     Falls back to byte-wise compare of resolved strings if
///     @p rankForEnumColumn is null (e.g. transitional state during a
///     dict rebuild).
///   - `String` / `Any` / `Unknown` - byte-wise compare of
///     `std::string_view`s materialised via `LogLine::PeekStringView`
///     or `LogTable::GetFormattedValue` for non-string slots.
///
/// `std::monostate` slots sort after every populated slot in an
/// ascending sort: monostate-vs-monostate is equal, monostate-vs-anything
/// is strictly greater. Callers wanting "monostate first" can negate
/// the return.
[[nodiscard]] int CompareRows(
    const LogTable &table,
    size_t lhsRow,
    size_t rhsRow,
    size_t columnIndex,
    const EnumDictRank *rankForEnumColumn = nullptr
);

} // namespace loglib
