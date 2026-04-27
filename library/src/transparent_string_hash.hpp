#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

namespace loglib::detail
{

/// Transparent hash for heterogeneous `string`/`string_view` lookup.
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

/// Transparent equality companion for `TransparentStringHash`.
struct TransparentStringEqual
{
    using is_transparent = void;

    bool operator()(std::string_view lhs, std::string_view rhs) const noexcept { return lhs == rhs; }
    bool operator()(std::string_view lhs, const std::string &rhs) const noexcept { return lhs == rhs; }
    bool operator()(const std::string &lhs, std::string_view rhs) const noexcept { return lhs == rhs; }
    bool operator()(const std::string &lhs, const std::string &rhs) const noexcept { return lhs == rhs; }
};

} // namespace loglib::detail
