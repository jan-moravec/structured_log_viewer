#include "loglib/log_string_matcher.hpp"

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace loglib
{
namespace
{

constexpr char32_t REPLACEMENT = 0xFFFD;
constexpr unsigned char ASCII_DEL = 0x7F;
constexpr unsigned char ASCII_SPACE = 0x20;
constexpr unsigned char HIGH_BIT = 0x80;
constexpr char32_t ASCII_TAB = 0x09;
constexpr char32_t ASCII_CR = 0x0D;
constexpr char32_t NEXT_LINE = 0x85;
constexpr char32_t NO_BREAK_SPACE = 0xA0;
constexpr char32_t OGHAM_SPACE = 0x1680;
constexpr char32_t EN_QUAD = 0x2000;
constexpr char32_t HAIR_SPACE = 0x200A;
constexpr char32_t LINE_SEPARATOR = 0x2028;
constexpr char32_t PARAGRAPH_SEPARATOR = 0x2029;
constexpr char32_t NARROW_NO_BREAK_SPACE = 0x202F;
constexpr char32_t MEDIUM_MATHEMATICAL_SPACE = 0x205F;
constexpr char32_t IDEOGRAPHIC_SPACE = 0x3000;
constexpr unsigned char CONTINUATION_MASK = 0xC0;
constexpr unsigned char CONTINUATION_BITS = 0x3F;
constexpr unsigned char TWO_BYTE_MASK = 0xE0;
constexpr unsigned char TWO_BYTE_PREFIX = 0xC0;
constexpr unsigned char TWO_BYTE_LEAD_BITS = 0x1F;
constexpr unsigned char THREE_BYTE_MASK = 0xF0;
constexpr unsigned char THREE_BYTE_PREFIX = 0xE0;
constexpr unsigned char THREE_BYTE_LEAD_BITS = 0x0F;
constexpr unsigned char FOUR_BYTE_MASK = 0xF8;
constexpr unsigned char FOUR_BYTE_PREFIX = 0xF0;
constexpr unsigned char FOUR_BYTE_LEAD_BITS = 0x07;
constexpr char32_t UTF8_MIN_2 = 0x80;
constexpr char32_t UTF8_MAX_2 = 0x7FF;
constexpr char32_t UTF8_MIN_3 = 0x800;
constexpr char32_t UTF8_MAX_3 = 0xFFFF;
constexpr char32_t UTF8_MIN_4 = 0x10000;
constexpr char32_t UNICODE_MAX = 0x10FFFF;
constexpr char32_t SURROGATE_MIN = 0xD800;
constexpr char32_t SURROGATE_MAX = 0xDFFF;
constexpr int UTF8_SHIFT_6 = 6;
constexpr int UTF8_SHIFT_12 = 12;
constexpr int UTF8_SHIFT_18 = 18;
constexpr std::size_t WILDCARD_ANCHOR_BYTES = 8;

/// True when @p bytes already equal their normalised form and are 7-bit ASCII.
bool IsSingleLineAsciiTrim(std::string_view bytes) noexcept
{
    if (bytes.empty())
    {
        return true;
    }
    const auto first = static_cast<unsigned char>(bytes.front());
    if (first <= ASCII_SPACE)
    {
        return false;
    }
    const auto last = static_cast<unsigned char>(bytes.back());
    if (last <= ASCII_SPACE)
    {
        return false;
    }
    bool previousWasSpace = false;
    for (const char ch : bytes)
    {
        const auto byte = static_cast<unsigned char>(ch);
        if (byte >= HIGH_BIT)
        {
            return false;
        }
        if (byte == ASCII_SPACE)
        {
            if (previousWasSpace)
            {
                return false;
            }
            previousWasSpace = true;
            continue;
        }
        if (byte < ASCII_SPACE || byte == ASCII_DEL)
        {
            return false;
        }
        previousWasSpace = false;
    }
    return true;
}

/// Qt `QChar::isSpace`: ASCII controls, NEL, NBSP, and Unicode separators.
bool IsMatchWhitespace(char32_t codePoint) noexcept
{
    if (codePoint == ASCII_SPACE || (codePoint <= ASCII_CR && codePoint >= ASCII_TAB))
    {
        return true;
    }
    if (codePoint < HIGH_BIT)
    {
        return false;
    }
    if (codePoint == NEXT_LINE || codePoint == NO_BREAK_SPACE || codePoint == OGHAM_SPACE ||
        codePoint == LINE_SEPARATOR || codePoint == PARAGRAPH_SEPARATOR || codePoint == NARROW_NO_BREAK_SPACE ||
        codePoint == MEDIUM_MATHEMATICAL_SPACE || codePoint == IDEOGRAPHIC_SPACE)
    {
        return true;
    }
    return codePoint >= EN_QUAD && codePoint <= HAIR_SPACE;
}

struct DecodedCodePoint
{
    char32_t codePoint = REPLACEMENT;
    std::size_t size = 1;
    bool valid = false;
};

/// One UTF-8 sequence, or one byte replaced by U+FFFD. Overlongs and
/// surrogates are invalid.
DecodedCodePoint DecodeOne(std::string_view bytes)
{
    const auto lead = static_cast<unsigned char>(bytes.front());
    if (lead < HIGH_BIT)
    {
        return {.codePoint = lead, .size = 1, .valid = true};
    }

    auto continuation = [&](std::size_t index) -> int {
        if (index >= bytes.size())
        {
            return -1;
        }
        const auto byte = static_cast<unsigned char>(bytes[index]);
        if ((byte & CONTINUATION_MASK) != HIGH_BIT)
        {
            return -1;
        }
        return byte & CONTINUATION_BITS;
    };

    if ((lead & TWO_BYTE_MASK) == TWO_BYTE_PREFIX)
    {
        const int tail = continuation(1);
        if (tail >= 0)
        {
            const auto codePoint =
                (static_cast<char32_t>(lead & TWO_BYTE_LEAD_BITS) << UTF8_SHIFT_6) | static_cast<char32_t>(tail);
            if (codePoint >= UTF8_MIN_2)
            {
                return {.codePoint = codePoint, .size = 2, .valid = true};
            }
        }
    }
    else if ((lead & THREE_BYTE_MASK) == THREE_BYTE_PREFIX)
    {
        const int tail1 = continuation(1);
        const int tail2 = continuation(2);
        if (tail1 >= 0 && tail2 >= 0)
        {
            const auto codePoint = (static_cast<char32_t>(lead & THREE_BYTE_LEAD_BITS) << UTF8_SHIFT_12) |
                                   (static_cast<char32_t>(tail1) << UTF8_SHIFT_6) | static_cast<char32_t>(tail2);
            if (codePoint >= UTF8_MIN_3 && (codePoint < SURROGATE_MIN || codePoint > SURROGATE_MAX))
            {
                return {.codePoint = codePoint, .size = 3, .valid = true};
            }
        }
    }
    else if ((lead & FOUR_BYTE_MASK) == FOUR_BYTE_PREFIX)
    {
        const int tail1 = continuation(1);
        const int tail2 = continuation(2);
        const int tail3 = continuation(3);
        if (tail1 >= 0 && tail2 >= 0 && tail3 >= 0)
        {
            const auto codePoint = (static_cast<char32_t>(lead & FOUR_BYTE_LEAD_BITS) << UTF8_SHIFT_18) |
                                   (static_cast<char32_t>(tail1) << UTF8_SHIFT_12) |
                                   (static_cast<char32_t>(tail2) << UTF8_SHIFT_6) | static_cast<char32_t>(tail3);
            if (codePoint >= UTF8_MIN_4 && codePoint <= UNICODE_MAX)
            {
                return {.codePoint = codePoint, .size = 4, .valid = true};
            }
        }
    }
    return {.codePoint = REPLACEMENT, .size = 1, .valid = false};
}

void AppendUtf8(std::string &out, char32_t codePoint)
{
    if (codePoint < UTF8_MIN_2)
    {
        out.push_back(static_cast<char>(codePoint));
        return;
    }
    if (codePoint <= UTF8_MAX_2)
    {
        out.push_back(static_cast<char>(TWO_BYTE_PREFIX | (codePoint >> UTF8_SHIFT_6)));
        out.push_back(static_cast<char>(HIGH_BIT | (codePoint & CONTINUATION_BITS)));
        return;
    }
    if (codePoint <= UTF8_MAX_3)
    {
        out.push_back(static_cast<char>(THREE_BYTE_PREFIX | (codePoint >> UTF8_SHIFT_12)));
        out.push_back(static_cast<char>(HIGH_BIT | ((codePoint >> UTF8_SHIFT_6) & CONTINUATION_BITS)));
        out.push_back(static_cast<char>(HIGH_BIT | (codePoint & CONTINUATION_BITS)));
        return;
    }
    out.push_back(static_cast<char>(FOUR_BYTE_PREFIX | (codePoint >> UTF8_SHIFT_18)));
    out.push_back(static_cast<char>(HIGH_BIT | ((codePoint >> UTF8_SHIFT_12) & CONTINUATION_BITS)));
    out.push_back(static_cast<char>(HIGH_BIT | ((codePoint >> UTF8_SHIFT_6) & CONTINUATION_BITS)));
    out.push_back(static_cast<char>(HIGH_BIT | (codePoint & CONTINUATION_BITS)));
}

/// UTF-8 decode with U+FFFD substitution. Does not trim or collapse.
std::string DecodeUtf8(std::string_view bytes)
{
    std::string out;
    out.reserve(bytes.size());
    std::size_t index = 0;
    while (index < bytes.size())
    {
        const DecodedCodePoint decoded = DecodeOne(bytes.substr(index));
        if (decoded.valid)
        {
            out.append(bytes.substr(index, decoded.size));
        }
        else
        {
            AppendUtf8(out, REPLACEMENT);
        }
        index += decoded.size;
    }
    return out;
}

/// `fromUtf8` plus `simplified()`: trim both ends, collapse whitespace to one space.
std::string Simplify(std::string_view bytes)
{
    std::string out;
    out.reserve(bytes.size());
    bool pendingSpace = false;
    bool wrote = false;
    std::size_t index = 0;
    while (index < bytes.size())
    {
        const DecodedCodePoint decoded = DecodeOne(bytes.substr(index));
        index += decoded.size;
        if (IsMatchWhitespace(decoded.codePoint))
        {
            if (wrote)
            {
                pendingSpace = true;
            }
            continue;
        }
        if (pendingSpace)
        {
            out.push_back(' ');
            pendingSpace = false;
        }
        if (decoded.valid && decoded.codePoint != REPLACEMENT)
        {
            out.append(bytes.substr(index - decoded.size, decoded.size));
        }
        else
        {
            AppendUtf8(out, REPLACEMENT);
        }
        wrote = true;
    }
    return out;
}

struct CodeDeleter
{
    void operator()(pcre2_code *code) const noexcept
    {
        if (code != nullptr)
        {
            pcre2_code_free(code);
        }
    }
};

struct CompiledRegex
{
    std::shared_ptr<pcre2_code> code;
    bool jit = false;
};

CompiledRegex CompilePattern(std::string_view pattern)
{
    int errcode = 0;
    PCRE2_SIZE erroffset = 0;
    pcre2_code *raw = pcre2_compile(
        reinterpret_cast<PCRE2_SPTR>(pattern.data()),
        static_cast<PCRE2_SIZE>(pattern.size()),
        PCRE2_UTF,
        &errcode,
        &erroffset,
        nullptr
    );
    if (raw == nullptr)
    {
        return {};
    }

    CompiledRegex compiled;
    compiled.code.reset(raw, CodeDeleter{});
    compiled.jit = pcre2_jit_compile(raw, PCRE2_JIT_COMPLETE) == 0;

    // Prime the JIT so the first caller does not pay the stub setup.
    pcre2_match_data *matchData = pcre2_match_data_create(1, nullptr);
    if (matchData != nullptr)
    {
        const auto *empty = reinterpret_cast<PCRE2_SPTR>("");
        if (compiled.jit)
        {
            pcre2_jit_match(raw, empty, 0, 0, PCRE2_NO_UTF_CHECK, matchData, nullptr);
        }
        else
        {
            pcre2_match(raw, empty, 0, 0, PCRE2_NO_UTF_CHECK, matchData, nullptr);
        }
        pcre2_match_data_free(matchData);
    }
    return compiled;
}

bool RunMatch(const CompiledRegex &compiled, std::string_view subject)
{
    if (compiled.code == nullptr)
    {
        return false;
    }
    pcre2_match_data *matchData = pcre2_match_data_create(1, nullptr);
    if (matchData == nullptr)
    {
        return false;
    }
    const auto *bytes = reinterpret_cast<PCRE2_SPTR>(subject.data());
    const auto length = static_cast<PCRE2_SIZE>(subject.size());
    const int rc = compiled.jit
                       ? pcre2_jit_match(compiled.code.get(), bytes, length, 0, PCRE2_NO_UTF_CHECK, matchData, nullptr)
                       : pcre2_match(compiled.code.get(), bytes, length, 0, PCRE2_NO_UTF_CHECK, matchData, nullptr);
    pcre2_match_data_free(matchData);
    return rc >= 0;
}

CallbackStringRowPredicate::MatchFn MakeRegexMatcher(std::string_view pattern)
{
    const std::string decoded = DecodeUtf8(pattern);
    const CompiledRegex compiled = CompilePattern(decoded);
    if (compiled.code == nullptr)
    {
        return [](std::string_view) { return false; };
    }
    return [compiled](std::string_view haystack) {
        if (IsSingleLineAsciiTrim(haystack))
        {
            return RunMatch(compiled, haystack);
        }
        return RunMatch(compiled, Simplify(haystack));
    };
}

bool IsRegexMeta(unsigned char byte) noexcept
{
    switch (byte)
    {
    case '.':
    case '^':
    case '$':
    case '+':
    case '(':
    case ')':
    case '{':
    case '}':
    case '|':
    case '\\':
        return true;
    default:
        return false;
    }
}

/// `*` → `.*`, `?` → `.`, classes copied, everything else escaped.
/// Anchored so the wildcard covers the whole haystack. `*` and `?`
/// match `/` and `\`.
std::string WildcardToPcre(std::string_view pattern)
{
    std::string out;
    out.reserve(pattern.size() + WILDCARD_ANCHOR_BYTES);
    out.append("\\A(?:");
    std::size_t index = 0;
    while (index < pattern.size())
    {
        const auto byte = static_cast<unsigned char>(pattern[index]);
        if (byte == '[')
        {
            std::size_t end = index + 1;
            if (end < pattern.size() && pattern[end] == ']')
            {
                ++end;
            }
            while (end < pattern.size() && pattern[end] != ']')
            {
                ++end;
            }
            if (end < pattern.size())
            {
                ++end;
            }
            out.append(pattern.substr(index, end - index));
            index = end;
            continue;
        }
        if (byte == '*')
        {
            out.append(".*");
            ++index;
            continue;
        }
        if (byte == '?')
        {
            out.push_back('.');
            ++index;
            continue;
        }
        if (IsRegexMeta(byte))
        {
            out.push_back('\\');
        }
        out.push_back(static_cast<char>(byte));
        ++index;
    }
    out.append(")\\z");
    return out;
}

CallbackStringRowPredicate::MatchFn MakeWildcardMatcher(std::string_view pattern)
{
    const std::string translated = WildcardToPcre(DecodeUtf8(pattern));
    const CompiledRegex compiled = CompilePattern(translated);
    if (compiled.code == nullptr)
    {
        return [](std::string_view) { return false; };
    }
    return [compiled](std::string_view haystack) {
        if (IsSingleLineAsciiTrim(haystack))
        {
            return RunMatch(compiled, haystack);
        }
        return RunMatch(compiled, Simplify(haystack));
    };
}

CallbackStringRowPredicate::MatchFn MakeExactly(std::string_view pattern)
{
    if (IsSingleLineAsciiTrim(pattern))
    {
        std::string expected(pattern);
        return [expected = std::move(expected)](std::string_view haystack) {
            if (IsSingleLineAsciiTrim(haystack))
            {
                return haystack == expected;
            }
            return Simplify(haystack) == expected;
        };
    }
    std::string expected = DecodeUtf8(pattern);
    return [expected = std::move(expected)](std::string_view haystack) { return Simplify(haystack) == expected; };
}

CallbackStringRowPredicate::MatchFn MakeContains(std::string_view pattern)
{
    if (IsSingleLineAsciiTrim(pattern))
    {
        std::string needle(pattern);
        return [needle = std::move(needle)](std::string_view haystack) {
            if (IsSingleLineAsciiTrim(haystack))
            {
                return haystack.contains(needle);
            }
            return Simplify(haystack).contains(needle);
        };
    }
    std::string needle = DecodeUtf8(pattern);
    return [needle = std::move(needle)](std::string_view haystack) { return Simplify(haystack).contains(needle); };
}

} // namespace

CallbackStringRowPredicate::MatchFn MakeStringMatcher(std::string_view pattern, LeafRule::Match match)
{
    using Match = LeafRule::Match;
    switch (match)
    {
    case Match::Exactly:
        return MakeExactly(pattern);
    case Match::Contains:
        return MakeContains(pattern);
    case Match::RegularExpression:
        return MakeRegexMatcher(pattern);
    case Match::Wildcard:
        return MakeWildcardMatcher(pattern);
    }
    return [](std::string_view) { return false; };
}

} // namespace loglib
