#include "text_table.hpp"

#include <catch2/catch_all.hpp>

TEST_CASE("TextTable keeps spaces that belong to the last cell", "[cli][text_table]")
{
    slp::TextTable table({"name", "value"});
    table.AddRow({"a", "x "});
    table.AddRow({"bb", "y"});
    CHECK(table.Render() == "name  value\na     x \nbb    y\n");
}

TEST_CASE("TextTable drops trailing empty cells and their padding", "[cli][text_table]")
{
    slp::TextTable table({"name", "value", "bar"});
    table.AddRow({"a", "1", ""});
    table.AddRow({"bb", "22"});
    table.SetCell(1, 2, "##");
    CHECK(table.Render() == "name  value  bar\na     1\nbb    22     ##\n");
}
