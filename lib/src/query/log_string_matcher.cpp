#include "loglib/query/log_string_matcher.hpp"

#include "loglib/core/internal/pcre2_support.hpp"

#include <utf8proc.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
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

/**
 * True when @p bytes already equal their normalised form and are 7-bit ASCII.
 */
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

/**
 * Qt QChar::isSpace: tab through carriage return, NEL, and every Unicode separator (space, line, and paragraph).
 */
bool IsMatchWhitespace(char32_t codePoint) noexcept
{
    if ((codePoint >= ASCII_TAB && codePoint <= ASCII_CR) || codePoint == NEXT_LINE)
    {
        return true;
    }
    if (codePoint < HIGH_BIT)
    {
        // The only other ASCII whitespace is the space itself, so ASCII text needs no category lookup.
        return codePoint == ASCII_SPACE;
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

/**
 * One UTF-8 sequence, or one byte replaced by U+FFFD. Overlongs, surrogates, and values past U+10FFFF are invalid.
 */
DecodedCodePoint DecodeOne(std::string_view bytes) noexcept
{
    if (!bytes.empty() && static_cast<unsigned char>(bytes.front()) < HIGH_BIT)
    {
        return {.codePoint = static_cast<unsigned char>(bytes.front()), .size = 1, .valid = true};
    }
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

/**
 * UTF-8 decode with U+FFFD substitution. Does not trim or collapse.
 */
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

/**
 * fromUtf8 plus simplified(): trim both ends, collapse whitespace to one space, and write the result to @p out. With @p
 * keepEdges, whitespace at an end becomes one space instead of being trimmed.
 */
void SimplifyInto(std::string &out, std::string_view bytes, bool keepEdges = false)
{
    out.clear();
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
            if (wrote || keepEdges)
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
    if (pendingSpace && keepEdges)
    {
        out.push_back(' ');
    }
}

std::string Simplify(std::string_view bytes, bool keepEdges = false)
{
    std::string out;
    SimplifyInto(out, bytes, keepEdges);
    return out;
}

/**
 * The text of @p bytes after Simplify, in a buffer of the calling thread. The view is valid until the thread's next
 * call. A matcher runs on many threads and many rows, so reusing the buffer avoids an allocation per row.
 */
std::string_view SimplifiedView(std::string_view bytes)
{
    thread_local std::string scratch;
    SimplifyInto(scratch, bytes);
    return scratch;
}

struct CompiledRegex
{
    std::shared_ptr<pcre2_code> code;
    bool jit = false;
};

CompiledRegex CompilePattern(std::string_view pattern)
{
    int errcode = 0;
    PCRE2_SIZE erroffset = 0;
    internal::Pcre2CodePtr code = internal::CompilePcre2(pattern, PCRE2_UTF, errcode, erroffset);
    if (code == nullptr)
    {
        return {};
    }

    pcre2_code *raw = code.get();
    CompiledRegex compiled;
    compiled.code = std::move(code);
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

/**
 * The match data of one thread. A match needs only the start and end of the whole match, so one block of one pair
 * serves every pattern, and a thread creates it once instead of once per cell.
 */
class ThreadMatchData
{
public:
    ThreadMatchData()
        : mData(pcre2_match_data_create(1, nullptr))
    {
    }
    ~ThreadMatchData()
    {
        pcre2_match_data_free(mData);
    }
    ThreadMatchData(const ThreadMatchData &) = delete;
    ThreadMatchData &operator=(const ThreadMatchData &) = delete;
    ThreadMatchData(ThreadMatchData &&) = delete;
    ThreadMatchData &operator=(ThreadMatchData &&) = delete;

    [[nodiscard]] pcre2_match_data *Get() const noexcept
    {
        return mData;
    }

private:
    pcre2_match_data *mData;
};

bool RunMatch(const CompiledRegex &compiled, std::string_view subject)
{
    if (compiled.code == nullptr)
    {
        return false;
    }
    thread_local const ThreadMatchData threadData;
    pcre2_match_data *matchData = threadData.Get();
    if (matchData == nullptr)
    {
        return false;
    }
    const auto *bytes = reinterpret_cast<PCRE2_SPTR>(subject.data());
    const auto length = static_cast<PCRE2_SIZE>(subject.size());
    const int rc = compiled.jit
                       ? pcre2_jit_match(compiled.code.get(), bytes, length, 0, PCRE2_NO_UTF_CHECK, matchData, nullptr)
                       : pcre2_match(compiled.code.get(), bytes, length, 0, PCRE2_NO_UTF_CHECK, matchData, nullptr);
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
        return RunMatch(compiled, SimplifiedView(haystack));
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

/**
 * Translates a wildcard pattern to a regular expression. A star becomes any text, a question mark becomes any one
 * character, bracket classes are copied and every other character is escaped. The expression is anchored so it covers
 * the whole cell. Star and question mark also match slash and backslash.
 */
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

CallbackStringRowPredicate::MatchFn MakeContainsOfSimplified(std::string needle);

CallbackStringRowPredicate::MatchFn MakeWildcardMatcher(std::string_view pattern)
{
    const std::string simplified = Simplify(pattern);
    // A pattern of the form *text*, with nothing special inside, matches the cells that contain text. The cell is
    // simplified before matching, so it holds no line break that a dot would refuse. Contains finds the same cells
    // without the regular expression.
    if (simplified.size() >= 2 && simplified.front() == '*' && simplified.back() == '*')
    {
        std::string inner = simplified.substr(1, simplified.size() - 2);
        if (inner.find_first_of("*?[") == std::string::npos)
        {
            return MakeContainsOfSimplified(std::move(inner));
        }
    }
    const std::string translated = WildcardToPcre(simplified);
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
        return RunMatch(compiled, SimplifiedView(haystack));
    };
}

CallbackStringRowPredicate::MatchFn MakeExactly(std::string_view pattern)
{
    if (IsSingleLineAsciiTrim(pattern))
    {
        std::string expected(pattern);
        return [expected = std::move(expected)](std::string_view haystack) {
            if (haystack == expected)
            {
                return true;
            }
            // Simplifying only shrinks ASCII text, so a shorter cell cannot become the expected text. A cell that is
            // already simplified is not equal to it, and only the others need the slow comparison.
            if (haystack.size() < expected.size() || IsSingleLineAsciiTrim(haystack))
            {
                return false;
            }
            return SimplifiedView(haystack) == expected;
        };
    }
    std::string expected = Simplify(pattern);
    return [expected = std::move(expected)](std::string_view haystack) { return SimplifiedView(haystack) == expected; };
}

/**
 * True for ASCII text without a space or a control byte. Simplifying a cell never creates or removes an occurrence of
 * such text: whitespace only collapses between other characters, and a replacement for a bad byte is not ASCII.
 */
bool IsPlainWord(std::string_view bytes) noexcept
{
    return std::ranges::all_of(bytes, [](char ch) {
        const auto byte = static_cast<unsigned char>(ch);
        return byte > ASCII_SPACE && byte < ASCII_DEL;
    });
}

/**
 * Matches the cells whose simplified text contains @p needle, which is already simplified. An edge space of the needle
 * stays.
 */
CallbackStringRowPredicate::MatchFn MakeContainsOfSimplified(std::string needle)
{
    if (IsPlainWord(needle))
    {
        // The raw cell answers the question, with no pass over it to see whether it is simplified.
        return [needle = std::move(needle)](std::string_view haystack) { return haystack.contains(needle); };
    }
    if (IsSingleLineAsciiTrim(needle))
    {
        // A match in the raw cell is also a match after simplifying, because the spaces of the needle are single
        // spaces between other characters. Only a miss in a cell that is not simplified needs the slow path.
        return [needle = std::move(needle)](std::string_view haystack) {
            if (haystack.contains(needle))
            {
                return true;
            }
            return !IsSingleLineAsciiTrim(haystack) && SimplifiedView(haystack).contains(needle);
        };
    }
    return
        [needle = std::move(needle)](std::string_view haystack) { return SimplifiedView(haystack).contains(needle); };
}

CallbackStringRowPredicate::MatchFn MakeContains(std::string_view pattern)
{
    if (IsSingleLineAsciiTrim(pattern))
    {
        return MakeContainsOfSimplified(std::string(pattern));
    }
    // An edge space stays, so " foo " does not match "food".
    return MakeContainsOfSimplified(Simplify(pattern, true));
}

/**
 * The compile error for @p pattern, or nullopt when it compiles.
 */
std::optional<std::string> CompileProblem(std::string_view pattern)
{
    int errcode = 0;
    PCRE2_SIZE erroffset = 0;
    if (internal::CompilePcre2(pattern, PCRE2_UTF, errcode, erroffset) != nullptr)
    {
        return std::nullopt;
    }
    return "invalid pattern '" + std::string(pattern) + "': " + internal::FormatPcre2Error(errcode);
}

} // namespace

std::optional<std::string> StringPatternProblem(std::string_view pattern, LeafRule::Match match)
{
    switch (match)
    {
    case LeafRule::Match::RegularExpression:
        return CompileProblem(DecodeUtf8(pattern));
    case LeafRule::Match::Wildcard:
        return CompileProblem(WildcardToPcre(DecodeUtf8(pattern)));
    case LeafRule::Match::Exactly:
    case LeafRule::Match::Contains:
        break;
    }
    return std::nullopt;
}

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
