// `loglib` has no Qt in its include chain, so the TBB / Qt `emit`
// collision that bites `app/` doesn't apply -- normal include order
// is fine.
#include "loglib/query/log_compare.hpp"

#include "loglib/core/compact_log_value.hpp"
#include "loglib/core/log_level.hpp"
#include "loglib/core/log_value.hpp"
#include "loglib/schema/log_configuration.hpp"
#include "loglib/table/log_table.hpp"

#include <oneapi/tbb/blocked_range.h>
#include <oneapi/tbb/enumerable_thread_specific.h>
#include <oneapi/tbb/parallel_for.h>
#include <oneapi/tbb/parallel_sort.h>

#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
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
/// `ColumnType` pins which slot variants a column holds,
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

/// How a cell takes part in a key sort. Cells that fit the column's type come first, then NaN in a floating column,
/// then every other cell. Descending order reverses the three groups.
enum class KeyClass : uint8_t
{
    Ordinary,
    NotANumber,
    Missing,
};

/// A cell reduced to a sort key. The key only means something for an ordinary cell.
struct SortKey
{
    KeyClass keyClass = KeyClass::Missing;
    uint64_t key = 0;
};

constexpr uint64_t SIGN_BIT = uint64_t{1} << 63;

/// Maps a signed integer to an unsigned key with the same order.
uint64_t OrderedKey(int64_t value) noexcept
{
    return static_cast<uint64_t>(value) ^ SIGN_BIT;
}

/// Maps a double that is not NaN to an unsigned key with the same order. Zero and negative zero are equal for
/// ThreeWayDouble, so both map to one key.
uint64_t OrderedKey(double value) noexcept
{
    if (value == 0.0)
    {
        value = 0.0;
    }
    const auto bits = std::bit_cast<uint64_t>(value);
    return (bits & SIGN_BIT) != 0 ? ~bits : (bits | SIGN_BIT);
}

SortKey MakeKey(uint64_t key) noexcept
{
    return {.keyClass = KeyClass::Ordinary, .key = key};
}

/// Clamps an unsigned value above INT64_MAX, as the integer and time order do.
int64_t ClampToInt64(uint64_t value) noexcept
{
    constexpr auto MAX = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    return value > MAX ? std::numeric_limits<int64_t>::max() : static_cast<int64_t>(value);
}

/// A boolean column holds booleans, and any other cell is missing.
SortKey BoolKey(const CompactLogValue *cell) noexcept
{
    if (cell != nullptr && cell->tag == CompactTag::Bool)
    {
        return MakeKey(cell->payload != 0 ? 1U : 0U);
    }
    return {};
}

/// An integer column holds integers. A double is truncated and clamped to the int64 range, and NaN is missing.
SortKey IntegerKey(const CompactLogValue *cell) noexcept
{
    if (cell == nullptr)
    {
        return {};
    }
    switch (cell->tag)
    {
    case CompactTag::Int64:
        return MakeKey(OrderedKey(static_cast<int64_t>(cell->payload)));
    case CompactTag::Uint64:
        return MakeKey(OrderedKey(ClampToInt64(cell->payload)));
    case CompactTag::Double:
    {
        const auto number = std::bit_cast<double>(cell->payload);
        if (std::isnan(number))
        {
            return {};
        }
        if (number >= static_cast<double>(std::numeric_limits<int64_t>::max()))
        {
            return MakeKey(OrderedKey(std::numeric_limits<int64_t>::max()));
        }
        if (number <= static_cast<double>(std::numeric_limits<int64_t>::min()))
        {
            return MakeKey(OrderedKey(std::numeric_limits<int64_t>::min()));
        }
        return MakeKey(OrderedKey(static_cast<int64_t>(number)));
    }
    default:
        return {};
    }
}

/// A floating column holds doubles and integers, compared as doubles. NaN sorts after every number.
SortKey FloatingKey(const CompactLogValue *cell) noexcept
{
    if (cell == nullptr)
    {
        return {};
    }
    double number = 0.0;
    switch (cell->tag)
    {
    case CompactTag::Double:
        number = std::bit_cast<double>(cell->payload);
        break;
    case CompactTag::Int64:
        number = static_cast<double>(static_cast<int64_t>(cell->payload));
        break;
    case CompactTag::Uint64:
        number = static_cast<double>(cell->payload);
        break;
    default:
        return {};
    }
    if (std::isnan(number))
    {
        return {.keyClass = KeyClass::NotANumber, .key = 0};
    }
    return MakeKey(OrderedKey(number));
}

/// A time column holds timestamps and integers, as microseconds since the epoch.
SortKey TimeKey(const CompactLogValue *cell) noexcept
{
    if (cell == nullptr)
    {
        return {};
    }
    switch (cell->tag)
    {
    case CompactTag::Timestamp:
    case CompactTag::Int64:
        return MakeKey(OrderedKey(static_cast<int64_t>(cell->payload)));
    case CompactTag::Uint64:
        return MakeKey(OrderedKey(ClampToInt64(cell->payload)));
    default:
        return {};
    }
}

/// Orders two keys: by class, then by key within the ordinary class. Cells of the other two classes are equal.
int CompareKeys(const SortKey &lhs, const SortKey &rhs) noexcept
{
    if (lhs.keyClass != rhs.keyClass)
    {
        return lhs.keyClass < rhs.keyClass ? -1 : 1;
    }
    return lhs.keyClass == KeyClass::Ordinary ? ThreeWay(lhs.key, rhs.key) : 0;
}

template <SortKey (*KeyOf)(const CompactLogValue *)>
int CompareByKey(const LogTable &table, size_t lhsRow, size_t rhsRow, size_t column)
{
    return CompareKeys(KeyOf(table.FindCell(lhsRow, column)), KeyOf(table.FindCell(rhsRow, column)));
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

/// Compare `Type::Level` rows by canonical `LogLevel` ordinal (Trace
/// < Debug < ... < Fatal). Unresolved slots (monostate, unmapped
/// string, missing cache) join the tail bucket, matching `CompareEnum`.
int CompareLevel(const LogTable &table, size_t lhsRow, size_t rhsRow, size_t column)
{
    // One lookup of the rank cache serves both rows. The lookup hashes the column's key, so it is the costly part.
    const std::vector<LogLevel> *ranks = table.LevelRankCache(column);
    const auto levelOf = [&](size_t row) -> std::optional<LogLevel> {
        if (ranks == nullptr)
        {
            return std::nullopt;
        }
        const auto id = table.GetEnumValueId(row, column);
        if (!id.has_value() || static_cast<size_t>(*id) >= ranks->size())
        {
            return std::nullopt;
        }
        const LogLevel level = (*ranks)[static_cast<size_t>(*id)];
        return level == LogLevel::Unknown ? std::nullopt : std::optional<LogLevel>{level};
    };
    const auto lhsLevel = levelOf(lhsRow);
    const auto rhsLevel = levelOf(rhsRow);
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
    const ColumnType type = columns[columnIndex].type;

    switch (type)
    {
    case ColumnType::Boolean:
        return CompareByKey<BoolKey>(table, lhsRow, rhsRow, columnIndex);
    case ColumnType::Integer:
        return CompareByKey<IntegerKey>(table, lhsRow, rhsRow, columnIndex);
    case ColumnType::Floating:
    case ColumnType::Number:
        return CompareByKey<FloatingKey>(table, lhsRow, rhsRow, columnIndex);
    case ColumnType::Time:
        return CompareByKey<TimeKey>(table, lhsRow, rhsRow, columnIndex);
    case ColumnType::Enumeration:
        return CompareEnum(table, lhsRow, rhsRow, columnIndex, rankForEnumColumn);
    case ColumnType::Level:
        return CompareLevel(table, lhsRow, rhsRow, columnIndex);
    case ColumnType::String:
    case ColumnType::Any:
    default:
        return CompareString(table, lhsRow, rhsRow, columnIndex);
    }
}

namespace
{

/// A row position with its key. Sorting these moves 16 bytes per row and never touches the table.
struct KeyedRow
{
    uint64_t key;
    size_t position;
};

/// The rows split by key class. Every list is in row order.
struct KeyedRows
{
    std::vector<KeyedRow> ordinary;
    std::vector<size_t> notANumber;
    std::vector<size_t> missing;
};

KeyedRows SplitByClass(const std::vector<KeyClass> &classes, const std::vector<uint64_t> &keys)
{
    KeyedRows split;
    size_t ordinaryCount = 0;
    for (const KeyClass keyClass : classes)
    {
        ordinaryCount += keyClass == KeyClass::Ordinary ? 1U : 0U;
    }
    split.ordinary.reserve(ordinaryCount);
    for (size_t i = 0; i < classes.size(); ++i)
    {
        switch (classes[i])
        {
        case KeyClass::Ordinary:
            split.ordinary.push_back({.key = keys[i], .position = i});
            break;
        case KeyClass::NotANumber:
            split.notANumber.push_back(i);
            break;
        case KeyClass::Missing:
            split.missing.push_back(i);
            break;
        }
    }
    return split;
}

/// The strict order of keyed rows: by key, then by @p tie for equal keys, which returns their order by the rest of the
/// cell or zero when the key is the whole cell, then by position.
template <bool Ascending, typename Tie> auto KeyedRowLess(const Tie &tie)
{
    return [&tie](const KeyedRow &a, const KeyedRow &b) {
        if (a.key != b.key)
        {
            return Ascending ? a.key < b.key : a.key > b.key;
        }
        const int cmp = tie(a.position, b.position);
        if (cmp != 0)
        {
            return Ascending ? cmp < 0 : cmp > 0;
        }
        return a.position < b.position;
    };
}

/// Puts the part [begin, end) of the sorted order of the keyed rows in place, and leaves the rest in no order. The
/// whole range is sorted in parallel. A smaller part is selected with nth_element and only that part is sorted.
template <bool Ascending, typename Tie>
void SortKeyedRows(std::vector<KeyedRow> &rows, const Tie &tie, size_t begin, size_t end)
{
    const auto less = KeyedRowLess<Ascending>(tie);
    if (begin == 0 && end == rows.size())
    {
        tbb::parallel_sort(rows.begin(), rows.end(), less);
        return;
    }
    const auto first = rows.begin();
    if (end < rows.size())
    {
        std::nth_element(first, first + static_cast<std::ptrdiff_t>(end), rows.end(), less);
    }
    if (begin > 0)
    {
        std::nth_element(first, first + static_cast<std::ptrdiff_t>(begin), first + static_cast<std::ptrdiff_t>(end), less);
    }
    std::sort(first + static_cast<std::ptrdiff_t>(begin), first + static_cast<std::ptrdiff_t>(end), less);
}

/// Puts the part [begin, end) of the sorted order of the keyed rows in place. They arrive in row order, so rows with
/// equal keys are in position order. With @p detectOrder, a key sequence that is already in order is left alone and one
/// that is exactly reversed, with no equal keys, is reversed. Neither needs a sort.
template <typename Tie>
void OrderKeyedRows(
    std::vector<KeyedRow> &rows, bool ascending, bool detectOrder, const Tie &tie, size_t begin, size_t end
)
{
    if (begin >= end)
    {
        return;
    }
    if (detectOrder && !rows.empty())
    {
        bool nonDecreasing = true;
        bool nonIncreasing = true;
        bool strictlyIncreasing = true;
        bool strictlyDecreasing = true;
        for (size_t i = 1; i < rows.size(); ++i)
        {
            const uint64_t before = rows[i - 1].key;
            const uint64_t after = rows[i].key;
            nonDecreasing = nonDecreasing && before <= after;
            nonIncreasing = nonIncreasing && before >= after;
            strictlyIncreasing = strictlyIncreasing && before < after;
            strictlyDecreasing = strictlyDecreasing && before > after;
        }
        if (ascending ? nonDecreasing : nonIncreasing)
        {
            return;
        }
        if (ascending ? strictlyDecreasing : strictlyIncreasing)
        {
            std::ranges::reverse(rows);
            return;
        }
    }
    if (ascending)
    {
        SortKeyedRows<true>(rows, tie, begin, end);
    }
    else
    {
        SortKeyedRows<false>(rows, tie, begin, end);
    }
}

/// A part [begin, end) of the permutation, as positions in the order of the groups: ordinary rows, then NaN rows, then
/// the rest. Descending reverses the three groups, and the rows inside the two small groups stay in row order.
struct PermutationWindow
{
    size_t begin = 0;
    size_t end = 0;
};

/// The part of the ordinary rows that the window of the permutation covers.
PermutationWindow OrdinaryWindow(const KeyedRows &split, bool ascending, PermutationWindow window)
{
    const size_t first = ascending ? 0 : split.missing.size() + split.notANumber.size();
    const size_t count = split.ordinary.size();
    const auto clamp = [&](size_t position) { return position <= first ? size_t{0} : std::min(position - first, count); };
    return {.begin = clamp(window.begin), .end = clamp(window.end)};
}

/// Writes the part of the permutation in @p window. The ordinary rows must be ordered inside the part that
/// OrdinaryWindow gives.
std::vector<size_t> AssemblePermutation(const KeyedRows &split, bool ascending, PermutationWindow window)
{
    std::vector<size_t> permutation;
    permutation.reserve(window.end - window.begin);
    size_t groupStart = 0;
    const auto appendSlice = [&](size_t groupSize, auto element) {
        const size_t from = window.begin > groupStart ? window.begin - groupStart : 0;
        const size_t to = window.end > groupStart ? std::min(window.end - groupStart, groupSize) : 0;
        for (size_t i = from; i < to; ++i)
        {
            permutation.push_back(element(i));
        }
        groupStart += groupSize;
    };
    const auto ordinary = [&] {
        appendSlice(split.ordinary.size(), [&](size_t i) { return split.ordinary[i].position; });
    };
    const auto notANumber = [&] {
        appendSlice(split.notANumber.size(), [&](size_t i) { return split.notANumber[i]; });
    };
    const auto missing = [&] { appendSlice(split.missing.size(), [&](size_t i) { return split.missing[i]; }); };
    if (ascending)
    {
        ordinary();
        notANumber();
        missing();
    }
    else
    {
        missing();
        notANumber();
        ordinary();
    }
    return permutation;
}

/// Sorts by a column whose cells reduce to a 64-bit key. One parallel pass reads each cell once, and the sort then
/// compares integers without touching the table.
template <typename KeyOf>
std::vector<size_t> SortByNumericKeys(
    const LogTable &table,
    std::span<const size_t> logRows,
    size_t columnIndex,
    bool ascending,
    PermutationWindow window,
    KeyOf keyOf
)
{
    const size_t n = logRows.size();
    std::vector<KeyClass> classes(n);
    std::vector<uint64_t> keys(n);
    tbb::parallel_for(tbb::blocked_range<size_t>(0, n), [&](const tbb::blocked_range<size_t> &range) {
        for (size_t i = range.begin(); i != range.end(); ++i)
        {
            const SortKey key = keyOf(table.FindCell(logRows[i], columnIndex));
            classes[i] = key.keyClass;
            keys[i] = key.key;
        }
    });
    KeyedRows split = SplitByClass(classes, keys);
    const PermutationWindow ordinary = OrdinaryWindow(split, ascending, window);
    OrderKeyedRows(split.ordinary, ascending, true, [](size_t, size_t) { return 0; }, ordinary.begin, ordinary.end);
    return AssemblePermutation(split, ascending, window);
}

/// The first eight bytes of the text as a big-endian integer, padded with zero bytes. Comparing two prefixes as integers
/// orders texts like an unsigned byte comparison, as far as the prefixes differ.
uint64_t PrefixKey(std::string_view text) noexcept
{
    uint64_t key = 0;
    for (size_t i = 0; i < sizeof(uint64_t); ++i)
    {
        key = (key << 8U) | (i < text.size() ? static_cast<uint8_t>(text[i]) : uint64_t{0});
    }
    return key;
}

/// Sorts by a string column. One parallel pass resolves each cell to text, and a second computes an eight-byte key from
/// the text after the start that all texts share. The sort compares the keys as integers and reads the rest of the text
/// only for rows with equal keys. A cell that is not a
/// string is compared by its formatted text, and an empty cell sorts to the tail.
std::vector<size_t> SortByStringKeys(
    const LogTable &table, std::span<const size_t> logRows, size_t columnIndex, bool ascending, PermutationWindow window
)
{
    const size_t n = logRows.size();
    std::vector<KeyClass> classes(n);
    std::vector<uint64_t> keys(n);
    std::vector<std::string_view> texts(n);
    // Text that is not a view into the table lives here, one deque per worker so the views stay valid.
    tbb::enumerable_thread_specific<std::deque<std::string>> owned;
    tbb::parallel_for(tbb::blocked_range<size_t>(0, n), [&](const tbb::blocked_range<size_t> &range) {
        std::deque<std::string> &store = owned.local();
        for (size_t i = range.begin(); i != range.end(); ++i)
        {
            LogValue value = LoadValue(table, logRows[i], columnIndex);
            std::string_view text;
            if (std::holds_alternative<std::monostate>(value))
            {
                classes[i] = KeyClass::Missing;
                continue;
            }
            if (const auto *view = std::get_if<std::string_view>(&value); view != nullptr)
            {
                text = *view;
            }
            else if (auto *str = std::get_if<std::string>(&value); str != nullptr)
            {
                text = store.emplace_back(std::move(*str));
            }
            else
            {
                text = store.emplace_back(table.GetFormattedValue(logRows[i], columnIndex));
            }
            classes[i] = KeyClass::Ordinary;
            texts[i] = text;
        }
    });

    // Bytes that every text starts with tell the rows apart not at all, so the keys start after them. Log paths and
    // messages often share a long start.
    size_t shared = std::numeric_limits<size_t>::max();
    std::string_view first;
    bool haveFirst = false;
    for (size_t i = 0; i < n && shared > 0; ++i)
    {
        if (classes[i] != KeyClass::Ordinary)
        {
            continue;
        }
        if (!haveFirst)
        {
            first = texts[i];
            shared = first.size();
            haveFirst = true;
            continue;
        }
        const std::string_view text = texts[i];
        const size_t limit = std::min(shared, text.size());
        size_t same = 0;
        while (same < limit && first[same] == text[same])
        {
            ++same;
        }
        shared = same;
    }
    if (!haveFirst)
    {
        shared = 0;
    }
    tbb::parallel_for(tbb::blocked_range<size_t>(0, n), [&](const tbb::blocked_range<size_t> &range) {
        for (size_t i = range.begin(); i != range.end(); ++i)
        {
            if (classes[i] == KeyClass::Ordinary)
            {
                keys[i] = PrefixKey(texts[i].substr(shared));
            }
        }
    });
    KeyedRows split = SplitByClass(classes, keys);
    const PermutationWindow ordinary = OrdinaryWindow(split, ascending, window);
    OrderKeyedRows(
        split.ordinary,
        ascending,
        false,
        [&texts](size_t a, size_t b) { return texts[a].compare(texts[b]); },
        ordinary.begin,
        ordinary.end
    );
    return AssemblePermutation(split, ascending, window);
}

/// Sorts @p permutation by the three-way @p compare, breaking ties by input index so the order is total. The
/// direction is a template parameter so each instantiation compiles to a plain comparison.
template <bool Ascending, typename ThreeWay>
void SortWithTieBreak(std::vector<size_t> &permutation, const ThreeWay &compare)
{
    tbb::parallel_sort(permutation.begin(), permutation.end(), [&compare](size_t a, size_t b) {
        const int cmp = compare(a, b);
        if (cmp != 0)
        {
            return Ascending ? cmp < 0 : cmp > 0;
        }
        return a < b;
    });
}

/// Orders @p permutation by the precomputed per-row @p ranks with a counting sort: one pass to count each rank, one to
/// place each row. Rows with equal ranks stay in input order in both directions, so the order is the same as a
/// comparison sort with an input-index tie-break.
template <typename Rank>
void SortByRanks(std::vector<size_t> &permutation, const std::vector<Rank> &ranks, bool ascending)
{
    if (ranks.empty())
    {
        return;
    }
    const size_t bucketCount = static_cast<size_t>(*std::ranges::max_element(ranks)) + 1;
    std::vector<size_t> next(bucketCount, 0);
    for (const Rank rank : ranks)
    {
        ++next[rank];
    }
    // Turn the counts into the first output position of each rank. Descending hands out the highest rank first.
    size_t position = 0;
    for (size_t i = 0; i < bucketCount; ++i)
    {
        const size_t bucket = ascending ? i : bucketCount - 1 - i;
        const size_t count = next[bucket];
        next[bucket] = position;
        position += count;
    }
    for (size_t row = 0; row < ranks.size(); ++row)
    {
        permutation[next[ranks[row]]++] = row;
    }
}

/// Sorts the part @p window of the order by prepared keys, for the column types that have them. Returns nothing for the
/// other types.
std::optional<std::vector<size_t>> SortByKeys(
    const LogTable &table,
    std::span<const size_t> logRows,
    size_t columnIndex,
    bool ascending,
    PermutationWindow window
)
{
    const auto &columns = table.Configuration().Configuration().columns;
    if (columnIndex >= columns.size())
    {
        return std::nullopt;
    }
    switch (columns[columnIndex].type)
    {
    case ColumnType::Boolean:
        return SortByNumericKeys(table, logRows, columnIndex, ascending, window, BoolKey);
    case ColumnType::Integer:
        return SortByNumericKeys(table, logRows, columnIndex, ascending, window, IntegerKey);
    case ColumnType::Floating:
    case ColumnType::Number:
        return SortByNumericKeys(table, logRows, columnIndex, ascending, window, FloatingKey);
    case ColumnType::Time:
        return SortByNumericKeys(table, logRows, columnIndex, ascending, window, TimeKey);
    case ColumnType::String:
    case ColumnType::Any:
        return SortByStringKeys(table, logRows, columnIndex, ascending, window);
    default:
        return std::nullopt;
    }
}

} // namespace

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
    const bool isEnum = columnInRange && columns[columnIndex].type == ColumnType::Enumeration;
    const bool isLevel = columnInRange && columns[columnIndex].type == ColumnType::Level;

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

        // The input-index tie-break gives a strict total order, so the
        // unstable parallel sort behaves like a stable sort for callers
        // that rely on insertion-order ties. Descending inverts only the
        // primary order; ties still resolve to source-row ascending.
        SortByRanks(permutation, rankForRow, ascending);
        return permutation;
    }

    // Level fast path: pre-materialise canonical `LogLevel` ordinals
    // per row in parallel. Unresolved slots get a sentinel rank so
    // they sort to the tail, matching `CompareLevel`. The rank cache
    // is hoisted once so the loop body is a plain `ranks[id]` lookup.
    if (isLevel)
    {
        constexpr auto SENTINEL = static_cast<uint8_t>(std::numeric_limits<uint8_t>::max());
        std::vector<uint8_t> rankForRow(n);
        const std::vector<LogLevel> *ranks = table.LevelRankCache(columnIndex);
        if (ranks == nullptr)
        {
            // `Type::Level` column with no observations yet (e.g. saved
            // config loaded before any batches). Sort every row to the
            // tail bucket, matching `CompareLevel`.
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
                        // `Unknown` marks "raw bytes did not map"; treat as missing.
                        rankForRow[i] = (lvl == LogLevel::Unknown) ? SENTINEL : static_cast<uint8_t>(lvl);
                    }
                }
            );
        }
        SortByRanks(permutation, rankForRow, ascending);
        return permutation;
    }

    // Number, boolean, time and string columns: reduce each cell to a key once, then sort the keys.
    if (auto sorted = SortByKeys(table, logRows, columnIndex, ascending, {.begin = 0, .end = n}); sorted.has_value())
    {
        return std::move(*sorted);
    }

    // Generic path: dispatch through `CompareRows` per comparison.
    // Pays the slot-resolution cost on every call but is correct for
    // the non-enum types (`Time`, `Integer`, `Floating`, string),
    // where no cheap pre-materialisation is available. Still benefits
    // from parallel sort.
    const auto compare = [&table, &logRows, columnIndex, rankForEnumColumn](size_t a, size_t b) {
        return CompareRows(table, logRows[a], logRows[b], columnIndex, rankForEnumColumn);
    };
    if (ascending)
    {
        SortWithTieBreak<true>(permutation, compare);
    }
    else
    {
        SortWithTieBreak<false>(permutation, compare);
    }
    return permutation;
}

std::vector<size_t> SortPermutationWindowByColumn(
    const LogTable &table,
    std::span<const size_t> logRows,
    size_t columnIndex,
    bool ascending,
    size_t begin,
    size_t end,
    const EnumDictRank *rankForEnumColumn
)
{
    const size_t n = logRows.size();
    end = std::min(end, n);
    begin = std::min(begin, end);
    if (begin == 0 && end == n)
    {
        return SortPermutationByColumn(table, logRows, columnIndex, ascending, rankForEnumColumn);
    }
    if (n > 1)
    {
        if (auto sorted = SortByKeys(table, logRows, columnIndex, ascending, {.begin = begin, .end = end});
            sorted.has_value())
        {
            return std::move(*sorted);
        }
    }
    // The other column types sort in linear time or compare rows one by one, so the whole order is built and cut.
    const std::vector<size_t> full = SortPermutationByColumn(table, logRows, columnIndex, ascending, rankForEnumColumn);
    return {full.begin() + static_cast<std::ptrdiff_t>(begin), full.begin() + static_cast<std::ptrdiff_t>(end)};
}

} // namespace loglib
