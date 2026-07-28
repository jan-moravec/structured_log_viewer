#include "loglib/query_parser.hpp"

#include "loglib/filter_expression.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
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
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace loglib
{

namespace
{

/// Tokens emitted by the lexer. Kept small; operator strings share
/// one enum entry per fixed spelling so the parser can pattern-match
/// on `kind` rather than re-checking `text`.
enum class TokenKind : std::uint8_t
{
    Ident,      ///< bareword identifier (column or value)
    Quoted,     ///< "quoted string" -- `text` holds the decoded body
    Regex,      ///< /regex body/ (only produced after `~`)
    Number,     ///< numeric literal -- `text` holds the raw digits
    True,       ///< the bareword `true`
    False,      ///< the bareword `false`
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
    End,        ///< end-of-input sentinel
};

struct Token
{
    TokenKind kind = TokenKind::End;
    /// Decoded text (for Ident/Quoted/Regex/Number). Empty otherwise.
    std::string text;
    /// Byte offset into the input where this token started. Used
    /// for error reporting.
    std::size_t offset = 0;
};

/// True iff @p ch may appear inside a bareword identifier. The set
/// deliberately excludes operator characters and brackets so the
/// lexer doesn't have to backtrack.
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

    /// Consume the next token. Returns `Token::End` after the
    /// last real token so callers can peek without special-casing.
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
        // Number literal -- signed, optional decimal + exponent. We
        // treat a leading `+` or `-` followed by a digit as the start
        // of a number so range bounds like `[-1..1]` parse cleanly.
        // ISO-8601 timestamp bareword takes precedence over plain
        // number when the shape matches (`YYYY-MM-DD` prefix) so a
        // bareword like `2024-01-02T00:00:00Z` doesn't split at the
        // first `-`.
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

    /// Regex literals are only recognised in this dedicated entry
    /// point (called by the parser after it consumes a `~`). This
    /// keeps `/` free to disambiguate: inside a regex it is a
    /// literal, outside it is a syntax error until we grow another
    /// operator that needs it.
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
                // Preserve every backslash escape verbatim; the
                // regex engine decides what `\d`, `\s`, `\.` mean.
                // Only `\/` collapses to `/` so the closing slash
                // stays unambiguous.
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

    /// Try to lex an ISO-8601 timestamp bareword starting at
    /// `mPos`. Recognises the same shape family `ParseIsoTimestamp`
    /// accepts (bare date, date+T+time, optional fraction / TZ)
    /// so bareword timestamps parse as one Ident token instead
    /// of splitting at the first `-`. Returns `nullopt` when the
    /// input doesn't match (caller falls back to `LexNumber`).
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
                // Not a time -- rewind so bare-date form still
                // matches (a trailing `T` would otherwise consume
                // the marker without content).
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
        // Reject if the token bleeds into another identifier /
        // number character we didn't intend to consume.
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
            // Reject a stripped-down exponent like `1e`, `1e+`, `1e-`
            // at the lexer -- otherwise `std::from_chars` fails later
            // against the whole token and the diagnostic points at the
            // token start rather than the offending 'e'.
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
        // Case-insensitive keyword mapping. `and`, `or`, `not`, `in`,
        // `true`, `false` are reserved; everything else is a plain
        // Ident.
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

/// Try to parse @p text as an inclusive numeric bound in the
/// `[..]` range grammar. Returns `nullopt` when the input isn't
/// a well-formed number (the range-parser then falls back to
/// treating the bound as a Time / ISO literal).
[[nodiscard]] std::optional<double> ParseDouble(std::string_view text) noexcept
{
    if (text.empty())
    {
        return std::nullopt;
    }
    // `std::from_chars` for `double` is available on MSVC, libc++,
    // and libstdc++ >= 11. It's the fastest allocation-free path.
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

/// Case-insensitive `str.starts_with` for the tiny prefix set used
/// by the ISO-timestamp detector.
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

/// Try to parse an ISO-8601 timestamp (with or without a fractional
/// part / timezone suffix) as microseconds since the UNIX epoch.
/// Returns `nullopt` for anything the format doesn't match. Kept
/// permissive on the calendar side and strict on the shape.
[[nodiscard]] std::optional<std::int64_t> ParseIsoTimestamp(std::string_view text)
{
    // Fast-reject: ISO timestamps start with `YYYY` and contain
    // either `-` or `T` in the first ten bytes.
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

    int hour = 0;
    int minute = 0;
    int second = 0;
    std::int64_t fractionalMicros = 0;
    std::int64_t offsetSeconds = 0;
    bool hasTime = false;
    std::size_t cursor = 10;
    if (cursor < text.size() && (text[cursor] == 'T' || text[cursor] == ' '))
    {
        hasTime = true;
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
            // Scale fractional up to microseconds: fractional / denom * 1e6.
            fractionalMicros = (fractional * 1'000'000) / denom;
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
                    ++cursor;
                }
                if (cursor + 1 < text.size() && (std::isdigit(static_cast<unsigned char>(text[cursor])) != 0))
                {
                    const auto tzMinutes = readInt(text, cursor, 2);
                    if (!tzMinutes.has_value())
                    {
                        return std::nullopt;
                    }
                    tzM = *tzMinutes;
                    cursor += 2;
                }
                offsetSeconds = sign * ((*tzH * 3600) + (tzM * 60));
            }
        }
    }
    if (cursor != text.size())
    {
        return std::nullopt;
    }
    // Convert `(year, month, day, hour, minute, second)` to
    // days-since-epoch via the civil-from-fields formula (Howard
    // Hinnant's date algorithms). Avoids pulling `<chrono>` field
    // conversions that require a system TZ database.
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
    if (!hasTime)
    {
        // Bare date -> midnight UTC of that day.
    }
    const std::int64_t micros = (secondsSinceEpoch * 1'000'000LL) + fractionalMicros;
    return micros;
}

/// Bareword classifier used by the pretty-printer. When the value
/// happens to look like a keyword, number, contains whitespace or
/// operator characters, we quote it in the output so the round-trip
/// re-parses to the same tree.
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
    // Reserved keywords must be quoted when used as identifiers /
    // values so they don't turn back into keyword tokens.
    static const std::unordered_set<std::string> reserved{"and", "or", "not", "in", "true", "false"};
    std::string lower;
    lower.reserve(text.size());
    for (const char ch : text)
    {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return reserved.contains(lower);
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

/// Emit a numeric bound so the round-trip parses back to the
/// exact double. `%.17g`-style rendering keeps precision at the
/// cost of a couple of extra digits; that's fine for filter UX.
[[nodiscard]] std::string FormatNumber(double value)
{
    // `std::to_chars` gives round-trip precision and is locale-free.
    // 32 bytes covers every double.
    std::array<char, 32> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (result.ec != std::errc{})
    {
        return std::to_string(value);
    }
    return std::string(buffer.data(), static_cast<std::size_t>(result.ptr - buffer.data()));
}

/// Render a `TimeStamp` field back to an ISO-8601 microsecond
/// literal (`YYYY-MM-DDTHH:MM:SS.uuuuuu`). Complement of
/// `ParseIsoTimestamp` for the pretty-printer.
[[nodiscard]] std::string FormatTimestampMicros(std::int64_t micros)
{
    // C++ integer division truncates towards zero, so a negative
    // `micros` with a non-zero remainder must borrow one whole
    // second and reflect the frac from the previous second. Without
    // this borrow, `-500'000` (500ms before epoch) rendered as
    // `1970-01-01T00:00:00.500000Z` instead of the correct
    // `1969-12-31T23:59:59.500000Z`.
    std::int64_t whole = micros / 1'000'000;
    std::int64_t remainder = micros % 1'000'000;
    if (remainder < 0)
    {
        whole -= 1;
        remainder += 1'000'000;
    }
    const auto frac = static_cast<int>(remainder);
    std::time_t t = static_cast<std::time_t>(whole);
    std::tm utc{};
    // `gmtime_s` (Windows) / `gmtime_r` (POSIX) both fail on times
    // outside the platform's representable range. Windows in
    // particular rejects year > 9999 with `EINVAL` and leaves `utc`
    // zeroed; `strftime` then emits "0000-00-00T00:00:00", which
    // would silently mis-render. Detect the failure and fall back
    // to the raw microsecond count so the roundtrip stays lossy but
    // non-misleading.
#ifdef _WIN32
    const bool gmtOk = (gmtime_s(&utc, &t) == 0);
#else
    const bool gmtOk = (gmtime_r(&t, &utc) != nullptr);
#endif
    if (!gmtOk)
    {
        return "epoch_micros:" + std::to_string(micros);
    }
    std::array<char, 32> buffer{};
    const std::size_t n = std::strftime(buffer.data(), buffer.size(), "%Y-%m-%dT%H:%M:%S", &utc);
    if (n == 0)
    {
        return "epoch_micros:" + std::to_string(micros);
    }
    std::string out(buffer.data(), n);
    std::array<char, 16> fracBuffer{};
    const int written = std::snprintf(fracBuffer.data(), fracBuffer.size(), ".%06d", frac);
    if (written > 0)
    {
        out.append(fracBuffer.data(), static_cast<std::size_t>(written));
    }
    out.push_back('Z');
    return out;
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
    ///
    /// Implicit-AND fires when the next token can start a new
    /// leaf / group and no `OR` / closing token is pending.
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
            // Implicit AND: peek at whether the next token can
            // start a fresh atom. Terminators (RParen, RBracket,
            // Comma, End, KwOr) close the AND chain.
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
            return FinishInLeaf(std::move(rule), opTok);
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
        // Payload type-drives here:
        //   `col = "text"`   -> String Exactly
        //   `col = 42`       -> Numeric equal (min = max = 42)
        //   `col = true`     -> Boolean true
        //   `col = ident`    -> String Exactly (bareword)
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
        // `col > N`, `col >= N`, `col < N`, `col <= N`.
        // Time (ISO literal) and Numeric supported; picks type from
        // the literal shape.
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
            // Numeric strict-vs-inclusive: we use ULP-adjacent
            // doubles for `>`/`<` so the range still expresses
            // "strictly greater / less". `nextafter` returns the
            // next representable double toward +inf / -inf.
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

    [[nodiscard]] std::expected<FilterExpression, QueryParseError> FinishInLeaf(LeafRule rule, const Token &opTok)
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
        // Two shapes: `[a, b, c]` (list -> Enumeration) or
        // `[min..max]` (range -> Numeric / Time). Distinguish
        // by peeking for `..` after the (optional) first bound.
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
            // Empty list -> inert leaf; caller drops.
            if (auto ok = Advance(); !ok.has_value())
            {
                return std::unexpected(ok.error());
            }
            rule.type = LeafRule::Type::Enumeration;
            return MakeLeaf(std::move(rule));
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
        return FinishListLeaf(std::move(rule), firstTok, opTok);
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
    FinishListLeaf(LeafRule rule, const Token &firstTok, const Token &opTok)
    {
        // A list of exclusively `true` / `false` tokens is the wire
        // form the pretty printer emits for `LeafRule::Type::Boolean`
        // (`col in [true, false]`). Detect that shape so the round
        // trip preserves the type; otherwise the compiled predicate
        // walks the `Enumeration` path against a `Boolean` column,
        // falls back to string-set matching, and never accepts a row.
        // Mixed lists (`[true, "x"]`) stay `Enumeration` so hand-typed
        // heterogeneous lists aren't silently misclassified.
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
        (void)opTok;
        if (allBool)
        {
            rule.type = LeafRule::Type::Boolean;
            // Normalise to lowercase so hand-typed `True` / `FALSE`
            // still round-trip cleanly through `FormatExpression`
            // (which emits lowercase) and match the compiled
            // predicate's canonical form.
            for (std::string &v : rule.filterValues)
            {
                std::ranges::transform(v, v.begin(), [](unsigned char ch) {
                    return static_cast<char>(std::tolower(ch));
                });
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
    Or,   ///< lowest -- printed unparenthesised at the root
    And,  ///< middle
    Not,  ///< above And
    Atom  ///< leaves and parenthesised subexpressions
};

void AppendExpression(const FilterExpression &expr, Precedence parent, std::string &out);

/// Emit one leaf. Bareword-friendly for common cases; falls back
/// to quoted forms when the payload would collide with a keyword,
/// operator, or number.
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
            // Force quoted output so the round-trip disambiguates
            // from a numeric or boolean `=` literal.
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
        // Single value -> `col = N`; one-sided -> `col > / >= / < / <= N`;
        // full range -> `col in [min..max]`.
        const bool hasMin = rule.filterMinValue.has_value();
        const bool hasMax = rule.filterMaxValue.has_value();
        if (hasMin && hasMax && *rule.filterMinValue == *rule.filterMaxValue)
        {
            out.push_back('=');
            out.append(FormatNumber(*rule.filterMinValue));
        }
        else if (hasMin && hasMax)
        {
            out.append(" in [");
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
            // No bounds -> inert. Emit a canonical placeholder so
            // the round-trip re-produces the same tree; the compile
            // step drops the leaf.
            out.append(" in []");
        }
        break;
    }
    case LeafRule::Type::Time:
    {
        const bool hasBegin = rule.filterBegin.has_value();
        const bool hasEnd = rule.filterEnd.has_value();
        if (hasBegin && hasEnd)
        {
            out.append(" in [");
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
            out.append(" in []");
        }
        break;
    }
    case LeafRule::Type::Boolean:
    {
        // Multi-value bool collapses to `col=true`, `col=false`, or
        // `col in [true, false]`.
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
            out.append(" in [true, false]");
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
            out.append(" in []");
        }
        break;
    }
    case LeafRule::Type::Enumeration:
        out.append(" in [");
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
        // Empty And = match all. Canonical spelling: `*` isn't in
        // the grammar; we emit a comment placeholder instead. Empty
        // trees round-trip through `ParseQuery("")`, so callers
        // usually short-circuit on `IsMatchAll` before formatting.
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
        // Empty Or = match none. No first-class spelling; emit an
        // always-false placeholder so the round-trip preserves shape.
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
    // Match-all round-trips as the empty string (see `ParseQuery`:
    // empty / whitespace-only input yields the default match-all
    // `FilterExpression`). Emitting anything for the top-level
    // empty `And` -- including the debug placeholder `*` used by
    // `AppendAnd` -- would break the header's round-trip contract,
    // since `*` is not in the grammar.
    if (IsMatchAll(expression))
    {
        return {};
    }
    std::string out;
    AppendExpression(expression, Precedence::Or, out);
    return out;
}

} // namespace loglib
