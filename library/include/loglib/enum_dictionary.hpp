#pragma once

#include "loglib/key_index.hpp"

#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace loglib
{

/// Dense per-column id assigned to each distinct string interned in an
/// `EnumDictionary`. Insertion-ordered, never reused. Stored inside a
/// `CompactTag::DictRef` payload as a 16-bit value (capacity headroom up
/// to `MAX_ENUM_VALUES`).
enum class EnumValueId : uint16_t
{
};

inline constexpr EnumValueId INVALID_ENUM_VALUE_ID{std::numeric_limits<uint16_t>::max()};

/// Compile-time hard ceiling on distinct values per column. Storage path
/// (`uint16_t` id field with `INVALID_ENUM_VALUE_ID = 0xFFFF` sentinel)
/// and the filter-UI value picker both rely on this staying well under
/// `1 << 16`. The *effective* per-column cap is runtime-configurable via
/// `internal::AdvancedParserOptions::enumValueCap` (default 64); the
/// constant below is the maximum that knob can be clamped to.
inline constexpr uint16_t MAX_ENUM_VALUES = 1024;

/// Default per-column distinct-value cap when no `AdvancedParserOptions`
/// override is supplied. Picked to comfortably cover real-world enum
/// columns (`level`, `host`, `service`, build IDs) without inflating the
/// filter picker's sorted list past the point of usability.
inline constexpr uint16_t DEFAULT_ENUM_VALUE_CAP = 64;

/// Per-column dictionary of distinct string values. Values are
/// insertion-ordered; their `EnumValueId`s are the index in that order
/// and remain stable for the dictionary's lifetime.
///
/// Single-writer / multiple-reader contract: mutators (`Insert`, `Clear`)
/// are called only from the `LogTable` thread that owns
/// `EnumDictionaryRegistry`; readers (`Find`, `Resolve`, `Values`,
/// `Size`) can race with concurrent reads but **not** with writers.
class EnumDictionary
{
public:
    EnumDictionary() = default;

    /// @p cap is the maximum number of distinct values this dictionary
    /// will accept; further `Insert` calls return `INVALID_ENUM_VALUE_ID`
    /// and `Full()` reports true. Clamped to `[1, MAX_ENUM_VALUES]`.
    explicit EnumDictionary(uint16_t cap) noexcept;

    EnumDictionary(const EnumDictionary &) = delete;
    EnumDictionary &operator=(const EnumDictionary &) = delete;

    EnumDictionary(EnumDictionary &&) noexcept = default;
    EnumDictionary &operator=(EnumDictionary &&) noexcept = default;

    /// Returns the existing id for @p bytes, or `INVALID_ENUM_VALUE_ID`
    /// if it is not interned.
    [[nodiscard]] EnumValueId Find(std::string_view bytes) const noexcept;

    /// Returns the id for @p bytes, allocating a new one if first-seen.
    /// Returns `INVALID_ENUM_VALUE_ID` if the dictionary is full
    /// (`Size() == MAX_ENUM_VALUES`) and @p bytes is not already present.
    /// Caller must check the return value before encoding a `DictRef`.
    [[nodiscard]] EnumValueId Insert(std::string_view bytes);

    /// Bytes for @p id, or empty when @p id is out of range.
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

    /// Snapshot for the filter UI (multi-select picker). View bytes are
    /// stable for the dictionary's lifetime.
    [[nodiscard]] std::span<const std::string> Values() const noexcept
    {
        return {mValues};
    }

    /// Drop all entries. Used by `LogTable::Reset` and on demotion.
    void Clear() noexcept;

private:
    /// Heterogeneous hasher so `mIndex.find(string_view)` does not have
    /// to construct a temporary `std::string`. The `equal_to<>`
    /// specialisation in the map type below pairs with this.
    struct StringHash
    {
        using is_transparent = void;
        [[nodiscard]] size_t operator()(std::string_view s) const noexcept
        {
            return std::hash<std::string_view>{}(s);
        }
        [[nodiscard]] size_t operator()(const std::string &s) const noexcept
        {
            return std::hash<std::string_view>{}(std::string_view(s));
        }
    };

    std::vector<std::string> mValues;
    std::unordered_map<std::string, EnumValueId, StringHash, std::equal_to<>> mIndex;
    /// Per-instance cap; stamped at construction by
    /// `EnumDictionaryRegistry::GetOrInsert` from
    /// `LogTable::mEnumValueCap`. Defaulted to `DEFAULT_ENUM_VALUE_CAP`
    /// so a default-constructed dictionary in tests still behaves.
    uint16_t mCap = DEFAULT_ENUM_VALUE_CAP;
};

/// `KeyId` -> `EnumDictionary` for every column currently configured as
/// `Type::enumeration`. Owned by `LogTable`; pointed at by every
/// `LineSource` in the same session via `LineSource::SetEnumDictionaries`
/// so that `CompactLogValue::Materialise(DictRef)` can resolve the bytes.
///
/// A single column may register multiple alias KeyIds (e.g. a column
/// whose `keys` are `["level", "severity"]`) onto the same logical
/// dictionary so that `GetValue` over any of those keys resolves through
/// the same canonical store. Aliases are registered with `Alias`.
class EnumDictionaryRegistry
{
public:
    EnumDictionaryRegistry() = default;

    EnumDictionaryRegistry(const EnumDictionaryRegistry &) = delete;
    EnumDictionaryRegistry &operator=(const EnumDictionaryRegistry &) = delete;

    EnumDictionaryRegistry(EnumDictionaryRegistry &&) noexcept = default;
    EnumDictionaryRegistry &operator=(EnumDictionaryRegistry &&) noexcept = default;

    /// True when @p key is one of an enum-encoded column's keys.
    [[nodiscard]] bool Contains(KeyId key) const noexcept;

    /// Read-only access. Returns nullptr if @p key is not registered.
    [[nodiscard]] const EnumDictionary *Find(KeyId key) const noexcept;

    /// Mutable access; inserts a fresh dictionary for @p key if absent.
    /// The returned reference is stable as long as the registry exists
    /// and `Erase`/`Clear` are not called; subsequent `GetOrInsert`
    /// calls for *different* keys do not invalidate it. @p cap is the
    /// per-column distinct-value cap stamped on the freshly-created
    /// dictionary; ignored when an entry for @p key already exists.
    [[nodiscard]] EnumDictionary &GetOrInsert(KeyId key, uint16_t cap = DEFAULT_ENUM_VALUE_CAP);

    /// Make @p alias resolve to the same dictionary as @p canonical.
    /// `canonical` must already have a dictionary (`GetOrInsert` it
    /// first). Idempotent: re-aliasing to the same canonical is a
    /// no-op; aliasing onto a different canonical replaces the prior
    /// alias entry.
    void Alias(KeyId canonical, KeyId alias);

    /// Drop the dictionary owned by @p canonical and every alias that
    /// resolves to it. Used on demotion.
    void Erase(KeyId canonical) noexcept;

    /// Wipe every dictionary (used by `LogTable::Reset`).
    void Clear() noexcept;

    [[nodiscard]] size_t CanonicalSize() const noexcept
    {
        return mDictionaries.size();
    }

    [[nodiscard]] bool Empty() const noexcept
    {
        return mDictionaries.empty();
    }

    /// Approximate heap bytes owned by the registry (for the
    /// memory-footprint benchmark; not part of the parse hot path).
    [[nodiscard]] size_t EstimatedMemoryBytes() const noexcept;

private:
    /// Canonical storage keyed by the column's first KeyId. Stored as
    /// `unique_ptr` so reference stability survives rehashes.
    std::unordered_map<KeyId, std::unique_ptr<EnumDictionary>> mDictionaries;
    /// Every KeyId (canonical and aliases) -> pointer into
    /// `mDictionaries`. Looked up by `Find` / `Contains`.
    std::unordered_map<KeyId, EnumDictionary *> mIndex;
};

} // namespace loglib
