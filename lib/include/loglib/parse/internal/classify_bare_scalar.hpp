/**
 * @brief Decides whether an unquoted text value is a number, a boolean or a string.
 *
 * Logfmt values and CSV cells have no type markers. These helpers type an unquoted scalar: empty text is nothing, true
 * and false are booleans, numbers become integers or doubles, and anything else stays a string. Both parsers share them
 * so a value gets the same type in either format. The header also builds the compact value that points at the file or
 * copies the text into an arena.
 *
 * This is part of the parse module.
 */
#pragma once

#include "loglib/core/compact_log_value.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <system_error>

// Some standard libraries, such as the libc++ that ships with Xcode 16, have no from_chars for floating point numbers.
// They use a strtod fallback that first checks the text for a strict number.
#if !defined(__cpp_lib_to_chars) || __cpp_lib_to_chars < 201611L || defined(_LIBCPP_VERSION)
#include <array>
#include <cerrno>
#include <cstdlib>
#endif

namespace loglib::internal
{

/**
 * @brief Reads text as a finite double. Returns false for anything else, including infinity and NaN.
 *
 * It uses from_chars where the library has it. Otherwise it checks that the text is a plain C floating-point number
 * before calling strtod, so the decimal separator of the current locale cannot change what the bytes mean. It is inline
 * so the logfmt and CSV hot paths can inline it.
 */
inline bool TryParseFiniteDouble(std::string_view raw, double &outValue)
{
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L && !defined(_LIBCPP_VERSION)
    const char *first = raw.data();
    const char *last = raw.data() + raw.size();
    const auto res = std::from_chars(first, last, outValue);
    return res.ec == std::errc{} && res.ptr == last && std::isfinite(outValue);
#else
    // Every IEEE 754 double fits in well under 64 bytes of text, so longer input is rejected and nothing is allocated.
    constexpr size_t DOUBLE_PARSE_BUFFER_SIZE = 64;

    if (raw.empty() || raw.size() >= DOUBLE_PARSE_BUFFER_SIZE)
    {
        return false;
    }

    size_t i = 0;
    if (raw[i] == '+' || raw[i] == '-')
    {
        ++i;
    }
    bool sawDigit = false;
    bool sawDot = false;
    while (i < raw.size())
    {
        const char c = raw[i];
        if (c >= '0' && c <= '9')
        {
            sawDigit = true;
            ++i;
            continue;
        }
        if (c == '.' && !sawDot)
        {
            sawDot = true;
            ++i;
            continue;
        }
        break;
    }
    if (!sawDigit)
    {
        return false;
    }
    if (i < raw.size() && (raw[i] == 'e' || raw[i] == 'E'))
    {
        ++i;
        if (i < raw.size() && (raw[i] == '+' || raw[i] == '-'))
        {
            ++i;
        }
        bool sawExpDigit = false;
        while (i < raw.size() && raw[i] >= '0' && raw[i] <= '9')
        {
            sawExpDigit = true;
            ++i;
        }
        if (!sawExpDigit)
        {
            return false;
        }
    }
    if (i != raw.size())
    {
        return false;
    }

    std::array<char, DOUBLE_PARSE_BUFFER_SIZE> buffer{};
    std::memcpy(buffer.data(), raw.data(), raw.size());
    buffer[raw.size()] = '\0';

    errno = 0;
    char *end = nullptr;
    const double value = std::strtod(buffer.data(), &end);
    if (errno == ERANGE || end != buffer.data() + raw.size() || !std::isfinite(value))
    {
        return false;
    }
    outValue = value;
    return true;
#endif
}

/**
 * @brief Turns text into a compact string value.
 *
 * Text inside the file range becomes a MmapSlice and costs no copy. Other text is copied into ownedArena as an
 * OwnedString. A streaming caller passes a null fileBegin to skip the range check, because comparing pointers of
 * unrelated objects is undefined. It is inline for hot paths.
 */
inline CompactLogValue MakeStringCompact(
    std::string_view sv, const char *fileBegin, size_t fileSize, std::string &ownedArena
)
{
    if (fileBegin != nullptr && sv.data() >= fileBegin && sv.data() + sv.size() <= fileBegin + fileSize)
    {
        const auto offset = static_cast<uint64_t>(sv.data() - fileBegin);
        return CompactLogValue::MakeMmapSlice(offset, static_cast<uint32_t>(sv.size()));
    }
    const uint64_t offset = ownedArena.size();
    ownedArena.append(sv.data(), sv.size());
    return CompactLogValue::MakeOwnedString(offset, static_cast<uint32_t>(sv.size()));
}

/**
 * @brief Chooses the type of an unquoted value, as found in logfmt and CSV. Quoted values skip this and stay strings.
 *
 * The result is:
 * - empty for empty text,
 * - a boolean for true and false,
 * - an unsigned integer, or else a signed one, for decimal digits,
 * - a finite double for forms with a sign, a dot or an exponent,
 * - a string for everything else, including text with trailing junk such as 42abc.
 *
 * Numbers that cannot stand for a quantity stay strings: values with a leading zero such as 007, and integers that do
 * not fit in 64 bits, which are identifiers. It is inline for hot paths.
 */
inline CompactLogValue ClassifyBareScalar(
    std::string_view raw, const char *fileBegin, size_t fileSize, std::string &ownedArena
)
{
    if (raw.empty())
    {
        return CompactLogValue::MakeMonostate();
    }

    if (raw == "true")
    {
        return CompactLogValue::MakeBool(true);
    }
    if (raw == "false")
    {
        return CompactLogValue::MakeBool(false);
    }

    const char *first = raw.data();
    const char *last = raw.data() + raw.size();

    const std::string_view unsignedPart = raw.front() == '-' ? raw.substr(1) : raw;
    if (unsignedPart.size() >= 2 && unsignedPart[0] == '0' && unsignedPart[1] >= '0' && unsignedPart[1] <= '9')
    {
        return MakeStringCompact(raw, fileBegin, fileSize, ownedArena);
    }

    if (raw.front() == '-')
    {
        // NOLINTNEXTLINE(misc-const-correctness): mutated through from_chars' out-ref.
        int64_t i64 = 0;
        const auto res = std::from_chars(first, last, i64);
        if (res.ec == std::errc{} && res.ptr == last)
        {
            return CompactLogValue::MakeInt64(i64);
        }
        if (res.ec == std::errc::result_out_of_range && res.ptr == last)
        {
            return MakeStringCompact(raw, fileBegin, fileSize, ownedArena);
        }
    }
    else if (raw.front() >= '0' && raw.front() <= '9')
    {
        // Unsigned first so large positive ids round-trip exactly.
        // NOLINTNEXTLINE(misc-const-correctness): mutated through from_chars' out-ref.
        uint64_t u64 = 0;
        const auto res = std::from_chars(first, last, u64);
        if (res.ec == std::errc{} && res.ptr == last)
        {
            return CompactLogValue::MakeUint64(u64);
        }
        if (res.ec == std::errc::result_out_of_range && res.ptr == last)
        {
            return MakeStringCompact(raw, fileBegin, fileSize, ownedArena);
        }
    }

    if (raw.front() == '-' || raw.front() == '.' || (raw.front() >= '0' && raw.front() <= '9'))
    {
        double d = 0.0;
        if (TryParseFiniteDouble(raw, d))
        {
            return CompactLogValue::MakeDouble(d);
        }
    }

    return MakeStringCompact(raw, fileBegin, fileSize, ownedArena);
}

} // namespace loglib::internal
