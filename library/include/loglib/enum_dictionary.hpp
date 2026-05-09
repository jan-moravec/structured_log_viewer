#pragma once

#include "loglib/internal/transparent_string_hash.hpp"
#include "loglib/key_index.hpp"

#include <tsl/robin_map.h>

#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace loglib
{

/// Dense per-column id for a string interned in an `EnumDictionary`.
/// Insertion-ordered, never reused. Stored in a `CompactTag::DictRef`
/// payload as `uint16_t`.
enum class EnumValueId : uint16_t
{
};

inline constexpr EnumValueId INVALID_ENUM_VALUE_ID{std::numeric_limits<uint16_t>::max()};

/// Hard ceiling on distinct values per column. The runtime cap
/// (`DEFAULT_ENUM_VALUE_CAP`, default 64; overridable via the
/// `LogTable::SetEnumValueCap` test/tuning hook) is clamped to this.
inline constexpr uint16_t MAX_ENUM_VALUES = 1024;

/// Default per-column distinct-value cap.
inline constexpr uint16_t DEFAULT_ENUM_VALUE_CAP = 64;

/// Maximum byte length for a value to be considered enum-shaped.
/// Values longer than this accrue against the column's health
/// budget (`LogTable::EnumColumnHealth`); a single stray long line
/// no longer disqualifies the column, but exceeding the percentile
/// tolerance over a sufficient sample demotes it to `Type::string`.
///
/// Only enforced for *auto-discovered* `Type::any` columns and
/// auto-promoted `Type::enumeration` columns. Columns the user
/// explicitly pinned to `Type::enumeration` honour user intent and
/// store values of any length.
inline constexpr uint32_t MAX_ENUM_CANDIDATE_LEN = 64;

/// Per-column dictionary of distinct string values. Values are
/// insertion-ordered and their `EnumValueId`s are stable for the
/// dictionary's lifetime.
///
/// Single-writer / multiple-reader: mutators run on the `LogTable`
/// thread; readers may race with each other but not with writers.
class EnumDictionary
{
public:
    EnumDictionary() = default;

    /// @p cap is the maximum number of distinct values; clamped to
    /// `[1, MAX_ENUM_VALUES]`. Further `Insert`s return
    /// `INVALID_ENUM_VALUE_ID` and `Full()` reports true.
    explicit EnumDictionary(uint16_t cap) noexcept;

    EnumDictionary(const EnumDictionary &) = delete;
    EnumDictionary &operator=(const EnumDictionary &) = delete;

    EnumDictionary(EnumDictionary &&) noexcept = default;
    EnumDictionary &operator=(EnumDictionary &&) noexcept = default;

    /// Existing id for @p bytes, or `INVALID_ENUM_VALUE_ID`.
    [[nodiscard]] EnumValueId Find(std::string_view bytes) const noexcept;

    /// Id for @p bytes, allocating one on first sight. Returns
    /// `INVALID_ENUM_VALUE_ID` when the dictionary is full and @p bytes
    /// is new; callers must check before encoding a `DictRef`.
    [[nodiscard]] EnumValueId Insert(std::string_view bytes);

    /// Bytes for @p id, or empty if out of range.
    [[nodiscard]] std::string_view Resolve(EnumValueId id) const noexcept;

    [[nodiscard]] uint16_t Size() const noexcept
    {
        return static_cast<uint16_t>(mValues.size());
    }

    [[nodiscard]] bool Empty() const noexcept
    {
        return mValues.empty();
    }

    [[nodiscard]] bool Full() const noexcept
    {
        return mValues.size() >= mCap;
    }

    [[nodiscard]] uint16_t Cap() const noexcept
    {
        return mCap;
    }

    /// Snapshot for the filter UI picker. Bytes are stable for the
    /// dictionary's lifetime. `std::deque` instead of a contiguous
    /// vector so the `string_view` keys in `mIndex` stay valid as
    /// new values are appended (deque elements never move).
    [[nodiscard]] const std::deque<std::string> &Values() const noexcept
    {
        return mValues;
    }

    void Clear() noexcept;

private:
    /// `std::deque` instead of `std::vector` so each interned string's
    /// address is stable across `Insert`. The index below keys on
    /// `string_view` pointing into these stable bytes — switching to
    /// a vector would force a re-key on every reallocation. Costs
    /// one indirection on `Resolve(id)` (deque element access) for
    /// roughly 50% off the dictionary's heap footprint vs. the old
    /// `std::vector<std::string>` + `robin_map<std::string, ...>`
    /// pair (which kept two copies of every interned string).
    std::deque<std::string> mValues;
    // Phase 2.6: robin_map cuts the per-Insert hash lookup cost
    // (open addressing + smaller per-probe footprint) and brings the
    // dictionary's hot path closer to `KeyIndex`. Heterogeneous
    // lookup keeps `string_view` queries allocation-free; the keys
    // here point into `mValues`'s stable bytes (one source of truth
    // per interned value).
    tsl::robin_map<
        std::string_view,
        EnumValueId,
        internal::TransparentStringHash,
        internal::TransparentStringEqual>
        mIndex;
    uint16_t mCap = DEFAULT_ENUM_VALUE_CAP;
};

/// `KeyId` -> `EnumDictionary` for every column currently configured as
/// `Type::enumeration`. Owned by `LogTable` and pointed at by every
/// `LineSource` in the session so `CompactLogValue::Materialise(DictRef)`
/// can resolve bytes.
///
/// A column's alias keys (e.g. `["level", "severity"]`) all resolve to
/// the same dictionary via `Alias`.
class EnumDictionaryRegistry
{
public:
    EnumDictionaryRegistry() = default;

    EnumDictionaryRegistry(const EnumDictionaryRegistry &) = delete;
    EnumDictionaryRegistry &operator=(const EnumDictionaryRegistry &) = delete;

    EnumDictionaryRegistry(EnumDictionaryRegistry &&) noexcept = default;
    EnumDictionaryRegistry &operator=(EnumDictionaryRegistry &&) noexcept = default;

    [[nodiscard]] bool Contains(KeyId key) const noexcept;

    [[nodiscard]] const EnumDictionary *Find(KeyId key) const noexcept;

    /// Returns the dictionary for @p key, creating one (with @p cap) if
    /// absent. The reference is stable until `Erase`/`Clear`.
    [[nodiscard]] EnumDictionary &GetOrInsert(KeyId key, uint16_t cap = DEFAULT_ENUM_VALUE_CAP);

    /// Make @p alias resolve to @p canonical's dictionary. @p canonical
    /// must already have a dictionary; @p alias must not already be
    /// either canonical or an alias of a different dictionary.
    /// Idempotent: aliasing the same pair twice returns `true` both
    /// times. Returns `false` (without modifying state) when the
    /// preconditions are violated; previously this was a silent
    /// `stderr` warning that masked configuration bugs.
    [[nodiscard]] bool Alias(KeyId canonical, KeyId alias);

    /// Drop the dictionary at @p canonical and every alias to it.
    void Erase(KeyId canonical) noexcept;

    void Clear() noexcept;

    [[nodiscard]] size_t CanonicalSize() const noexcept
    {
        return mDictionaries.size();
    }

    [[nodiscard]] bool Empty() const noexcept
    {
        return mDictionaries.empty();
    }

    /// Approximate heap bytes (memory-footprint benchmark only).
    [[nodiscard]] size_t EstimatedMemoryBytes() const noexcept;

private:
    /// One entry per canonical key. `aliases` always contains at
    /// least the canonical key itself plus any keys aliased onto it,
    /// so `Erase` can drop only the affected `mIndex` entries
    /// instead of walking the whole index.
    struct DictionaryEntry
    {
        std::unique_ptr<EnumDictionary> dict;
        std::vector<KeyId> aliases;
    };

    // Phase 2.6: robin_map again — `Find`/`Contains` are called every
    // `LogLine::GetValue(KeyId)` on a `DictRef` slot, so the hot
    // lookup matters.
    /// `unique_ptr` (inside the entry) so dictionary refs survive
    /// `mDictionaries` rehashes.
    tsl::robin_map<KeyId, DictionaryEntry> mDictionaries;
    /// Canonical and alias keys -> pointer into `mDictionaries`.
    tsl::robin_map<KeyId, EnumDictionary *> mIndex;
};

} // namespace loglib
