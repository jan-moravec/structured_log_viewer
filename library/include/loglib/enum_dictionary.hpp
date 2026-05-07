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

/// Dense per-column id for a string interned in an `EnumDictionary`.
/// Insertion-ordered, never reused. Stored in a `CompactTag::DictRef`
/// payload as `uint16_t`.
enum class EnumValueId : uint16_t
{
};

inline constexpr EnumValueId INVALID_ENUM_VALUE_ID{std::numeric_limits<uint16_t>::max()};

/// Hard ceiling on distinct values per column. The runtime cap
/// (`AdvancedParserOptions::enumValueCap`, default 64) is clamped to
/// this.
inline constexpr uint16_t MAX_ENUM_VALUES = 1024;

/// Default per-column distinct-value cap.
inline constexpr uint16_t DEFAULT_ENUM_VALUE_CAP = 64;

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
    /// dictionary's lifetime.
    [[nodiscard]] std::span<const std::string> Values() const noexcept
    {
        return {mValues};
    }

    void Clear() noexcept;

private:
    /// Heterogeneous hasher so `string_view` lookups skip the temp
    /// `std::string`.
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
    /// must already have one. Idempotent.
    void Alias(KeyId canonical, KeyId alias);

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
    /// `unique_ptr` so dictionary refs survive rehashes.
    std::unordered_map<KeyId, std::unique_ptr<EnumDictionary>> mDictionaries;
    /// Canonical and alias keys -> pointer into `mDictionaries`.
    std::unordered_map<KeyId, EnumDictionary *> mIndex;
};

} // namespace loglib
