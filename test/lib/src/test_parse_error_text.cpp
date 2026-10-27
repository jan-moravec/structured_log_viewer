#include <loglib/parse_error_text.hpp>

#include <catch2/catch_all.hpp>

#include <string>

using loglib::FormatLineError;
using loglib::FormatLineRangeError;
using loglib::LocateParseError;

TEST_CASE("FormatLineError round-trips through LocateParseError", "[parse_error_text]")
{
    const std::string text = FormatLineError(42, "Unexpected token.");
    CHECK(text == "Error on line 42: Unexpected token.");

    const auto location = LocateParseError(text);
    CHECK(location.firstLine == 42);
    CHECK(location.lastLine == 42);
    CHECK(location.message == "Unexpected token.");
}

TEST_CASE("FormatLineRangeError round-trips through LocateParseError", "[parse_error_text]")
{
    const std::string text = FormatLineRangeError(3, 7, "5 orphaned continuation lines dropped.");
    CHECK(text == "Error on lines 3-7: 5 orphaned continuation lines dropped.");

    const auto location = LocateParseError(text);
    CHECK(location.firstLine == 3);
    CHECK(location.lastLine == 7);
    CHECK(location.message == "5 orphaned continuation lines dropped.");
}

TEST_CASE("LocateParseError leaves unlocated text whole", "[parse_error_text]")
{
    for (const std::string text :
         {"Streaming parse failed: boom",
          "Error on line : missing number",
          "Error on line 0: zero is not a line",
          "Error on line 12 no colon",
          "Error on lines 9-3: reversed range",
          "Error on lines 4: missing range end",
          ""})
    {
        CAPTURE(text);
        const auto location = LocateParseError(text);
        CHECK(location.firstLine == 0);
        CHECK(location.lastLine == 0);
        CHECK(location.message == text);
    }
}
