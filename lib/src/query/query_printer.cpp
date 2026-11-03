#include "loglib/query/query_parser.hpp"

#include "loglib/core/ascii_case.hpp"
#include "loglib/query/internal/query_lexer.hpp"

#include "loglib/schema/filter_expression.hpp"
#include "loglib/time/time_zone_context.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <format>
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

using namespace internal::query;

/// Range accepted by the pretty printer -- outside `[0, 9999]`
/// there's no plain ISO spelling in this grammar, so the printer
/// falls back to the `epoch_micros:N` marker.
constexpr int ISO_YEAR_MAX = 9999;

/// `days_from_civil` / `civil_from_days` epoch constant from Howard
/// Hinnant's date algorithms (days between 0000-03-01 and the UNIX
/// epoch 1970-01-01).
constexpr std::int64_t CIVIL_DAYS_EPOCH_OFFSET = 719468;

/// Scratch buffer for `std::to_chars(double)`: 64 bytes fits every
/// shortest-form output with slack.
constexpr std::size_t NUMBER_TO_CHARS_BUFFER_SIZE = 64;

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
    static constexpr std::array<std::string_view, 6> RESERVED{"and", "or", "not", "in", "true", "false"};
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
    std::array<char, NUMBER_TO_CHARS_BUFFER_SIZE> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (result.ec != std::errc{})
    {
        return std::to_string(value);
    }
    return {buffer.data(), static_cast<std::size_t>(result.ptr - buffer.data())};
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
    z += CIVIL_DAYS_EPOCH_OFFSET;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const auto doe = static_cast<std::uint64_t>(z - (era * 146097)); // [0, 146096]
    const std::uint64_t yoe =                                        // [0, 399]
        (doe - (doe / 1460) + (doe / 36524) - (doe / 146096)) / 365;
    const std::int64_t y = static_cast<std::int64_t>(yoe) + (era * 400);
    const std::uint64_t doy = doe - ((365 * yoe) + (yoe / 4) - (yoe / 100)); // [0, 365]
    const std::uint64_t mp = ((5 * doy) + 2) / 153;                          // [0, 11]
    const auto d = static_cast<unsigned>(doy - (((153 * mp) + 2) / 5) + 1);  // [1, 31]
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
    std::int64_t totalSeconds = micros / MICROS_PER_SECOND;
    std::int64_t microRemainder = micros % MICROS_PER_SECOND;
    if (microRemainder < 0)
    {
        totalSeconds -= 1;
        microRemainder += MICROS_PER_SECOND;
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
    if (date.year < 0 || date.year > ISO_YEAR_MAX)
    {
        return "epoch_micros:" + std::to_string(micros);
    }

    const auto hour = static_cast<int>(secondOfDay / SECONDS_PER_HOUR);
    const auto minute = static_cast<int>((secondOfDay % SECONDS_PER_HOUR) / SECONDS_PER_MINUTE);
    const auto second = static_cast<int>(secondOfDay % SECONDS_PER_MINUTE);

    // `std::format` avoids the C-varargs `snprintf` (silences
    // cppcoreguidelines-pro-type-vararg) and hard-caps the buffer
    // to whatever `[0, ISO_YEAR_MAX]` produces.
    return std::format(
        "{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:06}Z",
        static_cast<int>(date.year),
        date.month,
        date.day,
        hour,
        minute,
        second,
        static_cast<int>(microRemainder)
    );
}

// ---- Pretty printer --------------------------------------------------------

enum class Precedence : std::uint8_t
{
    Or,
    And,
    Not,
    Atom,
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
            // No bounds -> inert. Debug placeholder only: the parser
            // rejects an empty list.
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

// `AppendAnd` / `AppendOr` / `AppendNot` / `AppendExpression` form
// a mutually-recursive AST walker; misc-no-recursion is silenced on
// each entry point so the visitor lambda specialisations don't get
// individually flagged.
// NOLINTNEXTLINE(misc-no-recursion)
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

// NOLINTNEXTLINE(misc-no-recursion)
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

// NOLINTNEXTLINE(misc-no-recursion)
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

// NOLINTNEXTLINE(misc-no-recursion)
void AppendExpression(const FilterExpression &expr, Precedence parent, std::string &out)
{
    std::visit(
        // NOLINTNEXTLINE(misc-no-recursion)
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
        if (andNode == nullptr || andNode->children.size() != 1 || IsMatchAll(andNode->children.front()))
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
