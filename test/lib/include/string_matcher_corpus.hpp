#pragma once

// Differential corpus for `MakeStringMatcher`. Expected flags are the
// Qt 6.11 `QRegularExpression` oracle (see `apptest_log_string_matcher`).
// `Wildcard` uses Qt's default conversion: anchored, and `*` / `?` do
// not cross `/` or `\`. Haystacks are judged after the GUI's
// `simplified()` normalisation; patterns are not.

#include <loglib/filter_expression.hpp>

#include <string_view>
#include <vector>

struct StringMatcherCase
{
    std::string_view id;
    loglib::LeafRule::Match match;
    std::string_view pattern;
    std::string_view haystack;
    bool expected;
};

[[nodiscard]] inline const std::vector<StringMatcherCase> &StringMatcherCorpus()
{
    using Match = loglib::LeafRule::Match;
    static const std::vector<StringMatcherCase> kCases = {
        // Exactly — ASCII, case, empty.
        {"exactly-ascii-equal", Match::Exactly, "error", "error", true},
        {"exactly-ascii-unequal", Match::Exactly, "error", "warn", false},
        {"exactly-case", Match::Exactly, "Error", "error", false},
        {"exactly-empty-both", Match::Exactly, "", "", true},
        {"exactly-empty-pattern", Match::Exactly, "", "x", false},
        {"exactly-empty-haystack", Match::Exactly, "x", "", false},

        // Exactly — whitespace. Patterns are not simplified.
        {"exactly-leading-space", Match::Exactly, "error", " error", true},
        {"exactly-trailing-space", Match::Exactly, "error", "error ", true},
        {"exactly-interior-collapse", Match::Exactly, "a b", "a   b", true},
        {"exactly-pattern-double-space", Match::Exactly, "a  b", "a b", false},
        {"exactly-pattern-trailing-space", Match::Exactly, "error ", "error", false},
        {"exactly-tab", Match::Exactly, "a b", "a\tb", true},
        {"exactly-newline", Match::Exactly, "a b", "a\nb", true},
        {"exactly-crlf", Match::Exactly, "a b", "a\r\nb", true},
        {"exactly-vertical-tab", Match::Exactly, "a b", "a\vb", true},
        {"exactly-form-feed", Match::Exactly, "a b", "a\fb", true},

        // Exactly — UTF-8, NUL, invalid UTF-8 (both sides become U+FFFD).
        {"exactly-utf8-equal", Match::Exactly, "caf\xc3\xa9", "caf\xc3\xa9", true},
        {"exactly-utf8-unequal", Match::Exactly, "caf\xc3\xa9", "cafe", false},
        {"exactly-utf8-case", Match::Exactly, "CAF\xc3\x89", "caf\xc3\xa9", false},
        {"exactly-nul", Match::Exactly, std::string_view("a\0b", 3), std::string_view("a\0b", 3), true},
        {"exactly-nul-vs-without", Match::Exactly, "ab", std::string_view("a\0b", 3), false},
        {"exactly-nul-adjacent", Match::Exactly, std::string_view("\0ab", 3), std::string_view("\0ab", 3), true},
        {"exactly-invalid-utf8-collapses", Match::Exactly, "\xff", "\xfe", true},
        {"exactly-del",
         Match::Exactly,
         std::string_view(
             "a\x7f"
             "b",
             3
         ),
         std::string_view(
             "a\x7f"
             "b",
             3
         ),
         true},

        // Contains.
        {"contains-substring", Match::Contains, "err", "error", true},
        {"contains-absent", Match::Contains, "zz", "error", false},
        {"contains-case", Match::Contains, "ERR", "error", false},
        {"contains-empty-pattern", Match::Contains, "", "abc", true},
        {"contains-empty-both", Match::Contains, "", "", true},
        {"contains-empty-haystack", Match::Contains, "a", "", false},
        {"contains-leading-space", Match::Contains, "err", "  error  ", true},
        {"contains-tab", Match::Contains, "a b", "xa\tbx", true},
        {"contains-pattern-double-space", Match::Contains, "a  b", "xa  bx", false},
        {"contains-utf8",
         Match::Contains,
         "\xc3\xbc",
         "\xc3\xbc"
         "ber",
         true},
        {"contains-utf8-multibyte", Match::Contains, "\xe4\xbd\xa0", "\xe4\xbd\xa0\xe5\xa5\xbd", true},
        {"contains-nul", Match::Contains, std::string_view("a\0", 2), std::string_view("xa\0y", 4), true},

        // Regular expressions: unanchored match, case-sensitive, invalid → none.
        {"regex-unanchored", Match::RegularExpression, "foo", "xxfooyy", true},
        {"regex-anchor-start", Match::RegularExpression, "^foo", "foo", true},
        {"regex-anchor-start-miss", Match::RegularExpression, "^foo", "xfoo", false},
        {"regex-anchor-end", Match::RegularExpression, "foo$", "foo", true},
        {"regex-anchor-end-miss", Match::RegularExpression, "foo$", "foobar", false},
        {"regex-anchor-both-after-ws", Match::RegularExpression, "^foo$", " foo\n", true},
        {"regex-alternation", Match::RegularExpression, "foo|bar", "bar", true},
        {"regex-alternation-miss", Match::RegularExpression, "foo|bar", "baz", false},
        {"regex-class", Match::RegularExpression, "[0-9]+", "id42", true},
        {"regex-class-miss", Match::RegularExpression, "[0-9]+", "id", false},
        {"regex-class-negated", Match::RegularExpression, "[^a]", "b", true},
        {"regex-case", Match::RegularExpression, "Error", "error", false},
        {"regex-dot", Match::RegularExpression, "a.c", "abc", true},
        {"regex-dot-sees-simplified-newline", Match::RegularExpression, "a.c", "a\nc", true},
        {"regex-escaped-dot", Match::RegularExpression, "a\\.c", "abc", false},
        {"regex-escaped-dot-hit", Match::RegularExpression, "a\\.c", "a.c", true},
        {"regex-invalid", Match::RegularExpression, "(", "x", false},
        {"regex-invalid-star", Match::RegularExpression, "*", "x", false},
        {"regex-empty-pattern", Match::RegularExpression, "", "abc", true},
        {"regex-empty-both", Match::RegularExpression, "", "", true},
        {"regex-utf8", Match::RegularExpression, "caf\xc3\xa9", "xxcaf\xc3\xa9yy", true},

        // Wildcards. Anchored; `*` and `?` do not match `/` or `\`.
        {"wildcard-exact", Match::Wildcard, "error", "error", true},
        {"wildcard-exact-miss", Match::Wildcard, "error", "errors", false},
        {"wildcard-star", Match::Wildcard, "err*", "error", true},
        {"wildcard-star-anchored", Match::Wildcard, "err*", "xerror", false},
        {"wildcard-star-middle", Match::Wildcard, "a*c", "abc", true},
        {"wildcard-star-empty-span", Match::Wildcard, "a*b", "ab", true},
        {"wildcard-star-needs-literal", Match::Wildcard, "a*b*c", "ac", false},
        {"wildcard-star-all", Match::Wildcard, "*", "anything", true},
        {"wildcard-star-not-slash", Match::Wildcard, "a*c", "ab/c", false},
        {"wildcard-star-not-backslash", Match::Wildcard, "a*c", "ab\\c", false},
        {"wildcard-question", Match::Wildcard, "err?r", "error", true},
        {"wildcard-question-short", Match::Wildcard, "err?r", "errr", false},
        {"wildcard-question-not-slash", Match::Wildcard, "a?c", "a/c", false},
        {"wildcard-class", Match::Wildcard, "file[123].log", "file2.log", true},
        {"wildcard-class-miss", Match::Wildcard, "file[123].log", "file4.log", false},
        {"wildcard-class-one-char", Match::Wildcard, "file[123].log", "file12.log", false},
        {"wildcard-dot-is-literal", Match::Wildcard, "a.txt", "abtxt", false},
        {"wildcard-dot-literal-hit", Match::Wildcard, "a.txt", "a.txt", true},
        {"wildcard-plus-is-literal", Match::Wildcard, "a+b", "a+b", true},
        {"wildcard-plus-not-any", Match::Wildcard, "a+b", "axb", false},
        {"wildcard-case", Match::Wildcard, "Error*", "error", false},
        {"wildcard-utf8-question", Match::Wildcard, "caf?", "caf\xc3\xa9", true},
        {"wildcard-question-two", Match::Wildcard, "?", "ab", false},
        {"wildcard-empty", Match::Wildcard, "", "", true},
        {"wildcard-empty-miss", Match::Wildcard, "", "a", false},
        {"wildcard-haystack-whitespace", Match::Wildcard, "a*", "  abc", true},
        {"wildcard-open-bracket", Match::Wildcard, "a[b", "a[b", false},
    };
    return kCases;
}
