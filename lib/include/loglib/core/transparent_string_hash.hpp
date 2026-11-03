/**
 * @brief Hash and equality that allow string_view lookups.
 *
 * Using these with an unordered map lets a caller look up by string_view or const char pointer without building a
 * std::string first.
 *
 * This is part of the core module.
 */
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

namespace loglib
{

/**
 * @brief A hash for string keys that lets a map be searched with a string_view or a C string, without building a
 * std::string.
 */
struct TransparentStringHash
{
    using is_transparent = void;

    size_t operator()(std::string_view sv) const noexcept
    {
        return std::hash<std::string_view>{}(sv);
    }
    size_t operator()(const std::string &s) const noexcept
    {
        return std::hash<std::string_view>{}(s);
    }
    size_t operator()(const char *s) const noexcept
    {
        return std::hash<std::string_view>{}(std::string_view(s));
    }
};

/** @brief The equality test to use with TransparentStringHash. */
struct TransparentStringEqual
{
    using is_transparent = void;

    bool operator()(std::string_view lhs, std::string_view rhs) const noexcept
    {
        return lhs == rhs;
    }
    bool operator()(std::string_view lhs, const std::string &rhs) const noexcept
    {
        return lhs == rhs;
    }
    bool operator()(const std::string &lhs, std::string_view rhs) const noexcept
    {
        return lhs == rhs;
    }
    bool operator()(const std::string &lhs, const std::string &rhs) const noexcept
    {
        return lhs == rhs;
    }
};

} // namespace loglib
