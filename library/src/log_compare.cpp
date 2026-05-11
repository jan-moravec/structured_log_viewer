#include "loglib/log_compare.hpp"

#include "loglib/log_configuration.hpp"
#include "loglib/log_table.hpp"
#include "loglib/log_value.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <numeric>
#include <optional>
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
    // Single trivially-zeroed allocation: `resize` value-initialises
    // `uint16_t` slots (the standard library's loop), and the inverse
    // pass below writes every index, so the zero-fill cost is recouped.
    mIdToRank.resize(size);
    // Sort `[0, size)` by the bytes of `values[i]`. `order[k]` ends up
    // holding the EnumValueId of the k-th-smallest value, so the
    // inverse pass `rank[order[k]] = k` gives the per-id rank in O(N)
    // once `order` is sorted.
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
        // Newly-minted id past the last rebuild: sort after known values.
        return static_cast<uint16_t>(mIdToRank.size());
    }
    return mIdToRank[idx];
}

uint16_t EnumDictRank::DictSize() const noexcept
{
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

/// `double` three-way that pushes NaN to the high end so it sorts at
/// the bottom of ascending. NaN-vs-NaN is equal.
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

/// Materialise the row's slot as a `LogValue`. Convenience over the
/// dispatch loop below.
LogValue LoadValue(const LogTable &table, size_t row, size_t column)
{
    return table.GetValue(row, column);
}

/// Compare two slots that may be string, string_view, or numeric.
/// When both sides materialise as strings the bytes are compared
/// directly; otherwise both sides are formatted through the column's
/// `printFormat` and the formatted bytes are compared. The fallback
/// keeps mixed columns deterministic without taking a position on
/// "numeric < string" or vice versa.
///
/// Empty / monostate is handled by the caller; this routine never
/// observes a monostate slot.
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

    // Fall back: format both sides through the column's `printFormat`
    // so e.g. an integer column sorts numerically below mixed strings.
    const std::string lhsFormatted = table.GetFormattedValue(lhsRow, column);
    const std::string rhsFormatted = table.GetFormattedValue(rhsRow, column);
    return ThreeWay(std::string_view(lhsFormatted), std::string_view(rhsFormatted));
}

/// `monostate` order: monostate-vs-monostate equal; monostate-vs-other
/// returns +1 (monostate is greater = sorts later in ascending).
/// Returns `nullopt` when neither side is monostate.
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

/// Shared shape for "monostate first, extract to T, three-way the
/// extracted values, mixed extracted/unextracted route extracted <
/// unextracted". `Extract` returns `std::optional<T>` and `Compare` is
/// a three-way comparator on `T`. The per-comparator definitions below
/// reduce to one-liners.
template <class Extract, class Compare>
int CompareTyped(const LogValue &lhs, const LogValue &rhs, Extract extract, Compare cmp)
{
    if (const auto order = CompareMonostateOrder(lhs, rhs); order.has_value())
    {
        return *order;
    }
    const auto lhsX = extract(lhs);
    const auto rhsX = extract(rhs);
    if (lhsX.has_value() && rhsX.has_value())
    {
        return cmp(*lhsX, *rhsX);
    }
    if (lhsX.has_value())
    {
        // Extracted value sorts before "non-extractable" (e.g. a number
        // column with a stray string slot keeps the numeric rows together).
        return -1;
    }
    if (rhsX.has_value())
    {
        return 1;
    }
    return 0;
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
            // Clamp to int64_t range: oversized values sort to the top
            // of the signed range, which is the closest order-preserving
            // mapping.
            constexpr uint64_t MAX = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
            return *u > MAX ? std::numeric_limits<int64_t>::max() : static_cast<int64_t>(*u);
        }
        if (const auto *d = std::get_if<double>(&v); d != nullptr)
        {
            // NaN has no integer image; route it to the non-numeric tail
            // (`nullopt`) so it sorts after every integer-shaped slot.
            // `static_cast<int64_t>(NaN)` is UB; same for values outside
            // the signed range. Clamp finite out-of-range doubles to the
            // signed limits (order-preserving) and treat NaN as missing.
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
        return std::nullopt;
    };
    return CompareTyped(lhs, rhs, toMicros, [](int64_t a, int64_t b) { return ThreeWay(a, b); });
}

int CompareEnum(
    const LogTable &table, size_t lhsRow, size_t rhsRow, size_t column, const EnumDictRank *rank
)
{
    const auto lhsId = table.GetEnumValueId(lhsRow, column);
    const auto rhsId = table.GetEnumValueId(rhsRow, column);

    if (lhsId.has_value() && rhsId.has_value() && rank != nullptr)
    {
        return ThreeWay(rank->RankOf(*lhsId), rank->RankOf(*rhsId));
    }
    if (lhsId.has_value() && rhsId.has_value() && rank == nullptr)
    {
        // No rank table: fall back to byte-wise compare of resolved
        // strings via `GetValue`. Slow path, only hit in transitional
        // states (between promotion and the first rank rebuild).
        return CompareLogValuesBytewise(table, lhsRow, rhsRow, column);
    }

    // One or both sides aren't DictRef. Could be monostate or an
    // unpromoted slot (string / numeric). Defer to the generic
    // string compare so the result still respects monostate order.
    const LogValue lhs = LoadValue(table, lhsRow, column);
    const LogValue rhs = LoadValue(table, rhsRow, column);
    if (const auto order = CompareMonostateOrder(lhs, rhs); order.has_value())
    {
        return *order;
    }
    return CompareLogValuesBytewise(table, lhsRow, rhsRow, column);
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
    case LogConfiguration::Type::Integer:
        return CompareInteger(LoadValue(table, lhsRow, columnIndex), LoadValue(table, rhsRow, columnIndex));
    case LogConfiguration::Type::Floating:
    case LogConfiguration::Type::Number:
        return CompareFloating(LoadValue(table, lhsRow, columnIndex), LoadValue(table, rhsRow, columnIndex));
    case LogConfiguration::Type::Time:
        return CompareTime(LoadValue(table, lhsRow, columnIndex), LoadValue(table, rhsRow, columnIndex));
    case LogConfiguration::Type::Enumeration:
        return CompareEnum(table, lhsRow, rhsRow, columnIndex, rankForEnumColumn);
    case LogConfiguration::Type::String:
    case LogConfiguration::Type::Any:
    case LogConfiguration::Type::Unknown:
    default:
        return CompareString(table, lhsRow, rhsRow, columnIndex);
    }
}

} // namespace loglib
