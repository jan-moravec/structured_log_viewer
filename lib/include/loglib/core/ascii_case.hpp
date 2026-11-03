/**
 * @brief Case-insensitive comparison for ASCII text.
 *
 * The keys and literals the library compares, such as level or true, are plain ASCII. Folding only the 7-bit letters
 * avoids a locale-aware allocation, so the helper is cheap enough for hot paths.
 *
 * This is part of the core module.
 */
#pragma once

#include <cstddef>
#include <string_view>

namespace loglib
{

/**
 * @brief Returns whether two views are equal when the letters A to Z are treated as a to z.
 *
 * The words it compares, such as level names and true and false, are ASCII. Comparing bytes avoids the locale lookup
 * and allocation of a locale-aware comparison, so it is cheap enough for hot paths.
 */
[[nodiscard]] constexpr bool EqualsIgnoreCaseAscii(std::string_view a, std::string_view b) noexcept
{
    if (a.size() != b.size())
    {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i)
    {
        const auto ca = static_cast<unsigned char>(a[i]);
        const auto cb = static_cast<unsigned char>(b[i]);
        const unsigned char la = (ca >= 'A' && ca <= 'Z') ? static_cast<unsigned char>(ca + ('a' - 'A')) : ca;
        const unsigned char lb = (cb >= 'A' && cb <= 'Z') ? static_cast<unsigned char>(cb + ('a' - 'A')) : cb;
        if (la != lb)
        {
            return false;
        }
    }
    return true;
}

} // namespace loglib
