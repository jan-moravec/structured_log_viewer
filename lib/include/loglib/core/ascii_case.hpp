#pragma once

#include <cstddef>
#include <string_view>

namespace loglib
{

/**
 * @brief Returns whether two views are equal ignoring 7-bit ASCII case.
 *
 * The keys this folds (`"level"`, `"true"`, `"false"`, ...) are ASCII, so
 * the comparison avoids a locale-aware allocation. Safe on hot paths.
 *
 * @param a First view.
 * @param b Second view.
 * @return `true` when the views match after folding `A`–`Z` to `a`–`z`.
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
