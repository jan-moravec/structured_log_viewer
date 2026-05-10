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

/// Per-column id for a string interned in an `EnumDictionary`.
/// Insertion-ordered; stored in a `CompactTag::DictRef` payload.
enum class EnumValueId : uint16_t
{
};

inline constexpr EnumValueId INVALID_ENUM_VALUE_ID{std::numeric_limits<uint16_t>::max()};

/// Hard ceiling on distinct values per column.
inline constexpr uint16_t MAX_ENUM_VALUES = 1024;

/// Default per-column distinct-value cap.
inline constexpr uint16_t DEFAULT_ENUM_VALUE_CAP = 64;

/// Max value length to be considered enum-shaped; long values accrue
/// against the column's health budget rather than killing immediately.
inline constexpr uint32_t MAX_ENUM_CANDIDATE_LEN = 64;

/// Per-column dictionary of distinct string values. Insertion-ordered;
/// `EnumValueId`s are stable for the dictionary's lifetime.
/// Single writer (`LogTable` thread); readers may race with each other.
class EnumDictionary
{
public:
    EnumDictionary() = default;

    /// @p cap is clamped to `[1, MAX_ENUM_VALUES]`; over-cap inserts return
    /// `INVALID_ENUM_VALUE_ID` and `Full()` returns true.
    explicit EnumDictionary(uint16_t cap) noexcept;

    EnumDictionary(const EnumDictionary &) = delete;
    EnumDictionary &operator=(const EnumDictionary &) = delete;

    EnumDictionary(EnumDictionary &&) noexcept = default;
    EnumDictionary &operator=(EnumDictionary &&) noexcept = default;

    ~EnumDictionary() = default;

    /// Existing id for @p bytes, or `INVALID_ENUM_VALUE_ID`.
    [[nodiscard]] EnumValueId Find(std::string_view bytes) const noexcept;

    /// Id for @p bytes, allocating one on first sight. Returns
    /// `INVALID_ENUM_VALUE_ID` when full and @p bytes is new.
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

    /// Snapshot for the filter UI picker; bytes are stable for the
    /// dictionary's lifetime.
    [[nodiscard]] const std::deque<std::string> &Values() const noexcept
    {
        return mValues;
    }

    void Clear() noexcept;

private:
    /// `std::deque` keeps each string's address stable so the index can
    /// key on `string_view`s into them.
    std::deque<std::string> mValues;
    tsl::robin_map<std::string_view, EnumValueId, internal::TransparentStringHash, internal::TransparentStringEqual>
        mIndex;
    uint16_t mCap = DEFAULT_ENUM_VALUE_CAP;
};

/// `KeyId` -> `EnumDictionary` for every `Type::Enumeration` column.
/// Owned by `LogTable`; each `LineSource` points at it so
/// `CompactLogValue::Materialise(DictRef)` can resolve bytes. Multiple
/// alias keys can share one dictionary via `Alias`.
class EnumDictionaryRegistry
{
public:
    EnumDictionaryRegistry() = default;

    EnumDictionaryRegistry(const EnumDictionaryRegistry &) = delete;
    EnumDictionaryRegistry &operator=(const EnumDictionaryRegistry &) = delete;

    EnumDictionaryRegistry(EnumDictionaryRegistry &&) noexcept = default;
    EnumDictionaryRegistry &operator=(EnumDictionaryRegistry &&) noexcept = default;

    ~EnumDictionaryRegistry() = default;

    [[nodiscard]] bool Contains(KeyId key) const noexcept;

    [[nodiscard]] const EnumDictionary *Find(KeyId key) const noexcept;

    /// Returns the dictionary for @p key, creating one (with @p cap) if absent.
    /// The reference is stable until `Erase`/`Clear`.
    [[nodiscard]] EnumDictionary &GetOrInsert(KeyId key, uint16_t cap = DEFAULT_ENUM_VALUE_CAP);

    /// Map @p alias to @p canonical's dictionary. @p canonical must have a
    /// dictionary; @p alias must not already be canonical or aliased to a
    /// different dictionary. Idempotent; returns `false` on precondition
    /// violation without modifying state.
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
    /// `aliases` always includes the canonical key.
    struct DictionaryEntry
    {
        std::unique_ptr<EnumDictionary> dict;
        std::vector<KeyId> aliases;
    };

    /// `unique_ptr` so dict refs survive rehashes of `mDictionaries`.
    tsl::robin_map<KeyId, DictionaryEntry> mDictionaries;
    /// Canonical and alias keys -> pointer into `mDictionaries`.
    tsl::robin_map<KeyId, EnumDictionary *> mIndex;
};

} // namespace loglib
