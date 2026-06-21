#pragma once

#include "loglib/internal/compact_log_value.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <system_error>

// libc++ ships without `std::from_chars` for floating-point types as of
// LLVM 18 (Xcode 16); fall back to a locale-safe `strtod` on a stack
// buffer there. The extra headers are only pulled in on that branch.
#if !defined(__cpp_lib_to_chars) || __cpp_lib_to_chars < 201611L || defined(_LIBCPP_VERSION)
#include <array>
#include <cerrno>
#include <cstdlib>
#endif

namespace loglib::internal
{

/// Parse @p raw as a finite double. Wraps `std::from_chars` where
/// available and falls back to a strict-syntax `std::strtod` on a stack
/// buffer when libc++ lacks the floating-point overload (Xcode 16's
/// libc++ as of LLVM 18). The fallback first validates that @p raw
/// matches a strict C floating-point literal (no leading whitespace,
/// no hex, no `nan` / `inf` tokens) so the locale-driven decimal
/// separator in `strtod` cannot reinterpret the bytes.
///
/// `inline` (not out-of-line) so per-bare-value callers in the logfmt
/// and CSV hot paths keep their within-TU inlining regardless of LTO.
inline bool TryParseFiniteDouble(std::string_view raw, double &outValue)
{
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L && !defined(_LIBCPP_VERSION)
    const char *first = raw.data();
    const char *last = raw.data() + raw.size();
    const auto res = std::from_chars(first, last, outValue);
    return res.ec == std::errc{} && res.ptr == last && std::isfinite(outValue);
#else
    // Stack buffer cap for the strtod fallback. Any IEEE 754 double
    // round-trips in well under this width including sign / exponent /
    // padding; longer inputs are rejected rather than allocating.
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

/// Promote @p sv to a compact value. `MmapSlice` (zero copy) when it
/// points inside `[fileBegin, fileBegin + fileSize)`, otherwise the
/// bytes are copied into @p ownedArena and tagged `OwnedString`.
/// Streaming callers pass `fileBegin == nullptr` (which gates the
/// range check — relational compares across unrelated objects are UB).
///
/// `inline` so the parser hot paths keep their within-TU inlining.
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

/// Typed-value classifier for bare (unquoted) values. Used by logfmt
/// for bare values and by CSV for unquoted cells; quoted values bypass
/// this and stay strings (so `pid="42"` / `"42"` keeps the user's
/// intent in both formats).
///
/// Empty input is treated as monostate. `true` / `false` become bool.
/// Decimal integers without a sign go through `uint64_t` first so
/// large positive ids stay exact; signed integers fall through to
/// `int64_t`. Anything with a sign / dot / exponent goes through
/// `TryParseFiniteDouble`. Trailing junk (e.g. `42abc`) is rejected
/// and falls through to a string.
///
/// `inline` so the parser hot paths keep their within-TU inlining.
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

    // Probe int / uint / double via std::from_chars. Trailing junk
    // (e.g. `42abc`) is rejected and falls through to string.
    const char *first = raw.data();
    const char *last = raw.data() + raw.size();

    if (raw.front() == '-')
    {
        int64_t i64 = 0;
        const auto res = std::from_chars(first, last, i64);
        if (res.ec == std::errc{} && res.ptr == last)
        {
            return CompactLogValue::MakeInt64(i64);
        }
    }
    else if (raw.front() >= '0' && raw.front() <= '9')
    {
        // Unsigned first so large positive ids stay exact.
        uint64_t u64 = 0;
        const auto res = std::from_chars(first, last, u64);
        if (res.ec == std::errc{} && res.ptr == last)
        {
            return CompactLogValue::MakeUint64(u64);
        }
    }

    if (raw.front() == '-' || raw.front() == '+' || raw.front() == '.' || (raw.front() >= '0' && raw.front() <= '9'))
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
