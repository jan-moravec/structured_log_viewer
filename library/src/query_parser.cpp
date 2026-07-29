#include "loglib/query_parser.hpp"

#include "loglib/filter_expression.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <ratio>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace loglib
{

namespace
{

/// Tokens emitted by the lexer.
enum class TokenKind : std::uint8_t
{
    Ident,      ///< bareword identifier (column or value)
    Quoted,     ///< "quoted string" (`text` = decoded body)
    Regex,      ///< /regex body/ (only produced after `~`)
    Number,     ///< numeric literal (`text` = raw digits)
    True,
    False,
    KwAnd,
    KwOr,
    KwNot,
    KwIn,
    Colon,      ///< `:`
    Eq,         ///< `=`
    Tilde,      ///< `~`
    Percent,    ///< `%`
    Gt,         ///< `>`
    GtEq,       ///< `>=`
    Lt,         ///< `<`
    LtEq,       ///< `<=`
    LParen,     ///< `(`
    RParen,     ///< `)`
    LBracket,   ///< `[`
    RBracket,   ///< `]`
    Comma,      ///< `,`
    DotDot,     ///< `..`
    End,
};

struct Token
{
    TokenKind kind = TokenKind::End;
    /// Decoded text (Ident/Quoted/Regex/Number).
    std::string text;
    /// Byte offset into the input; used for error reporting.
    std::size_t offset = 0;
};

/// Ident-body character set; excludes operator/bracket chars so
/// the lexer never needs to backtrack.
[[nodiscard]] bool IsIdentChar(char ch) noexcept
{
    return (std::isalnum(static_cast<unsigned char>(ch)) != 0) || ch == '_' || ch == '.' || ch == '-';
}

[[nodiscard]] bool IsIdentStart(char ch) noexcept
{
    return (std::isalpha(static_cast<unsigned char>(ch)) != 0) || ch == '_';
}

class Lexer
{
public:
    explicit Lexer(std::string_view input) noexcept : mInput(input)
    {
    }

    /// Consume the next token; keeps returning `End` past EOI so
    /// callers can peek without special-casing.
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
        // Signed number literal (leading `+`/`-` with a digit) so
        // ranges like `[-1..1]` parse cleanly. ISO-8601 shape takes
        // precedence so `2024-01-02T00:00:00Z` isn't split at `-`.
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

    /// Dedicated entry called after the parser sees a `~`. Keeps
    /// `/` free to mean "regex delimiter" here and stay a syntax
    /// error everywhere else.
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
                // decodes them). Only `\/` collapses to `/`.
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

    /// Try to consume an ISO-8601 timestamp as a single Ident token.
    /// Same shape family as `ParseIsoTimestamp` (bare date,
    /// date+T+time, optional fraction / TZ). Returns `nullopt` when
    /// no match, so the caller falls back to `LexNumber`.
    [[nodiscard]] std::optional<Token> TryLexIsoTimestamp() noexcept
    {
        // Need at least `YYYY-MM-DD` = 10 bytes.
        if (mPos + 10 > mInput.size())
        {
            return std::nullopt;
        }
        for (std::size_t i = 0; i < 4; ++i)
        {
            if (std::isdigit(static_cast<unsigned char>(mInput[mPos + i])) == 0)
            {
                return std::nullopt;
            }
        }
        if (mInput[mPos + 4] != '-')
        {
            return std::nullopt;
        }
        for (std::size_t i = 5; i <= 6; ++i)
        {
            if (std::isdigit(static_cast<unsigned char>(mInput[mPos + i])) == 0)
            {
                return std::nullopt;
            }
        }
        if (mInput[mPos + 7] != '-')
        {
            return std::nullopt;
        }
        for (std::size_t i = 8; i <= 9; ++i)
        {
            if (std::isdigit(static_cast<unsigned char>(mInput[mPos + i])) == 0)
            {
                return std::nullopt;
            }
        }
        const std::size_t start = mPos;
        std::size_t cursor = mPos + 10;
        // Optional time part: `T` or ` ` followed by `HH:MM(:SS)?(.frac)?(TZ)?`.
        if (cursor < mInput.size() && (mInput[cursor] == 'T' || mInput[cursor] == ' '))
        {
            const std::size_t timeStart = cursor;
            ++cursor;
            if (cursor + 5 > mInput.size() ||
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
                cursor += 5;
                if (cursor + 2 < mInput.size() && mInput[cursor] == ':' &&
                    std::isdigit(static_cast<unsigned char>(mInput[cursor + 1])) != 0 &&
                    std::isdigit(static_cast<unsigned char>(mInput[cursor + 2])) != 0)
                {
                    cursor += 3;
                }
                if (cursor < mInput.size() && (mInput[cursor] == '.' || mInput[cursor] == ','))
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
                        while (cursor < mInput.size() && (mInput[cursor] == ':' ||
                                                          (std::isdigit(static_cast<unsigned char>(mInput[cursor])) != 0)))
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
        // Decimal part -- but reject `1..2` so `..` stays a token.
        if (mPos < mInput.size() && mInput[mPos] == '.' &&
            (mPos + 1 >= mInput.size() || mInput[mPos + 1] != '.'))
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
            // Reject `1e` / `1e+` / `1e-` here so the error points
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

/// Parse @p text as a numeric range bound. `nullopt` when malformed;
/// the range-parser then re-tries the bound as an ISO timestamp.
[[nodiscard]] std::optional<double> ParseDouble(std::string_view text) noexcept
{
    if (text.empty())
    {
        return std::nullopt;
    }
    double value = 0.0;
    const char *first = text.data();
    const char *last = text.data() + text.size();
    const auto result = std::from_chars(first, last, value);
    if (result.ec != std::errc{} || result.ptr != last)
    {
        return std::nullopt;
    }
    return value;
}

/// True iff @p text starts with four ASCII digits (an ISO year).
[[nodiscard]] bool StartsWithDigitYear(std::string_view text) noexcept
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

/// Proleptic-Gregorian length of @p month (1-based) in @p year.
[[nodiscard]] int DaysInMonth(int year, int month) noexcept
{
    constexpr std::array<int, 12> LENGTHS{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12)
    {
        return 0;
    }
    if (month == 2)
    {
        const bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
        return leap ? 29 : 28;
    }
    return LENGTHS[static_cast<std::size_t>(month - 1)];
}

/// Parse an ISO-8601 timestamp (optional fraction and TZ) into
/// microseconds since the UNIX epoch. Strict on shape and calendar
/// ranges so typos surface as parse errors, not rolled-over dates.
[[nodiscard]] std::optional<std::int64_t> ParseIsoTimestamp(std::string_view text)
{
    if (!StartsWithDigitYear(text))
    {
        return std::nullopt;
    }
    if (text.size() < 10)
    {
        return std::nullopt;
    }
    if (text[4] != '-')
    {
        return std::nullopt;
    }

    auto readInt = [](std::string_view sv, std::size_t start, std::size_t width) -> std::optional<int> {
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
            out = (out * 10) + (ch - '0');
        }
        return out;
    };

    const auto year = readInt(text, 0, 4);
    const auto month = readInt(text, 5, 2);
    if (!year.has_value() || !month.has_value())
    {
        return std::nullopt;
    }
    if (text.size() < 10 || text[7] != '-')
    {
        return std::nullopt;
    }
    const auto day = readInt(text, 8, 2);
    if (!day.has_value())
    {
        return std::nullopt;
    }
    // Reject out-of-range fields; the civil-from-fields formula
    // below would silently roll `2024-13-45` over to `2025-02-14`.
    if (*month < 1 || *month > 12 || *day < 1 || *day > DaysInMonth(*year, *month))
    {
        return std::nullopt;
    }

    int hour = 0;
    int minute = 0;
    int second = 0;
    std::int64_t fractionalMicros = 0;
    std::int64_t offsetSeconds = 0;
    std::size_t cursor = 10;
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
        // Accept ISO end-of-day (`24:00:00`) and leap-second
        // (`:60`) -- both roll over cleanly below. Reject anything
        // past `24:00:00` so `24:00:00.5` doesn't silently become
        // `00:00:00.5` of the next day.
        if (hour > 24 || minute > 59 || second > 60 ||
            (hour == 24 && (minute != 0 || second != 0)))
        {
            return std::nullopt;
        }
        if (cursor < text.size() && (text[cursor] == '.' || text[cursor] == ','))
        {
            ++cursor;
            std::int64_t fractional = 0;
            std::int64_t denom = 1;
            while (cursor < text.size() && (std::isdigit(static_cast<unsigned char>(text[cursor])) != 0) &&
                   denom < 10'000'000)
            {
                fractional = (fractional * 10) + (text[cursor] - '0');
                denom *= 10;
                ++cursor;
            }
            // Ignore any trailing digits past microsecond precision.
            while (cursor < text.size() && (std::isdigit(static_cast<unsigned char>(text[cursor])) != 0))
            {
                ++cursor;
            }
            // Scale fractional up to microseconds.
            fractionalMicros = (fractional * 1'000'000) / denom;
            // Same reason as above: reject `24:00:00.5`.
            if (hour == 24 && fractionalMicros != 0)
            {
                return std::nullopt;
            }
        }
        // Optional timezone: `Z`, `+HH:MM`, `+HHMM`, `-HH:MM`, `-HHMM`.
        if (cursor < text.size())
        {
            const char tzCh = text[cursor];
            if (tzCh == 'Z')
            {
                ++cursor;
            }
            else if (tzCh == '+' || tzCh == '-')
            {
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
                    // `+HH:MM`: minutes are mandatory after `:`.
                    // Rejects `+00:` and `+00:0`.
                    ++cursor;
                    const auto tzMinutes = readInt(text, cursor, 2);
                    if (!tzMinutes.has_value())
                    {
                        return std::nullopt;
                    }
                    tzM = *tzMinutes;
                    cursor += 2;
                }
                else if (cursor + 1 < text.size() &&
                         (std::isdigit(static_cast<unsigned char>(text[cursor])) != 0))
                {
                    // `+HHMM` compact form: two more digits.
                    const auto tzMinutes = readInt(text, cursor, 2);
                    if (!tzMinutes.has_value())
                    {
                        return std::nullopt;
                    }
                    tzM = *tzMinutes;
                    cursor += 2;
                }
                if (*tzH > 23 || tzM > 59)
                {
                    return std::nullopt;
                }
                offsetSeconds = sign * ((*tzH * 3600) + (tzM * 60));
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
    const unsigned long long doy =
        static_cast<unsigned long long>((((153LL * (m + ((m > 2) ? -3 : 9))) + 2) / 5) + (*day - 1));
    const unsigned long long doe = (yoe * 365) + (yoe / 4) - (yoe / 100) + doy;
    const long long daysSinceEpoch = (era * 146097) + static_cast<long long>(doe) - 719468;
    const long long secondsSinceEpoch = (daysSinceEpoch * 86400LL) +
                                        (static_cast<long long>(hour) * 3600LL) +
                                        (static_cast<long long>(minute) * 60LL) + static_cast<long long>(second) -
                                        offsetSeconds;
    // Bare-date: `hour`/`minute`/`second` stay 0 = midnight UTC.
    const std::int64_t micros = (secondsSinceEpoch * 1'000'000LL) + fractionalMicros;
    return micros;
}

/// ASCII-only case-insensitive equality; the keyword set is closed
/// and stays on the ASCII plane.
[[nodiscard]] bool EqualsIgnoreCaseAscii(std::string_view lhs, std::string_view rhs) noexcept
{
    if (lhs.size() != rhs.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i)
    {
        const auto lc = std::tolower(static_cast<unsigned char>(lhs[i]));
        const auto rc = std::tolower(static_cast<unsigned char>(rhs[i]));
        if (lc != rc)
        {
            return false;
        }
    }
    return true;
}

/// True iff @p text needs quoting to survive the round-trip
/// (starts with a non-ident char, contains operator chars, or
/// happens to be a reserved keyword).
[[nodiscard]] bool NeedsQuoting(std::string_view text) noexcept
{
    if (text.empty())
    {
        return true;
    }
    if (!IsIdentStart(text.front()))
    {
        return true;
    }
    for (const char ch : text)
    {
        if (!IsIdentChar(ch))
        {
            return true;
        }
    }
    // Reserved keywords must be quoted so they don't relex as tokens.
    static constexpr std::array<std::string_view, 6> RESERVED{
        "and", "or", "not", "in", "true", "false"
    };
    return std::ranges::any_of(RESERVED, [text](std::string_view keyword) {
        return EqualsIgnoreCaseAscii(text, keyword);
    });
}

[[nodiscard]] std::string QuoteString(std::string_view text)
{
    std::string out;
    out.reserve(text.size() + 2);
    out.push_back('"');
    for (const char ch : text)
    {
        switch (ch)
        {
        case '"':
            out.append("\\\"");
            break;
        case '\\':
            out.append("\\\\");
            break;
        case '\n':
            out.append("\\n");
            break;
        case '\r':
            out.append("\\r");
            break;
        case '\t':
            out.append("\\t");
            break;
        default:
            out.push_back(ch);
            break;
        }
    }
    out.push_back('"');
    return out;
}

[[nodiscard]] std::string FormatIdent(std::string_view text)
{
    return NeedsQuoting(text) ? QuoteString(text) : std::string(text);
}

/// Emit a numeric bound with round-trip precision (via
/// `std::to_chars`, locale-free).
[[nodiscard]] std::string FormatNumber(double value)
{
    // 64 bytes fits any `to_chars(double)` shortest form with slack.
    std::array<char, 64> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (result.ec != std::errc{})
    {
        return std::to_string(value);
    }
    return std::string(buffer.data(), static_cast<std::size_t>(result.ptr - buffer.data()));
}

/// Calendar fields for a day count relative to the UNIX epoch.
struct CivilDate
{
    std::int64_t year = 0;
    unsigned month = 1;
    unsigned day = 1;
};

/// `civil_from_days` (Howard Hinnant's date algorithms): exact
/// inverse of the `days_from_civil` arithmetic used above, valid
/// across the whole `int64_t` range including negative days.
[[nodiscard]] CivilDate CivilFromDays(std::int64_t z) noexcept
{
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const auto doe = static_cast<std::uint64_t>(z - (era * 146097));           // [0, 146096]
    const std::uint64_t yoe =                                                  // [0, 399]
        (doe - (doe / 1460) + (doe / 36524) - (doe / 146096)) / 365;
    const std::int64_t y = static_cast<std::int64_t>(yoe) + (era * 400);
    const std::uint64_t doy = doe - ((365 * yoe) + (yoe / 4) - (yoe / 100));    // [0, 365]
    const std::uint64_t mp = ((5 * doy) + 2) / 153;                             // [0, 11]
    const auto d = static_cast<unsigned>(doy - (((153 * mp) + 2) / 5) + 1);     // [1, 31]
    // Shift `mp` (March-based) back to a January-based month.
    // Signed arithmetic; the canonical unsigned-wrap form works but
    // is subtle.
    const auto marchBased = static_cast<int>(mp);
    const auto m = static_cast<unsigned>(marchBased + (marchBased < 10 ? 3 : -9)); // [1, 12]
    return CivilDate{.year = y + (m <= 2 ? 1 : 0), .month = m, .day = d};
}

/// Render epoch microseconds as `YYYY-MM-DDTHH:MM:SS.uuuuuuZ`.
/// Uses the same civil-date arithmetic as `ParseIsoTimestamp` so
/// the two are exact inverses on every platform, including
/// pre-epoch values (MSVC `gmtime_s` returns `EINVAL` below
/// 1969-12-31T12:00:00Z, hence the manual arithmetic).
///
/// Years outside `[0, 9999]` (no plain ISO spelling in this
/// grammar) fall back to an `epoch_micros:<n>` marker. That
/// marker is deliberately not re-parseable, so a corrupt/synthetic
/// far-out bound is visible instead of silently mis-rendered.
[[nodiscard]] std::string FormatTimestampMicros(std::int64_t micros)
{
    // C++ integer division truncates towards zero, so negative
    // `micros` with a non-zero remainder need to borrow a whole
    // second (otherwise `-500'000` renders as
    // `1970-01-01T00:00:00.500000Z` instead of the correct
    // `1969-12-31T23:59:59.500000Z`.
    std::int64_t totalSeconds = micros / 1'000'000;
    std::int64_t microRemainder = micros % 1'000'000;
    if (microRemainder < 0)
    {
        totalSeconds -= 1;
        microRemainder += 1'000'000;
    }

    // Floor-divide into days + seconds-of-day so pre-epoch values
    // land on the previous day rather than truncating towards zero.
    constexpr std::int64_t SECONDS_PER_DAY = 86'400;
    std::int64_t days = totalSeconds / SECONDS_PER_DAY;
    std::int64_t secondOfDay = totalSeconds % SECONDS_PER_DAY;
    if (secondOfDay < 0)
    {
        days -= 1;
        secondOfDay += SECONDS_PER_DAY;
    }

    const CivilDate date = CivilFromDays(days);
    if (date.year < 0 || date.year > 9999)
    {
        return "epoch_micros:" + std::to_string(micros);
    }

    const auto hour = static_cast<int>(secondOfDay / 3600);
    const auto minute = static_cast<int>((secondOfDay % 3600) / 60);
    const auto second = static_cast<int>(secondOfDay % 60);

    std::array<char, 40> buffer{};
    const int written = std::snprintf(
        buffer.data(),
        buffer.size(),
        "%04d-%02u-%02uT%02d:%02d:%02d.%06dZ",
        static_cast<int>(date.year),
        date.month,
        date.day,
        hour,
        minute,
        second,
        static_cast<int>(microRemainder)
    );
    if (written <= 0)
    {
        return "epoch_micros:" + std::to_string(micros);
    }
    return std::string(buffer.data(), static_cast<std::size_t>(written));
}

// ---- Parser ----------------------------------------------------------------

class Parser
{
public:
    explicit Parser(std::string_view input) : mLexer(input), mInput(input)
    {
    }

    [[nodiscard]] std::expected<FilterExpression, QueryParseError> Parse()
    {
        if (auto first = mLexer.Next(); first.has_value())
        {
            mLookahead = std::move(*first);
        }
        else
        {
            return std::unexpected(first.error());
        }
        if (mLookahead.kind == TokenKind::End)
        {
            // Empty input -> "match all" default.
            return FilterExpression{};
        }
        auto result = ParseOr();
        if (!result.has_value())
        {
            return result;
        }
        if (mLookahead.kind != TokenKind::End)
        {
            QueryParseError err;
            err.offset = mLookahead.offset;
            err.message = "unexpected trailing tokens after expression";
            return std::unexpected(err);
        }
        return result;
    }

private:
    /// Advance one token; the just-consumed token is returned.
    [[nodiscard]] std::expected<Token, QueryParseError> Advance()
    {
        Token consumed = std::move(mLookahead);
        auto next = mLexer.Next();
        if (!next.has_value())
        {
            return std::unexpected(next.error());
        }
        mLookahead = std::move(*next);
        return consumed;
    }

    [[nodiscard]] std::expected<Token, QueryParseError> AdvanceRegex()
    {
        Token consumed = std::move(mLookahead);
        auto next = mLexer.NextRegex();
        if (!next.has_value())
        {
            return std::unexpected(next.error());
        }
        mLookahead = std::move(*next);
        return consumed;
    }

    /// `or_expr := and_expr ( 'OR' and_expr )*`
    [[nodiscard]] std::expected<FilterExpression, QueryParseError> ParseOr()
    {
        auto first = ParseAnd();
        if (!first.has_value())
        {
            return first;
        }
        std::vector<FilterExpression> children;
        children.push_back(std::move(*first));
        while (mLookahead.kind == TokenKind::KwOr)
        {
            if (auto ok = Advance(); !ok.has_value())
            {
                return std::unexpected(ok.error());
            }
            auto next = ParseAnd();
            if (!next.has_value())
            {
                return next;
            }
            children.push_back(std::move(*next));
        }
        if (children.size() == 1)
        {
            return std::move(children.front());
        }
        return MakeOr(std::move(children));
    }

    /// `and_expr := not_expr ( ('AND' | epsilon) not_expr )*`
    /// Implicit-AND fires when the next token can start a new atom.
    [[nodiscard]] std::expected<FilterExpression, QueryParseError> ParseAnd()
    {
        auto first = ParseNot();
        if (!first.has_value())
        {
            return first;
        }
        std::vector<FilterExpression> children;
        children.push_back(std::move(*first));
        while (true)
        {
            if (mLookahead.kind == TokenKind::KwAnd)
            {
                if (auto ok = Advance(); !ok.has_value())
                {
                    return std::unexpected(ok.error());
                }
                auto next = ParseNot();
                if (!next.has_value())
                {
                    return next;
                }
                children.push_back(std::move(*next));
                continue;
            }
            // Implicit-AND: only if the next token can start an atom.
            // Terminators (`)` `]` `,` End `OR`) close the chain.
            switch (mLookahead.kind)
            {
            case TokenKind::Ident:
            case TokenKind::Quoted:
            case TokenKind::KwNot:
            case TokenKind::LParen:
            {
                auto next = ParseNot();
                if (!next.has_value())
                {
                    return next;
                }
                children.push_back(std::move(*next));
                continue;
            }
            default:
                break;
            }
            break;
        }
        if (children.size() == 1)
        {
            return std::move(children.front());
        }
        return MakeAnd(std::move(children));
    }

    /// `not_expr := 'NOT' not_expr | atom`
    [[nodiscard]] std::expected<FilterExpression, QueryParseError> ParseNot()
    {
        if (mLookahead.kind == TokenKind::KwNot)
        {
            if (auto ok = Advance(); !ok.has_value())
            {
                return std::unexpected(ok.error());
            }
            auto inner = ParseNot();
            if (!inner.has_value())
            {
                return inner;
            }
            return MakeNot(std::move(*inner));
        }
        return ParseAtom();
    }

    /// `atom := '(' or_expr ')' | leaf`
    [[nodiscard]] std::expected<FilterExpression, QueryParseError> ParseAtom()
    {
        if (mLookahead.kind == TokenKind::LParen)
        {
            if (auto ok = Advance(); !ok.has_value())
            {
                return std::unexpected(ok.error());
            }
            auto inner = ParseOr();
            if (!inner.has_value())
            {
                return inner;
            }
            if (mLookahead.kind != TokenKind::RParen)
            {
                QueryParseError err;
                err.offset = mLookahead.offset;
                err.message = "expected ')' to close group";
                return std::unexpected(err);
            }
            if (auto ok = Advance(); !ok.has_value())
            {
                return std::unexpected(ok.error());
            }
            return inner;
        }
        return ParseLeaf();
    }

    /// `leaf := column op value | column 'in' value_list`
    [[nodiscard]] std::expected<FilterExpression, QueryParseError> ParseLeaf()
    {
        // Column: bareword or quoted string.
        if (mLookahead.kind != TokenKind::Ident && mLookahead.kind != TokenKind::Quoted)
        {
            QueryParseError err;
            err.offset = mLookahead.offset;
            err.message = "expected a column name to start the leaf";
            return std::unexpected(err);
        }
        auto columnTok = Advance();
        if (!columnTok.has_value())
        {
            return std::unexpected(columnTok.error());
        }
        LeafRule rule;
        rule.columnKeys.push_back(columnTok->text);

        // Operator.
        const Token opTok = mLookahead;
        switch (opTok.kind)
        {
        case TokenKind::Colon:
            if (auto ok = Advance(); !ok.has_value())
            {
                return std::unexpected(ok.error());
            }
            return FinishStringLeaf(std::move(rule), LeafRule::Match::Contains);
        case TokenKind::Eq:
            if (auto ok = Advance(); !ok.has_value())
            {
                return std::unexpected(ok.error());
            }
            return FinishEqLeaf(std::move(rule));
        case TokenKind::Tilde:
        {
            if (auto ok = AdvanceRegex(); !ok.has_value())
            {
                return std::unexpected(ok.error());
            }
            if (mLookahead.kind != TokenKind::Regex)
            {
                QueryParseError err;
                err.offset = mLookahead.offset;
                err.message = "expected a regex literal '/.../' after '~'";
                return std::unexpected(err);
            }
            rule.type = LeafRule::Type::String;
            rule.matchType = LeafRule::Match::RegularExpression;
            rule.filterString = mLookahead.text;
            if (auto ok = Advance(); !ok.has_value())
            {
                return std::unexpected(ok.error());
            }
            return MakeLeaf(std::move(rule));
        }
        case TokenKind::Percent:
        {
            if (auto ok = Advance(); !ok.has_value())
            {
                return std::unexpected(ok.error());
            }
            if (mLookahead.kind != TokenKind::Quoted && mLookahead.kind != TokenKind::Ident)
            {
                QueryParseError err;
                err.offset = mLookahead.offset;
                err.message = "expected a pattern after '%' (wildcard operator)";
                return std::unexpected(err);
            }
            rule.type = LeafRule::Type::String;
            rule.matchType = LeafRule::Match::Wildcard;
            rule.filterString = mLookahead.text;
            if (auto ok = Advance(); !ok.has_value())
            {
                return std::unexpected(ok.error());
            }
            return MakeLeaf(std::move(rule));
        }
        case TokenKind::Gt:
        case TokenKind::GtEq:
        case TokenKind::Lt:
        case TokenKind::LtEq:
            if (auto ok = Advance(); !ok.has_value())
            {
                return std::unexpected(ok.error());
            }
            return FinishCompareLeaf(std::move(rule), opTok);
        case TokenKind::KwIn:
            if (auto ok = Advance(); !ok.has_value())
            {
                return std::unexpected(ok.error());
            }
            return FinishInLeaf(std::move(rule));
        case TokenKind::KwNot:
        {
            // Sugar for `column NOT IN [...]` -> `NOT (column IN [...])`.
            // Requires `IN` immediately after `NOT`; anything else is a
            // parse error so `NOT` at the leaf position stays unambiguous.
            if (auto ok = Advance(); !ok.has_value())
            {
                return std::unexpected(ok.error());
            }
            if (mLookahead.kind != TokenKind::KwIn)
            {
                QueryParseError err;
                err.offset = mLookahead.offset;
                err.message = "expected 'IN' after 'NOT' in leaf position";
                return std::unexpected(err);
            }
            if (auto ok = Advance(); !ok.has_value())
            {
                return std::unexpected(ok.error());
            }
            auto inner = FinishInLeaf(std::move(rule));
            if (!inner.has_value())
            {
                return inner;
            }
            return MakeNot(std::move(*inner));
        }
        default:
        {
            QueryParseError err;
            err.offset = opTok.offset;
            err.message = "expected an operator after the column name (':', '=', '~', '%', '<', '>', 'in')";
            return std::unexpected(err);
        }
        }
    }

    [[nodiscard]] std::expected<FilterExpression, QueryParseError>
    FinishStringLeaf(LeafRule rule, LeafRule::Match matchType)
    {
        if (mLookahead.kind != TokenKind::Quoted && mLookahead.kind != TokenKind::Ident &&
            mLookahead.kind != TokenKind::Number && mLookahead.kind != TokenKind::True &&
            mLookahead.kind != TokenKind::False)
        {
            QueryParseError err;
            err.offset = mLookahead.offset;
            err.message = "expected a value after ':' operator";
            return std::unexpected(err);
        }
        rule.type = LeafRule::Type::String;
        rule.matchType = matchType;
        rule.filterString = mLookahead.text;
        if (auto ok = Advance(); !ok.has_value())
        {
            return std::unexpected(ok.error());
        }
        return MakeLeaf(std::move(rule));
    }

    [[nodiscard]] std::expected<FilterExpression, QueryParseError> FinishEqLeaf(LeafRule rule)
    {
        // Payload picks the concrete leaf type:
        //   "text"    -> String Exactly    (also bareword ident)
        //   42        -> Numeric equal (min = max = 42)
        //   true/false-> Boolean
        switch (mLookahead.kind)
        {
        case TokenKind::Quoted:
        case TokenKind::Ident:
            rule.type = LeafRule::Type::String;
            rule.matchType = LeafRule::Match::Exactly;
            rule.filterString = mLookahead.text;
            if (auto ok = Advance(); !ok.has_value())
            {
                return std::unexpected(ok.error());
            }
            return MakeLeaf(std::move(rule));
        case TokenKind::Number:
        {
            const auto value = ParseDouble(mLookahead.text);
            if (!value.has_value())
            {
                QueryParseError err;
                err.offset = mLookahead.offset;
                err.message = "invalid numeric literal";
                return std::unexpected(err);
            }
            rule.type = LeafRule::Type::Number;
            rule.filterMinValue = *value;
            rule.filterMaxValue = *value;
            if (auto ok = Advance(); !ok.has_value())
            {
                return std::unexpected(ok.error());
            }
            return MakeLeaf(std::move(rule));
        }
        case TokenKind::True:
            rule.type = LeafRule::Type::Boolean;
            rule.filterValues.emplace_back("true");
            if (auto ok = Advance(); !ok.has_value())
            {
                return std::unexpected(ok.error());
            }
            return MakeLeaf(std::move(rule));
        case TokenKind::False:
            rule.type = LeafRule::Type::Boolean;
            rule.filterValues.emplace_back("false");
            if (auto ok = Advance(); !ok.has_value())
            {
                return std::unexpected(ok.error());
            }
            return MakeLeaf(std::move(rule));
        default:
        {
            QueryParseError err;
            err.offset = mLookahead.offset;
            err.message = "expected a value after '=' operator";
            return std::unexpected(err);
        }
        }
    }

    [[nodiscard]] std::expected<FilterExpression, QueryParseError> FinishCompareLeaf(LeafRule rule, const Token &opTok)
    {
        // `col {>,>=,<,<=} <ISO timestamp | number>`. Type picked
        // from the literal shape.
        std::string valueText;
        if (mLookahead.kind == TokenKind::Number || mLookahead.kind == TokenKind::Ident ||
            mLookahead.kind == TokenKind::Quoted)
        {
            valueText = mLookahead.text;
            if (auto ok = Advance(); !ok.has_value())
            {
                return std::unexpected(ok.error());
            }
        }
        else
        {
            QueryParseError err;
            err.offset = mLookahead.offset;
            err.message = "expected a value after comparison operator";
            return std::unexpected(err);
        }

        const auto asTimestamp = ParseIsoTimestamp(valueText);
        const auto asNumber = asTimestamp.has_value() ? std::nullopt : ParseDouble(valueText);
        if (!asTimestamp.has_value() && !asNumber.has_value())
        {
            QueryParseError err;
            err.offset = opTok.offset;
            err.message = "comparison operator expects a number or ISO timestamp";
            return std::unexpected(err);
        }
        const bool isTimestamp = asTimestamp.has_value();
        if (isTimestamp)
        {
            rule.type = LeafRule::Type::Time;
            switch (opTok.kind)
            {
            case TokenKind::Gt:
                rule.filterBegin = *asTimestamp + 1;
                break;
            case TokenKind::GtEq:
                rule.filterBegin = *asTimestamp;
                break;
            case TokenKind::Lt:
                rule.filterEnd = *asTimestamp - 1;
                break;
            case TokenKind::LtEq:
                rule.filterEnd = *asTimestamp;
                break;
            default:
                break;
            }
        }
        else
        {
            rule.type = LeafRule::Type::Number;
            // Strict `>`/`<` -> use the ULP-adjacent double via
            // `nextafter` so the inclusive-range predicate still
            // expresses "strictly greater / less".
            switch (opTok.kind)
            {
            case TokenKind::Gt:
                rule.filterMinValue = std::nextafter(*asNumber, std::numeric_limits<double>::infinity());
                break;
            case TokenKind::GtEq:
                rule.filterMinValue = *asNumber;
                break;
            case TokenKind::Lt:
                rule.filterMaxValue = std::nextafter(*asNumber, -std::numeric_limits<double>::infinity());
                break;
            case TokenKind::LtEq:
                rule.filterMaxValue = *asNumber;
                break;
            default:
                break;
            }
        }
        return MakeLeaf(std::move(rule));
    }

    [[nodiscard]] std::expected<FilterExpression, QueryParseError> FinishInLeaf(LeafRule rule)
    {
        if (mLookahead.kind != TokenKind::LBracket)
        {
            QueryParseError err;
            err.offset = mLookahead.offset;
            err.message = "expected '[' to open value list after 'in'";
            return std::unexpected(err);
        }
        if (auto ok = Advance(); !ok.has_value())
        {
            return std::unexpected(ok.error());
        }
        // Two shapes: `[a, b, c]` (Enumeration/Boolean list) or
        // `[min..max]` (Numeric/Time range). Peek for `..` to
        // pick between them.
        if (mLookahead.kind == TokenKind::DotDot)
        {
            // `[..max]`
            if (auto ok = Advance(); !ok.has_value())
            {
                return std::unexpected(ok.error());
            }
            return FinishRangeUpper(std::move(rule), std::nullopt);
        }
        Token firstTok = mLookahead;
        if (mLookahead.kind == TokenKind::Number || mLookahead.kind == TokenKind::Ident ||
            mLookahead.kind == TokenKind::Quoted)
        {
            if (auto ok = Advance(); !ok.has_value())
            {
                return std::unexpected(ok.error());
            }
        }
        else if (mLookahead.kind == TokenKind::True || mLookahead.kind == TokenKind::False)
        {
            if (auto ok = Advance(); !ok.has_value())
            {
                return std::unexpected(ok.error());
            }
        }
        else if (mLookahead.kind == TokenKind::RBracket)
        {
            // Reject `col in []`: it used to compile to nothing
            // (silent match-all) and hit the UI with an empty
            // payload. Surface it as an underlined parse error.
            QueryParseError err;
            err.offset = mLookahead.offset;
            err.message = "value list cannot be empty";
            return std::unexpected(err);
        }
        else
        {
            QueryParseError err;
            err.offset = mLookahead.offset;
            err.message = "expected a value or ']' inside 'in [...]'";
            return std::unexpected(err);
        }

        if (mLookahead.kind == TokenKind::DotDot)
        {
            // `[a..` -> range with a lower bound.
            if (auto ok = Advance(); !ok.has_value())
            {
                return std::unexpected(ok.error());
            }
            return FinishRangeUpper(std::move(rule), firstTok);
        }
        // List form: gather the rest.
        return FinishListLeaf(std::move(rule), firstTok);
    }

    [[nodiscard]] std::expected<FilterExpression, QueryParseError>
    FinishRangeUpper(LeafRule rule, std::optional<Token> firstTok)
    {
        std::optional<Token> secondTok;
        if (mLookahead.kind == TokenKind::Number || mLookahead.kind == TokenKind::Ident ||
            mLookahead.kind == TokenKind::Quoted)
        {
            secondTok = mLookahead;
            if (auto ok = Advance(); !ok.has_value())
            {
                return std::unexpected(ok.error());
            }
        }
        if (mLookahead.kind != TokenKind::RBracket)
        {
            QueryParseError err;
            err.offset = mLookahead.offset;
            err.message = "expected ']' to close range";
            return std::unexpected(err);
        }
        // Reject `col in [..]` for the same reason as `[]`.
        if (!firstTok.has_value() && !secondTok.has_value())
        {
            QueryParseError err;
            err.offset = mLookahead.offset;
            err.message = "range needs at least a lower or an upper bound";
            return std::unexpected(err);
        }
        if (auto ok = Advance(); !ok.has_value())
        {
            return std::unexpected(ok.error());
        }

        // Type inference: if either bound parses as ISO timestamp,
        // the whole range is Time; otherwise Numeric.
        std::optional<std::int64_t> tsMin;
        std::optional<std::int64_t> tsMax;
        std::optional<double> numMin;
        std::optional<double> numMax;
        auto classify = [&](const Token &tok, std::optional<std::int64_t> &ts, std::optional<double> &num) {
            const auto asTs = ParseIsoTimestamp(tok.text);
            if (asTs.has_value())
            {
                ts = *asTs;
                return true;
            }
            const auto asNum = ParseDouble(tok.text);
            if (asNum.has_value())
            {
                num = *asNum;
                return true;
            }
            return false;
        };
        if (firstTok.has_value() && !classify(*firstTok, tsMin, numMin))
        {
            QueryParseError err;
            err.offset = firstTok->offset;
            err.message = "range lower bound must be a number or ISO timestamp";
            return std::unexpected(err);
        }
        if (secondTok.has_value() && !classify(*secondTok, tsMax, numMax))
        {
            QueryParseError err;
            err.offset = secondTok->offset;
            err.message = "range upper bound must be a number or ISO timestamp";
            return std::unexpected(err);
        }
        // Reject mixed-type bounds up-front. Otherwise the "Time or
        // Number" branch below picks Time and silently drops the
        // numeric bound (a hidden half-open range).
        if ((tsMin.has_value() && numMax.has_value()) ||
            (numMin.has_value() && tsMax.has_value()))
        {
            QueryParseError err;
            err.offset = secondTok->offset;
            err.message = "range bounds must both be numeric or both be ISO timestamps";
            return std::unexpected(err);
        }
        // Reject inverted ranges: they can never accept a row, and
        // "Parsed OK" + empty table gives no cue.
        const bool invertedTime = tsMin.has_value() && tsMax.has_value() && *tsMin > *tsMax;
        const bool invertedNumber = numMin.has_value() && numMax.has_value() && *numMin > *numMax;
        if (invertedTime || invertedNumber)
        {
            QueryParseError err;
            err.offset = secondTok->offset;
            err.message = "range upper bound is below the lower bound";
            return std::unexpected(err);
        }
        if (tsMin.has_value() || tsMax.has_value())
        {
            rule.type = LeafRule::Type::Time;
            rule.filterBegin = tsMin;
            rule.filterEnd = tsMax;
        }
        else
        {
            rule.type = LeafRule::Type::Number;
            rule.filterMinValue = numMin;
            rule.filterMaxValue = numMax;
        }
        return MakeLeaf(std::move(rule));
    }

    [[nodiscard]] std::expected<FilterExpression, QueryParseError>
    FinishListLeaf(LeafRule rule, const Token &firstTok)
    {
        // All-boolean list (`[true, false]`) is the printer's wire
        // form for `Type::Boolean`; detect that so the round-trip
        // preserves the type. Mixed lists stay `Enumeration`.
        const auto isBoolKind = [](TokenKind k) {
            return k == TokenKind::True || k == TokenKind::False;
        };
        bool allBool = isBoolKind(firstTok.kind);
        rule.filterValues.push_back(firstTok.text);
        while (mLookahead.kind == TokenKind::Comma)
        {
            if (auto ok = Advance(); !ok.has_value())
            {
                return std::unexpected(ok.error());
            }
            if (mLookahead.kind != TokenKind::Ident && mLookahead.kind != TokenKind::Quoted &&
                mLookahead.kind != TokenKind::Number && mLookahead.kind != TokenKind::True &&
                mLookahead.kind != TokenKind::False)
            {
                QueryParseError err;
                err.offset = mLookahead.offset;
                err.message = "expected a value after ','";
                return std::unexpected(err);
            }
            allBool = allBool && isBoolKind(mLookahead.kind);
            rule.filterValues.push_back(mLookahead.text);
            if (auto ok = Advance(); !ok.has_value())
            {
                return std::unexpected(ok.error());
            }
        }
        if (mLookahead.kind != TokenKind::RBracket)
        {
            QueryParseError err;
            err.offset = mLookahead.offset;
            err.message = "expected ']' or ',' to continue 'in [...]' list";
            return std::unexpected(err);
        }
        if (auto ok = Advance(); !ok.has_value())
        {
            return std::unexpected(ok.error());
        }
        if (allBool)
        {
            rule.type = LeafRule::Type::Boolean;
            // Lowercase so `True`/`FALSE` still round-trip through
            // the pretty printer's lowercase output.
            for (std::string &v : rule.filterValues)
            {
                std::ranges::transform(v, v.begin(), [](unsigned char ch) {
                    return static_cast<char>(std::tolower(ch));
                });
            }
        }
        else if (rule.filterValues.size() == 1)
        {
            // Single-item list: if the value parses as a number,
            // demote to `Type::Number` (min==max) so `col IN [42]`
            // behaves like `col = 42` on numeric columns instead of
            // silently matching zero rows via the enum fallback.
            // ISO timestamps get the same treatment for symmetry.
            const std::string &only = rule.filterValues.front();
            if (const auto asTs = ParseIsoTimestamp(only); asTs.has_value())
            {
                rule.type = LeafRule::Type::Time;
                rule.filterBegin = *asTs;
                rule.filterEnd = *asTs;
                rule.filterValues.clear();
            }
            else if (const auto asNum = ParseDouble(only); asNum.has_value())
            {
                rule.type = LeafRule::Type::Number;
                rule.filterMinValue = *asNum;
                rule.filterMaxValue = *asNum;
                rule.filterValues.clear();
            }
            else
            {
                rule.type = LeafRule::Type::Enumeration;
            }
        }
        else
        {
            rule.type = LeafRule::Type::Enumeration;
        }
        return MakeLeaf(std::move(rule));
    }

    Lexer mLexer;
    std::string_view mInput;
    Token mLookahead;
};

// ---- Pretty printer --------------------------------------------------------

enum class Precedence : std::uint8_t
{
    Or,
    And,
    Not,
    Atom
};

void AppendExpression(const FilterExpression &expr, Precedence parent, std::string &out);

/// Emit one leaf. Barewords for common cases; quoted forms when
/// the payload would collide with a keyword / operator / number.
void AppendLeaf(const LeafRule &rule, std::string &out)
{
    const std::string columnText = rule.columnKeys.empty() ? std::string() : FormatIdent(rule.columnKeys.front());
    out.append(columnText);
    switch (rule.type)
    {
    case LeafRule::Type::String:
    {
        const LeafRule::Match match = rule.matchType.value_or(LeafRule::Match::Contains);
        const std::string value = rule.filterString.value_or(std::string{});
        switch (match)
        {
        case LeafRule::Match::Contains:
            out.push_back(':');
            out.append(FormatIdent(value));
            break;
        case LeafRule::Match::Exactly:
            out.push_back('=');
            // Always quoted so the round-trip stays unambiguous
            // vs. numeric / boolean `=` literals.
            out.append(QuoteString(value));
            break;
        case LeafRule::Match::RegularExpression:
            out.push_back('~');
            out.push_back('/');
            for (const char ch : value)
            {
                if (ch == '/')
                {
                    out.append("\\/");
                }
                else
                {
                    out.push_back(ch);
                }
            }
            out.push_back('/');
            break;
        case LeafRule::Match::Wildcard:
            out.push_back('%');
            out.append(QuoteString(value));
            break;
        }
        break;
    }
    case LeafRule::Type::Number:
    {
        // Single value -> `=N`; one-sided -> `>=N` / `<=N`;
        // both sides -> `IN [min..max]`.
        const bool hasMin = rule.filterMinValue.has_value();
        const bool hasMax = rule.filterMaxValue.has_value();
        if (hasMin && hasMax && *rule.filterMinValue == *rule.filterMaxValue)
        {
            out.push_back('=');
            out.append(FormatNumber(*rule.filterMinValue));
        }
        else if (hasMin && hasMax)
        {
            out.append(" IN [");
            out.append(FormatNumber(*rule.filterMinValue));
            out.append("..");
            out.append(FormatNumber(*rule.filterMaxValue));
            out.push_back(']');
        }
        else if (hasMin)
        {
            out.append(">=");
            out.append(FormatNumber(*rule.filterMinValue));
        }
        else if (hasMax)
        {
            out.append("<=");
            out.append(FormatNumber(*rule.filterMaxValue));
        }
        else
        {
            // No bounds -> inert. Placeholder round-trips as the
            // same tree; the compile step drops the leaf.
            out.append(" IN []");
        }
        break;
    }
    case LeafRule::Type::Time:
    {
        const bool hasBegin = rule.filterBegin.has_value();
        const bool hasEnd = rule.filterEnd.has_value();
        if (hasBegin && hasEnd)
        {
            out.append(" IN [");
            out.append(FormatTimestampMicros(*rule.filterBegin));
            out.append("..");
            out.append(FormatTimestampMicros(*rule.filterEnd));
            out.push_back(']');
        }
        else if (hasBegin)
        {
            out.append(">=");
            out.append(FormatTimestampMicros(*rule.filterBegin));
        }
        else if (hasEnd)
        {
            out.append("<=");
            out.append(FormatTimestampMicros(*rule.filterEnd));
        }
        else
        {
            out.append(" IN []");
        }
        break;
    }
    case LeafRule::Type::Boolean:
    {
        // Collapse to `=true` / `=false` / `IN [true, false]`.
        bool hasTrue = false;
        bool hasFalse = false;
        for (const std::string &v : rule.filterValues)
        {
            std::string lower;
            lower.reserve(v.size());
            for (const char ch : v)
            {
                lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
            }
            if (lower == "true")
            {
                hasTrue = true;
            }
            else if (lower == "false")
            {
                hasFalse = true;
            }
        }
        if (hasTrue && hasFalse)
        {
            out.append(" IN [true, false]");
        }
        else if (hasTrue)
        {
            out.append("=true");
        }
        else if (hasFalse)
        {
            out.append("=false");
        }
        else
        {
            out.append(" IN []");
        }
        break;
    }
    case LeafRule::Type::Enumeration:
        out.append(" IN [");
        for (std::size_t i = 0; i < rule.filterValues.size(); ++i)
        {
            if (i != 0)
            {
                out.append(", ");
            }
            out.append(FormatIdent(rule.filterValues[i]));
        }
        out.push_back(']');
        break;
    }
}

void AppendAnd(const FilterExpression::And &node, Precedence parent, std::string &out)
{
    if (node.children.empty())
    {
        // Empty And = match-all. The top-level case renders as the
        // empty string in `FormatExpression`; a nested empty And is
        // not in the grammar, so emit `*` as a debug placeholder.
        out.append("*");
        return;
    }
    const bool needParens = parent > Precedence::And;
    if (needParens)
    {
        out.push_back('(');
    }
    for (std::size_t i = 0; i < node.children.size(); ++i)
    {
        if (i != 0)
        {
            out.append(" AND ");
        }
        AppendExpression(node.children[i], Precedence::And, out);
    }
    if (needParens)
    {
        out.push_back(')');
    }
}

void AppendOr(const FilterExpression::Or &node, Precedence parent, std::string &out)
{
    if (node.children.empty())
    {
        // Empty Or = match-none. No grammar spelling; debug placeholder.
        out.append("()");
        return;
    }
    const bool needParens = parent > Precedence::Or;
    if (needParens)
    {
        out.push_back('(');
    }
    for (std::size_t i = 0; i < node.children.size(); ++i)
    {
        if (i != 0)
        {
            out.append(" OR ");
        }
        AppendExpression(node.children[i], Precedence::Or, out);
    }
    if (needParens)
    {
        out.push_back(')');
    }
}

void AppendNot(const FilterExpression::Not &node, std::string &out)
{
    out.append("NOT ");
    if (node.child == nullptr)
    {
        out.append("()");
        return;
    }
    AppendExpression(*node.child, Precedence::Not, out);
}

void AppendExpression(const FilterExpression &expr, Precedence parent, std::string &out)
{
    std::visit(
        [&out, parent](const auto &node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, FilterExpression::Leaf>)
            {
                AppendLeaf(node.rule, out);
            }
            else if constexpr (std::is_same_v<T, FilterExpression::And>)
            {
                AppendAnd(node, parent, out);
            }
            else if constexpr (std::is_same_v<T, FilterExpression::Or>)
            {
                AppendOr(node, parent, out);
            }
            else
            {
                AppendNot(node, out);
            }
        },
        expr.node
    );
}

} // namespace

std::expected<FilterExpression, QueryParseError> ParseQuery(std::string_view input)
{
    Parser parser(input);
    return parser.Parse();
}

std::string FormatExpression(const FilterExpression &expression)
{
    // Match-all -> empty string (round-trip contract in the header).
    if (IsMatchAll(expression))
    {
        return {};
    }
    // Peel single-child `And` wrappers at the root:
    // `MirrorSessionStateToConfiguration` / `ApplyAdvancedFilterResult`
    // wrap Advanced-only subtrees in `And([X])` to keep the "top-level
    // Leaf children live in `mSimpleLeaves`" invariant. Unwrapping keeps
    // the editor round-trip visibly stable. Stops before producing
    // match-all so nested empty `And` still hits the placeholder path.
    const FilterExpression *effective = &expression;
    while (true)
    {
        const auto *andNode = std::get_if<FilterExpression::And>(&effective->node);
        if (andNode == nullptr || andNode->children.size() != 1 ||
            IsMatchAll(andNode->children.front()))
        {
            break;
        }
        effective = &andNode->children.front();
    }
    std::string out;
    AppendExpression(*effective, Precedence::Or, out);
    return out;
}

} // namespace loglib
