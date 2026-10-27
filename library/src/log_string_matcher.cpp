#include "loglib/log_string_matcher.hpp"

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>
#include <utf8proc.h>

#include <array>
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

/// Qt `QChar::isSpace`: tab through carriage return, NEL, and every
/// Unicode separator (space, line, and paragraph).
bool IsMatchWhitespace(char32_t codePoint) noexcept
{
    if ((codePoint >= ASCII_TAB && codePoint <= ASCII_CR) || codePoint == NEXT_LINE)
    {
        return true;
    }
    switch (utf8proc_category(static_cast<utf8proc_int32_t>(codePoint)))
    {
    case UTF8PROC_CATEGORY_ZS:
    case UTF8PROC_CATEGORY_ZL:
    case UTF8PROC_CATEGORY_ZP:
        return true;
    default:
        return false;
    }
}

struct DecodedCodePoint
{
    char32_t codePoint = REPLACEMENT;
    std::size_t size = 1;
    bool valid = false;
};

/// One UTF-8 sequence, or one byte replaced by U+FFFD. Overlongs,
/// surrogates, and values past U+10FFFF are invalid.
DecodedCodePoint DecodeOne(std::string_view bytes) noexcept
{
    utf8proc_int32_t codePoint = 0;
    const utf8proc_ssize_t size = utf8proc_iterate(
        reinterpret_cast<const utf8proc_uint8_t *>(bytes.data()),
        static_cast<utf8proc_ssize_t>(bytes.size()),
        &codePoint
    );
    if (size <= 0 || codePoint < 0)
    {
        return {};
    }
    return {.codePoint = static_cast<char32_t>(codePoint), .size = static_cast<std::size_t>(size), .valid = true};
}

void AppendUtf8(std::string &out, char32_t codePoint)
{
    std::array<utf8proc_uint8_t, 4> buffer{};
    const utf8proc_ssize_t size = utf8proc_encode_char(static_cast<utf8proc_int32_t>(codePoint), buffer.data());
    out.append(reinterpret_cast<const char *>(buffer.data()), static_cast<std::size_t>(size));
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
