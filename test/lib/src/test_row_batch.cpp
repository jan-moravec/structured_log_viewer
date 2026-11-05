#include <loglib/core/row_batch.hpp>

#include <catch2/catch_all.hpp>

#include <string>

using loglib::FormatParseError;
using loglib::ParseError;

TEST_CASE("FormatParseError writes one line, a range and no line", "[row_batch]")
{
    CHECK(
        FormatParseError(ParseError{.firstLine = 42, .lastLine = 42, .message = "Unexpected token."}) ==
        "Error on line 42: Unexpected token."
    );
    CHECK(
        FormatParseError(
            ParseError{.firstLine = 3, .lastLine = 7, .message = "5 orphaned continuation lines dropped."}
        ) == "Error on lines 3-7: 5 orphaned continuation lines dropped."
    );
    CHECK(
        FormatParseError(ParseError{.message = "Regex parser requires a non-empty pattern."}) ==
        "Regex parser requires a non-empty pattern."
    );
}
