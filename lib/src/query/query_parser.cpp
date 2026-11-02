#include "loglib/query/query_parser.hpp"

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

// ---- Parser ----------------------------------------------------------------

class Parser
{
public:
    explicit Parser(std::string_view input)
        : mLexer(input), mInput(input)
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
    ///
    /// The four `ParseOr` / `ParseAnd` / `ParseNot` / `ParseAtom`
    /// members form a mutually-recursive descent parser; the
    /// misc-no-recursion suppression is here (and on the three
    /// members below) rather than at the parser entry point so the
    /// grammar shape stays obvious.
    // NOLINTNEXTLINE(misc-no-recursion)
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
    // NOLINTNEXTLINE(misc-no-recursion)
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

    /// Counts one level of recursion for the lifetime of a parse step.
    class NestingScope
    {
    public:
        explicit NestingScope(std::size_t &depth) noexcept
            : mDepth(depth)
        {
            ++mDepth;
        }
        ~NestingScope()
        {
            --mDepth;
        }
        NestingScope(const NestingScope &) = delete;
        NestingScope &operator=(const NestingScope &) = delete;
        NestingScope(NestingScope &&) = delete;
        NestingScope &operator=(NestingScope &&) = delete;

    private:
        std::size_t &mDepth;
    };

    [[nodiscard]] QueryParseError NestingError() const
    {
        QueryParseError err;
        err.offset = mLookahead.offset;
        err.message = "expression is nested too deeply";
        return err;
    }

    /// `not_expr := 'NOT' not_expr | atom`
    // NOLINTNEXTLINE(misc-no-recursion)
    [[nodiscard]] std::expected<FilterExpression, QueryParseError> ParseNot()
    {
        if (mLookahead.kind == TokenKind::KwNot)
        {
            const NestingScope scope(mNesting);
            if (mNesting > MAX_QUERY_NESTING)
            {
                return std::unexpected(NestingError());
            }
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
    // NOLINTNEXTLINE(misc-no-recursion)
    [[nodiscard]] std::expected<FilterExpression, QueryParseError> ParseAtom()
    {
        if (mLookahead.kind == TokenKind::LParen)
        {
            const NestingScope scope(mNesting);
            if (mNesting > MAX_QUERY_NESTING)
            {
                return std::unexpected(NestingError());
            }
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

    [[nodiscard]] std::expected<FilterExpression, QueryParseError> FinishStringLeaf(
        LeafRule rule, LeafRule::Match matchType
    )
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
            rule.filterMinValue = value;
            rule.filterMaxValue = value;
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
                rule.filterBegin = asTimestamp;
                break;
            case TokenKind::Lt:
                rule.filterEnd = *asTimestamp - 1;
                break;
            case TokenKind::LtEq:
                rule.filterEnd = asTimestamp;
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
                rule.filterMinValue = asNumber;
                break;
            case TokenKind::Lt:
                rule.filterMaxValue = std::nextafter(*asNumber, -std::numeric_limits<double>::infinity());
                break;
            case TokenKind::LtEq:
                rule.filterMaxValue = asNumber;
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

    [[nodiscard]] std::expected<FilterExpression, QueryParseError> FinishRangeUpper(
        LeafRule rule, std::optional<Token> firstTok
    )
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
        const auto classify = [&](const Token &tok, std::optional<std::int64_t> &ts, std::optional<double> &num) {
            const auto asTs = ParseIsoTimestamp(tok.text);
            if (asTs.has_value())
            {
                ts = asTs;
                return true;
            }
            const auto asNum = ParseDouble(tok.text);
            if (asNum.has_value())
            {
                num = asNum;
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
        // Both diagnostics below fire only when `secondTok` was
        // successfully classified (its parsed value populated
        // `tsMax` / `numMax`), so its offset is always available.
        // `value_or` keeps clang-tidy's bugprone-unchecked-optional
        // -access analysis happy without adding a branch clang can
        // constant-fold away.
        const auto secondOffset = secondTok.has_value() ? secondTok->offset : std::size_t{0};
        // Reject mixed-type bounds up-front. Otherwise the "Time or
        // Number" branch below picks Time and silently drops the
        // numeric bound (a hidden half-open range).
        if ((tsMin.has_value() && numMax.has_value()) || (numMin.has_value() && tsMax.has_value()))
        {
            QueryParseError err;
            err.offset = secondOffset;
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
            err.offset = secondOffset;
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

    [[nodiscard]] std::expected<FilterExpression, QueryParseError> FinishListLeaf(LeafRule rule, const Token &firstTok)
    {
        // All-boolean list (`[true, false]`) is the printer's wire
        // form for `Type::Boolean`; detect that so the round-trip
        // preserves the type. Mixed lists stay `Enumeration`.
        const auto isBoolKind = [](TokenKind k) { return k == TokenKind::True || k == TokenKind::False; };
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
                rule.filterBegin = asTs;
                rule.filterEnd = asTs;
                rule.filterValues.clear();
            }
            else if (const auto asNum = ParseDouble(only); asNum.has_value())
            {
                rule.type = LeafRule::Type::Number;
                rule.filterMinValue = asNum;
                rule.filterMaxValue = asNum;
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
    std::size_t mNesting = 0;
};

} // namespace

std::expected<FilterExpression, QueryParseError> ParseQuery(std::string_view input)
{
    Parser parser(input);
    return parser.Parse();
}

} // namespace loglib
