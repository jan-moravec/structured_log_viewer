/**
 * @brief Tokens, lexer and value parsing for the query language.
 *
 * The advanced filter text is split into tokens by Lexer. The helpers here also read numbers and ISO 8601 timestamps,
 * which the parser and the printer's quoting rules both need. Only query_parser.cpp and query_printer.cpp include it.
 *
 * This is part of the query module.
 */
#pragma once

#include "loglib/query/query_parser.hpp"
#include "loglib/time/time_zone_context.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <limits>
#include <optional>
#include <ratio>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace loglib::internal::query
{

// The constants below name the digit positions, ranges and scale factors of ISO 8601 timestamps, so the parsing code
// reads without bare numbers. They are grouped by what they describe.

/** @brief The length of a date written as YYYY-MM-DD, and the positions of its digits and separators. */
inline constexpr std::size_t ISO_DATE_LEN = 10;
inline constexpr std::size_t ISO_YEAR_LEN = 4;
inline constexpr std::size_t ISO_MONTH_DIGIT_POS = 5;
inline constexpr std::size_t ISO_MONTH_LAST_DIGIT_POS = 6;
inline constexpr std::size_t ISO_MONTH_DAY_DASH_POS = 7;
inline constexpr std::size_t ISO_DAY_DIGIT_POS = 8;
inline constexpr std::size_t ISO_DAY_LAST_DIGIT_POS = 9;
/** @brief The length of HH:MM, the shortest time of day that may follow the date. */
inline constexpr std::size_t ISO_TIME_HHMM_LEN = 5;

/** @brief Limits of calendar and clock fields. An upper limit is included unless noted. */
inline constexpr int MONTHS_PER_YEAR = 12;
inline constexpr int FEB_DAYS_LEAP = 29;
inline constexpr int FEB_DAYS_NORMAL = 28;
inline constexpr int HOUR_MAX_INCLUSIVE = 24; // ISO end-of-day 24:00:00
inline constexpr int MINUTE_MAX_INCLUSIVE = 59;
inline constexpr int SECOND_MAX_INCLUSIVE = 60; // ISO leap second :60
inline constexpr int TZ_HOUR_MAX_INCLUSIVE = 23;

/** @brief Factors for converting time units and for calendar arithmetic. */
inline constexpr int DECIMAL_BASE = 10;
inline constexpr int SECONDS_PER_MINUTE = 60;
inline constexpr int SECONDS_PER_HOUR = 3600;
inline constexpr std::int64_t MICROS_PER_SECOND = 1'000'000;
/**
 * @brief The largest denominator kept while reading a fraction of a second. Past it, the value already has microsecond
 * precision and further digits change nothing.
 */
inline constexpr std::int64_t MICROS_DENOM_CAP = 10'000'000;

/**
 * @brief Tokens emitted by the lexer.
 *
 * Ident is a bareword column or value. Quoted carries the decoded body of a quoted string. Regex carries the body of a
 * slash-delimited pattern and is only produced after the tilde operator. Number carries the raw digits. The remaining
 * kinds are keywords and punctuation named after what they match.
 */
enum class TokenKind : std::uint8_t
{
    Ident,
    Quoted,
    Regex,
    Number,
    True,
    False,
    KwAnd,
    KwOr,
    KwNot,
    KwIn,
    Colon,
    Eq,
    Tilde,
    Percent,
    Gt,
    GtEq,
    Lt,
    LtEq,
    LParen,
    RParen,
    LBracket,
    RBracket,
    Comma,
    DotDot,
    End,
};

struct Token
{
    TokenKind kind = TokenKind::End;
    /** @brief The text of an Ident, Quoted, Regex or Number token, with quotes and escapes decoded. */
    std::string text;
    /** @brief The byte position of the token in the input, used in error messages. */
    std::size_t offset = 0;
};

/**
 * @brief True for characters that may continue a bareword. Operator and bracket characters are excluded, so the lexer
 * never has to go back.
 */
[[nodiscard]] inline bool IsIdentChar(char ch) noexcept
{
    return (std::isalnum(static_cast<unsigned char>(ch)) != 0) || ch == '_' || ch == '.' || ch == '-';
}

[[nodiscard]] inline bool IsIdentStart(char ch) noexcept
{
    return (std::isalpha(static_cast<unsigned char>(ch)) != 0) || ch == '_';
}

/** @brief Splits query text into tokens, one at a time. */
class Lexer
{
public:
    explicit Lexer(std::string_view input) noexcept
        : mInput(input)
    {
    }

    /**
     * @brief Returns the next token. After the end of the input it keeps returning End, so a caller can look ahead
     * without special cases.
     */
    [[nodiscard]] std::expected<Token, QueryParseError> Next()
    {
        SkipWhitespace();
        Token tok;
        tok.offset = mPos;
        if (mPos >= mInput.size())
        {
            tok.kind = TokenKind::End;
            return tok;
        }
        const char ch = mInput[mPos];
        if (ch == '"')
        {
            return LexQuoted();
        }
        if (ch == '(')
        {
            ++mPos;
            tok.kind = TokenKind::LParen;
            return tok;
        }
        if (ch == ')')
        {
            ++mPos;
            tok.kind = TokenKind::RParen;
            return tok;
        }
        if (ch == '[')
        {
            ++mPos;
            tok.kind = TokenKind::LBracket;
            return tok;
        }
        if (ch == ']')
        {
            ++mPos;
            tok.kind = TokenKind::RBracket;
            return tok;
        }
        if (ch == ',')
        {
            ++mPos;
            tok.kind = TokenKind::Comma;
            return tok;
        }
        if (ch == ':')
        {
            ++mPos;
            tok.kind = TokenKind::Colon;
            return tok;
        }
        if (ch == '=')
        {
            ++mPos;
            tok.kind = TokenKind::Eq;
            return tok;
        }
        if (ch == '~')
        {
            ++mPos;
            tok.kind = TokenKind::Tilde;
            return tok;
        }
        if (ch == '%')
        {
            ++mPos;
            tok.kind = TokenKind::Percent;
            return tok;
        }
        if (ch == '>')
        {
            ++mPos;
            if (mPos < mInput.size() && mInput[mPos] == '=')
            {
                ++mPos;
                tok.kind = TokenKind::GtEq;
            }
            else
            {
                tok.kind = TokenKind::Gt;
            }
            return tok;
        }
        if (ch == '<')
        {
            ++mPos;
            if (mPos < mInput.size() && mInput[mPos] == '=')
            {
                ++mPos;
                tok.kind = TokenKind::LtEq;
            }
            else
            {
                tok.kind = TokenKind::Lt;
            }
            return tok;
        }
        if (ch == '.' && mPos + 1 < mInput.size() && mInput[mPos + 1] == '.')
        {
            mPos += 2;
            tok.kind = TokenKind::DotDot;
            return tok;
        }
        if (ch == '&' && mPos + 1 < mInput.size() && mInput[mPos + 1] == '&')
        {
            mPos += 2;
            tok.kind = TokenKind::KwAnd;
            return tok;
        }
        if (ch == '|' && mPos + 1 < mInput.size() && mInput[mPos + 1] == '|')
        {
            mPos += 2;
            tok.kind = TokenKind::KwOr;
            return tok;
        }
        if (ch == '!')
        {
            ++mPos;
            tok.kind = TokenKind::KwNot;
            return tok;
        }
        // Signed number literal (leading +/- with a digit) so
        // ranges like [-1..1] parse cleanly. ISO-8601 shape takes
        // precedence so 2024-01-02T00:00:00Z isn't split at -.
        if ((std::isdigit(static_cast<unsigned char>(ch)) != 0) ||
            ((ch == '+' || ch == '-') && mPos + 1 < mInput.size() &&
             std::isdigit(static_cast<unsigned char>(mInput[mPos + 1])) != 0))
        {
            if (auto iso = TryLexIsoTimestamp(); iso.has_value())
            {
                return *iso;
            }
            return LexNumber();
        }
        if (IsIdentStart(ch))
        {
            return LexIdent();
        }
        QueryParseError err;
        err.offset = mPos;
        err.message = std::string("unexpected character '") + ch + "'";
        return std::unexpected(err);
    }

    /**
     * @brief Returns the next token as a slash-delimited regular expression. The parser calls it after a tilde, which
     * keeps the slash an error everywhere else.
     */
    [[nodiscard]] std::expected<Token, QueryParseError> NextRegex()
    {
        SkipWhitespace();
        Token tok;
        tok.offset = mPos;
        if (mPos >= mInput.size() || mInput[mPos] != '/')
        {
            QueryParseError err;
            err.offset = mPos;
            err.message = "expected '/' to open regex literal";
            return std::unexpected(err);
        }
        ++mPos;
        std::string body;
        while (mPos < mInput.size())
        {
            const char ch = mInput[mPos];
            if (ch == '\\' && mPos + 1 < mInput.size())
            {
                // Preserve escapes verbatim (the regex engine
                // decodes them). Only \/ collapses to /.
                const char next = mInput[mPos + 1];
                if (next == '/')
                {
                    body.push_back('/');
                }
                else
                {
                    body.push_back('\\');
                    body.push_back(next);
                }
                mPos += 2;
                continue;
            }
            if (ch == '/')
            {
                ++mPos;
                tok.kind = TokenKind::Regex;
                tok.text = std::move(body);
                return tok;
            }
            body.push_back(ch);
            ++mPos;
        }
        QueryParseError err;
        err.offset = mPos;
        err.message = "unterminated regex literal";
        return std::unexpected(err);
    }

    [[nodiscard]] std::size_t Position() const noexcept
    {
        return mPos;
    }

private:
    void SkipWhitespace() noexcept
    {
        while (mPos < mInput.size() && (std::isspace(static_cast<unsigned char>(mInput[mPos])) != 0))
        {
            ++mPos;
        }
    }

    [[nodiscard]] std::expected<Token, QueryParseError> LexQuoted()
    {
        Token tok;
        tok.offset = mPos;
        tok.kind = TokenKind::Quoted;
        ++mPos; // consume opening "
        std::string body;
        while (mPos < mInput.size())
        {
            const char ch = mInput[mPos];
            if (ch == '\\' && mPos + 1 < mInput.size())
            {
                const char next = mInput[mPos + 1];
                switch (next)
                {
                case '"':
                    body.push_back('"');
                    break;
                case '\\':
                    body.push_back('\\');
                    break;
                case 'n':
                    body.push_back('\n');
                    break;
                case 'r':
                    body.push_back('\r');
                    break;
                case 't':
                    body.push_back('\t');
                    break;
                default:
                    body.push_back(next);
                    break;
                }
                mPos += 2;
                continue;
            }
            if (ch == '"')
            {
                ++mPos;
                tok.text = std::move(body);
                return tok;
            }
            body.push_back(ch);
            ++mPos;
        }
        QueryParseError err;
        err.offset = mPos;
        err.message = "unterminated string literal";
        return std::unexpected(err);
    }

    /**
     * @brief Reads an ISO 8601 timestamp as one Ident token.
     *
     * It accepts the same shapes as ParseIsoTimestamp: a date alone, or a date and time, with an optional fraction and
     * zone. It returns nullopt if the text is not one, and the caller then tries a number. It can throw, because it
     * builds a string, and the exception reaches the parser like any other error.
     */
    [[nodiscard]] std::optional<Token> TryLexIsoTimestamp()
    {
        // Need at least YYYY-MM-DD = 10 bytes.
        if (mPos + ISO_DATE_LEN > mInput.size())
        {
            return std::nullopt;
        }
        for (std::size_t i = 0; i < ISO_YEAR_LEN; ++i)
        {
            if (std::isdigit(static_cast<unsigned char>(mInput[mPos + i])) == 0)
            {
                return std::nullopt;
            }
        }
        if (mInput[mPos + ISO_YEAR_LEN] != '-')
        {
            return std::nullopt;
        }
        for (std::size_t i = ISO_MONTH_DIGIT_POS; i <= ISO_MONTH_LAST_DIGIT_POS; ++i)
        {
            if (std::isdigit(static_cast<unsigned char>(mInput[mPos + i])) == 0)
            {
                return std::nullopt;
            }
        }
        if (mInput[mPos + ISO_MONTH_DAY_DASH_POS] != '-')
        {
            return std::nullopt;
        }
        for (std::size_t i = ISO_DAY_DIGIT_POS; i <= ISO_DAY_LAST_DIGIT_POS; ++i)
        {
            if (std::isdigit(static_cast<unsigned char>(mInput[mPos + i])) == 0)
            {
                return std::nullopt;
            }
        }
        const std::size_t start = mPos;
        std::size_t cursor = mPos + ISO_DATE_LEN;
        // Optional time part: T or   followed by HH:MM(:SS)?(.frac)?(TZ)?.
        if (cursor < mInput.size() && (mInput[cursor] == 'T' || mInput[cursor] == ' '))
        {
            const std::size_t timeStart = cursor;
            ++cursor;
            if (cursor + ISO_TIME_HHMM_LEN > mInput.size() ||
                std::isdigit(static_cast<unsigned char>(mInput[cursor])) == 0 ||
                std::isdigit(static_cast<unsigned char>(mInput[cursor + 1])) == 0 || mInput[cursor + 2] != ':' ||
                std::isdigit(static_cast<unsigned char>(mInput[cursor + 3])) == 0 ||
                std::isdigit(static_cast<unsigned char>(mInput[cursor + 4])) == 0)
            {
                // No time part; rewind so bare-date form still matches.
                cursor = timeStart;
            }
            else
            {
                cursor += ISO_TIME_HHMM_LEN;
                if (cursor + 2 < mInput.size() && mInput[cursor] == ':' &&
                    std::isdigit(static_cast<unsigned char>(mInput[cursor + 1])) != 0 &&
                    std::isdigit(static_cast<unsigned char>(mInput[cursor + 2])) != 0)
                {
                    cursor += 3;
                }
                // A fraction needs a digit, so .. after a bound stays a token.
                if (cursor + 1 < mInput.size() && (mInput[cursor] == '.' || mInput[cursor] == ',') &&
                    std::isdigit(static_cast<unsigned char>(mInput[cursor + 1])) != 0)
                {
                    ++cursor;
                    while (cursor < mInput.size() && (std::isdigit(static_cast<unsigned char>(mInput[cursor])) != 0))
                    {
                        ++cursor;
                    }
                }
                if (cursor < mInput.size())
                {
                    const char tzCh = mInput[cursor];
                    if (tzCh == 'Z')
                    {
                        ++cursor;
                    }
                    else if (tzCh == '+' || tzCh == '-')
                    {
                        ++cursor;
                        while (
                            cursor < mInput.size() &&
                            (mInput[cursor] == ':' || (std::isdigit(static_cast<unsigned char>(mInput[cursor])) != 0)))
                        {
                            ++cursor;
                        }
                    }
                }
            }
        }
        // Reject if the token bleeds into another ident/number char.
        if (cursor < mInput.size() && IsIdentChar(mInput[cursor]) && mInput[cursor] != '-' && mInput[cursor] != '.')
        {
            return std::nullopt;
        }
        Token tok;
        tok.kind = TokenKind::Ident;
        tok.offset = start;
        tok.text = std::string(mInput.substr(start, cursor - start));
        mPos = cursor;
        return tok;
    }

    [[nodiscard]] std::expected<Token, QueryParseError> LexNumber()
    {
        Token tok;
        tok.offset = mPos;
        tok.kind = TokenKind::Number;
        const std::size_t start = mPos;
        if (mInput[mPos] == '+' || mInput[mPos] == '-')
        {
            ++mPos;
        }
        while (mPos < mInput.size() && (std::isdigit(static_cast<unsigned char>(mInput[mPos])) != 0))
        {
            ++mPos;
        }
        // Decimal part -- but reject 1..2 so .. stays a token.
        if (mPos < mInput.size() && mInput[mPos] == '.' && (mPos + 1 >= mInput.size() || mInput[mPos + 1] != '.'))
        {
            ++mPos;
            while (mPos < mInput.size() && (std::isdigit(static_cast<unsigned char>(mInput[mPos])) != 0))
            {
                ++mPos;
            }
        }
        if (mPos < mInput.size() && (mInput[mPos] == 'e' || mInput[mPos] == 'E'))
        {
            const std::size_t expStart = mPos;
            ++mPos;
            if (mPos < mInput.size() && (mInput[mPos] == '+' || mInput[mPos] == '-'))
            {
                ++mPos;
            }
            const std::size_t digitsStart = mPos;
            while (mPos < mInput.size() && (std::isdigit(static_cast<unsigned char>(mInput[mPos])) != 0))
            {
                ++mPos;
            }
            // Reject 1e / 1e+ / 1e- here so the error points
            // at the exponent, not at the start of the token.
            if (mPos == digitsStart)
            {
                QueryParseError err;
                err.offset = expStart;
                err.message = "exponent in numeric literal must include at least one digit";
                return std::unexpected(err);
            }
        }
        tok.text = std::string(mInput.substr(start, mPos - start));
        return tok;
    }

    [[nodiscard]] std::expected<Token, QueryParseError> LexIdent()
    {
        Token tok;
        tok.offset = mPos;
        const std::size_t start = mPos;
        while (mPos < mInput.size() && IsIdentChar(mInput[mPos]))
        {
            ++mPos;
        }
        tok.text = std::string(mInput.substr(start, mPos - start));
        // Case-insensitive keyword mapping.
        std::string lower = tok.text;
        std::ranges::transform(lower, lower.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (lower == "and")
        {
            tok.kind = TokenKind::KwAnd;
        }
        else if (lower == "or")
        {
            tok.kind = TokenKind::KwOr;
        }
        else if (lower == "not")
        {
            tok.kind = TokenKind::KwNot;
        }
        else if (lower == "in")
        {
            tok.kind = TokenKind::KwIn;
        }
        else if (lower == "true")
        {
            tok.kind = TokenKind::True;
        }
        else if (lower == "false")
        {
            tok.kind = TokenKind::False;
        }
        else
        {
            tok.kind = TokenKind::Ident;
        }
        return tok;
    }

    std::string_view mInput;
    std::size_t mPos = 0;
};

/**
 * @brief Reads text as a number for a range bound. Returns nullopt if it is not one, and the range parser then tries
 * an ISO timestamp.
 *
 * It uses strtod because some standard libraries do not implement from_chars for doubles. strtod accepts more than the
 * query grammar allows, so leading spaces and hexadecimal prefixes are rejected first. That keeps forms such as col >
 * 42, col = -1.5 and col IN [1e3..2e3] valid and nothing else.
 */
[[nodiscard]] inline std::optional<double> ParseDouble(std::string_view text)
{
    if (text.empty())
    {
        return std::nullopt;
    }
    // Guard against std::strtod's permissive prefixes: bare
    // whitespace, hex (0x / 0X), or a trailing null in the middle
    // of the range must all fail parse, matching the previous
    // std::from_chars semantics.
    const auto firstChar = static_cast<unsigned char>(text.front());
    if (std::isspace(firstChar) != 0)
    {
        return std::nullopt;
    }
    if (text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
    {
        return std::nullopt;
    }
    if (text.contains('\0'))
    {
        return std::nullopt;
    }
    // std::strtod requires a NUL-terminated buffer; copy into a
    // stack std::string (short in every realistic parser input).
    const std::string nulTerminated(text);
    const char *first = nulTerminated.c_str();
    char *end = nullptr;
    const double value = std::strtod(first, &end);
    if (end == nullptr || end != first + nulTerminated.size())
    {
        return std::nullopt;
    }
    // Some libraries set ERANGE for a result that underflows to a tiny number, which is a valid bound and which the
    // printer writes for an exclusive bound next to zero. Only a result too large for a double is rejected.
    if (!std::isfinite(value))
    {
        return std::nullopt;
    }
    return value;
}

/** @brief True if the text starts with four ASCII digits, as an ISO year does. */
[[nodiscard]] inline bool StartsWithDigitYear(std::string_view text) noexcept
{
    if (text.size() < 4)
    {
        return false;
    }
    for (std::size_t i = 0; i < 4; ++i)
    {
        if (std::isdigit(static_cast<unsigned char>(text[i])) == 0)
        {
            return false;
        }
    }
    return true;
}

/** @brief The number of days in a month of a year (month 1 to 12), by the Gregorian calendar. */
[[nodiscard]] inline int DaysInMonth(int year, int month) noexcept
{
    // Days per month for the non-February cases; February varies
    // with the leap-year check below.
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
    constexpr std::array<int, MONTHS_PER_YEAR> LENGTHS{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > MONTHS_PER_YEAR)
    {
        return 0;
    }
    if (month == 2)
    {
        // Gregorian leap-year rule: divisible by 4 except centuries
        // that aren't divisible by 400.
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
        const bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
        return leap ? FEB_DAYS_LEAP : FEB_DAYS_NORMAL;
    }
    return LENGTHS[static_cast<std::size_t>(month - 1)];
}

/**
 * @brief Reads an ISO 8601 timestamp, with an optional fraction and zone, as microseconds since the epoch.
 *
 * The shape and the calendar ranges are checked strictly, so a typo gives an error and not a date that rolled over.
 */
[[nodiscard]] inline std::optional<std::int64_t> ParseIsoTimestamp(
    std::string_view text, const TimeZoneContext &timeZone
)
{
    if (!StartsWithDigitYear(text))
    {
        return std::nullopt;
    }
    if (text.size() < ISO_DATE_LEN)
    {
        return std::nullopt;
    }
    if (text[ISO_YEAR_LEN] != '-')
    {
        return std::nullopt;
    }

    const auto readInt = [](std::string_view sv, std::size_t start, std::size_t width) -> std::optional<int> {
        if (start + width > sv.size())
        {
            return std::nullopt;
        }
        int out = 0;
        for (std::size_t i = 0; i < width; ++i)
        {
            const char ch = sv[start + i];
            if (std::isdigit(static_cast<unsigned char>(ch)) == 0)
            {
                return std::nullopt;
            }
            out = (out * DECIMAL_BASE) + (ch - '0');
        }
        return out;
    };

    const auto year = readInt(text, 0, ISO_YEAR_LEN);
    const auto month = readInt(text, ISO_MONTH_DIGIT_POS, 2);
    if (!year.has_value() || !month.has_value())
    {
        return std::nullopt;
    }
    if (text.size() < ISO_DATE_LEN || text[ISO_MONTH_DAY_DASH_POS] != '-')
    {
        return std::nullopt;
    }
    const auto day = readInt(text, ISO_DAY_DIGIT_POS, 2);
    if (!day.has_value())
    {
        return std::nullopt;
    }
    // Reject out-of-range fields; the civil-from-fields formula
    // below would silently roll 2024-13-45 over to 2025-02-14.
    if (*month < 1 || *month > MONTHS_PER_YEAR || *day < 1 || *day > DaysInMonth(*year, *month))
    {
        return std::nullopt;
    }

    int hour = 0;
    int minute = 0;
    int second = 0;
    std::int64_t fractionalMicros = 0;
    std::int64_t offsetSeconds = 0;
    bool hasZone = false;
    std::size_t cursor = ISO_DATE_LEN;
    if (cursor < text.size() && (text[cursor] == 'T' || text[cursor] == ' '))
    {
        ++cursor;
        const auto h = readInt(text, cursor, 2);
        if (!h.has_value() || cursor + 2 >= text.size() || text[cursor + 2] != ':')
        {
            return std::nullopt;
        }
        hour = *h;
        cursor += 3;
        const auto m = readInt(text, cursor, 2);
        if (!m.has_value())
        {
            return std::nullopt;
        }
        minute = *m;
        cursor += 2;
        if (cursor < text.size() && text[cursor] == ':')
        {
            ++cursor;
            const auto s = readInt(text, cursor, 2);
            if (!s.has_value())
            {
                return std::nullopt;
            }
            second = *s;
            cursor += 2;
        }
        // Accept ISO end-of-day (24:00:00) and leap-second
        // (:60) -- both roll over cleanly below. Reject anything
        // past 24:00:00 so 24:00:00.5 doesn't silently become
        // 00:00:00.5 of the next day.
        if (hour > HOUR_MAX_INCLUSIVE || minute > MINUTE_MAX_INCLUSIVE || second > SECOND_MAX_INCLUSIVE ||
            (hour == HOUR_MAX_INCLUSIVE && (minute != 0 || second != 0)))
        {
            return std::nullopt;
        }
        if (cursor < text.size() && (text[cursor] == '.' || text[cursor] == ','))
        {
            ++cursor;
            std::int64_t fractional = 0;
            std::int64_t denom = 1;
            while (cursor < text.size() && (std::isdigit(static_cast<unsigned char>(text[cursor])) != 0) &&
                   denom < MICROS_DENOM_CAP)
            {
                fractional = (fractional * DECIMAL_BASE) + (text[cursor] - '0');
                denom *= DECIMAL_BASE;
                ++cursor;
            }
            // Ignore any trailing digits past microsecond precision.
            while (cursor < text.size() && (std::isdigit(static_cast<unsigned char>(text[cursor])) != 0))
            {
                ++cursor;
            }
            // Scale fractional up to microseconds.
            fractionalMicros = (fractional * MICROS_PER_SECOND) / denom;
            // Same reason as above: reject 24:00:00.5.
            if (hour == HOUR_MAX_INCLUSIVE && fractionalMicros != 0)
            {
                return std::nullopt;
            }
        }
        // Optional timezone: Z, +HH:MM, +HHMM, -HH:MM, -HHMM.
        if (cursor < text.size())
        {
            const char tzCh = text[cursor];
            if (tzCh == 'Z')
            {
                hasZone = true;
                ++cursor;
            }
            else if (tzCh == '+' || tzCh == '-')
            {
                hasZone = true;
                const int sign = (tzCh == '-') ? -1 : 1;
                ++cursor;
                const auto tzH = readInt(text, cursor, 2);
                if (!tzH.has_value())
                {
                    return std::nullopt;
                }
                cursor += 2;
                int tzM = 0;
                if (cursor < text.size() && text[cursor] == ':')
                {
                    // +HH:MM: minutes are mandatory after :.
                    // Rejects +00: and +00:0.
                    ++cursor;
                    const auto tzMinutes = readInt(text, cursor, 2);
                    if (!tzMinutes.has_value())
                    {
                        return std::nullopt;
                    }
                    tzM = *tzMinutes;
                    cursor += 2;
                }
                else if (cursor + 1 < text.size() && (std::isdigit(static_cast<unsigned char>(text[cursor])) != 0))
                {
                    // +HHMM compact form: two more digits.
                    const auto tzMinutes = readInt(text, cursor, 2);
                    if (!tzMinutes.has_value())
                    {
                        return std::nullopt;
                    }
                    tzM = *tzMinutes;
                    cursor += 2;
                }
                if (*tzH > TZ_HOUR_MAX_INCLUSIVE || tzM > MINUTE_MAX_INCLUSIVE)
                {
                    return std::nullopt;
                }
                offsetSeconds = sign * ((*tzH * SECONDS_PER_HOUR) + (tzM * SECONDS_PER_MINUTE));
            }
        }
    }
    if (cursor != text.size())
    {
        return std::nullopt;
    }
    // Civil-from-fields via Howard Hinnant's date algorithms; no
    // TZ database dependency.
    const long long y = *year - ((*month <= 2) ? 1 : 0);
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = static_cast<unsigned long long>(y - (era * 400));
    const long long m = *month;
    const auto doy = static_cast<unsigned long long>((((153LL * (m + ((m > 2) ? -3 : 9))) + 2) / 5) + (*day - 1));
    const unsigned long long doe = (yoe * 365) + (yoe / 4) - (yoe / 100) + doy;
    const long long daysSinceEpoch = (era * 146097) + static_cast<long long>(doe) - 719468;
    const long long secondsSinceEpoch = (daysSinceEpoch * 86400LL) + (static_cast<long long>(hour) * 3600LL) +
                                        (static_cast<long long>(minute) * 60LL) + static_cast<long long>(second) -
                                        offsetSeconds;
    // Bare-date: hour/minute/second stay 0 = midnight.
    const std::int64_t micros = (secondsSinceEpoch * 1'000'000LL) + fractionalMicros;
    return hasZone ? micros : timeZone.LocalMicrosecondsToUtc(micros);
}

} // namespace loglib::internal::query
