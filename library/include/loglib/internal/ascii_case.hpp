#pragma once

#include <cstddef>
#include <string_view>

namespace loglib::internal
{

/// Case-insensitive ASCII equality. ASCII-only by design: log column
/// keys we case-fold ("level", "true", "false", ...) are guaranteed
/// 7-bit, so a per-byte fold avoids the heap allocation of a
/// locale-aware comparison and keeps the helper safe to call from
/// hot parser/filter paths.
constexpr bool EqualsIgnoreCaseAscii(std::string_view a, std::string_view b) noexcept
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

} // namespace loglib::internal
