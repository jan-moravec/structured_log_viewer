/**
 * @brief Dictionaries that replace repeated strings with small ids.
 *
 * Columns such as level hold few distinct values. An EnumDictionary interns each distinct string once and hands out an
 * id, and the row stores only that id. EnumDictionaryRegistry keeps one dictionary per column, and a LineSource points
 * at it so the ids can be turned back into text.
 *
 * This is part of the core module.
 */
#pragma once

#include "loglib/core/key_index.hpp"
#include "loglib/core/transparent_string_hash.hpp"

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

/** @brief Id of a string in an EnumDictionary. Ids count up in insertion order and are stored in DictRef cells. */
enum class EnumValueId : uint16_t
{
};

inline constexpr EnumValueId INVALID_ENUM_VALUE_ID{std::numeric_limits<uint16_t>::max()};

/** @brief Largest number of distinct values one column's dictionary may hold. */
inline constexpr uint16_t MAX_ENUM_VALUES = 1024;

/** @brief Number of distinct values a column's dictionary holds unless a cap is given. */
inline constexpr uint16_t DEFAULT_ENUM_VALUE_CAP = 64;

/**
 * @brief Longest value, in bytes, that still looks like an enumeration value. Longer values count against the column's
 * health.
 */
inline constexpr uint32_t MAX_ENUM_CANDIDATE_LEN = 64;

/**
 * @brief The distinct string values of one enumeration column, each with a small id.
 *
 * Ids are assigned in insertion order and never change. One thread writes; any number of threads may read while no
 * write is in progress.
 */
class EnumDictionary
{
public:
    EnumDictionary() = default;

    /** @brief Creates a dictionary that holds at most cap values. The cap is limited to 1 through MAX_ENUM_VALUES. */
    explicit EnumDictionary(uint16_t cap) noexcept;

    EnumDictionary(const EnumDictionary &) = delete;
    EnumDictionary &operator=(const EnumDictionary &) = delete;

    EnumDictionary(EnumDictionary &&) noexcept = default;
    EnumDictionary &operator=(EnumDictionary &&) noexcept = default;

    ~EnumDictionary() = default;

    /** @brief The id of bytes, or INVALID_ENUM_VALUE_ID if it was never inserted. */
    [[nodiscard]] EnumValueId Find(std::string_view bytes) const noexcept;

    /**
     * @brief The id of bytes, adding it on first sight. Returns INVALID_ENUM_VALUE_ID if bytes is new and the
     * dictionary is full.
     */
    [[nodiscard]] EnumValueId Insert(std::string_view bytes);

    /** @brief The value for id, or an empty view if the id is not in the dictionary. */
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

    /** @brief All values in id order. The strings keep their address until Clear, so views into them stay valid. */
    [[nodiscard]] const std::deque<std::string> &Values() const noexcept
    {
        return mValues;
    }

    void Clear() noexcept;

private:
    /** @brief A deque keeps each string at a fixed address, so the index can key on views into them. */
    std::deque<std::string> mValues;
    tsl::robin_map<std::string_view, EnumValueId, loglib::TransparentStringHash, loglib::TransparentStringEqual> mIndex;
    uint16_t mCap = DEFAULT_ENUM_VALUE_CAP;
};

/**
 * @brief The dictionaries of all enumeration columns, looked up by key.
 *
 * LogTable owns it, and each LineSource points at it so a DictRef cell can be turned back into text. Several keys can
 * share one dictionary through Alias.
 */
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

    /**
     * @brief The dictionary for key, created with the given cap if it does not exist. The reference stays valid until
     * Erase or Clear.
     */
    [[nodiscard]] EnumDictionary &GetOrInsert(KeyId key, uint16_t cap = DEFAULT_ENUM_VALUE_CAP);

    /**
     * @brief Makes alias use the dictionary of canonical.
     *
     * canonical must already have a dictionary, and alias must not be a canonical key or an alias of another
     * dictionary. Repeating a call is harmless. When a requirement is not met it returns false and changes nothing.
     */
    [[nodiscard]] bool Alias(KeyId canonical, KeyId alias);

    /** @brief Removes the dictionary of canonical and every alias that points to it. */
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

    /** @brief Approximate heap bytes used. For the memory benchmark. */
    [[nodiscard]] size_t EstimatedMemoryBytes() const noexcept;

private:
    /** @brief A dictionary and every key that uses it. The list always includes the canonical key. */
    struct DictionaryEntry
    {
        std::unique_ptr<EnumDictionary> dict;
        std::vector<KeyId> aliases;
    };

    /** @brief Dictionaries by canonical key. They are held by pointer so references survive a rehash. */
    tsl::robin_map<KeyId, DictionaryEntry> mDictionaries;
    /** @brief Every canonical and alias key, mapped to its dictionary. */
    tsl::robin_map<KeyId, EnumDictionary *> mIndex;
};

} // namespace loglib
