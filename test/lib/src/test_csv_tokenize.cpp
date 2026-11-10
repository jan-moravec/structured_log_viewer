#include <loglib/parse/internal/csv_tokenize.hpp>

#include <catch2/catch_all.hpp>

#include <string>
#include <string_view>
#include <vector>

using loglib::internal::CsvCell;
using loglib::internal::TokenizeCsvLine;

namespace
{

struct Tokenized
{
    bool complete = false;
    std::vector<std::string> values;
    std::vector<bool> quoted;
    std::vector<bool> fromScratch;
};

Tokenized Tokenize(std::string_view line)
{
    Tokenized result;
    std::string scratch;
    result.complete = TokenizeCsvLine(line, scratch, [&](const CsvCell &cell) {
        result.values.emplace_back(cell.value);
        result.quoted.push_back(cell.wasQuoted);
        result.fromScratch.push_back(cell.fromScratch);
    });
    return result;
}

using Strings = std::vector<std::string>;
using Flags = std::vector<bool>;

} // namespace

TEST_CASE("TokenizeCsvLine splits plain cells", "[csv_tokenize]")
{
    const Tokenized result = Tokenize("a,b,c");
    CHECK(result.complete);
    CHECK(result.values == Strings{"a", "b", "c"});
    CHECK(result.quoted == Flags{false, false, false});
}

TEST_CASE("TokenizeCsvLine gives one empty cell for an empty line", "[csv_tokenize]")
{
    const Tokenized result = Tokenize("");
    CHECK(result.complete);
    CHECK(result.values == Strings{""});
}

TEST_CASE("TokenizeCsvLine keeps empty cells", "[csv_tokenize]")
{
    const Tokenized result = Tokenize(",,x,,");
    CHECK(result.complete);
    CHECK(result.values == Strings{"", "", "x", "", ""});
}

TEST_CASE("TokenizeCsvLine adds an empty cell after a trailing separator", "[csv_tokenize]")
{
    CHECK(Tokenize("a,b,").values == Strings{"a", "b", ""});
    CHECK(Tokenize("a,").values == Strings{"a", ""});
    CHECK(Tokenize(",").values == Strings{"", ""});
    CHECK(Tokenize("\"a\",").values == Strings{"a", ""});
}

TEST_CASE("TokenizeCsvLine reads a quoted cell with a separator inside", "[csv_tokenize]")
{
    const Tokenized result = Tokenize("\"a,b\",c");
    CHECK(result.complete);
    CHECK(result.values == Strings{"a,b", "c"});
    CHECK(result.quoted == Flags{true, false});
    CHECK(result.fromScratch == Flags{false, false});
}

TEST_CASE("TokenizeCsvLine unescapes doubled quotes through the scratch string", "[csv_tokenize]")
{
    const Tokenized result = Tokenize(R"csv("say ""hi""","""")csv");
    CHECK(result.complete);
    CHECK(result.values == Strings{"say \"hi\"", "\""});
    CHECK(result.fromScratch == Flags{true, true});
}

TEST_CASE("TokenizeCsvLine reads an empty quoted cell", "[csv_tokenize]")
{
    const Tokenized result = Tokenize("\"\",x");
    CHECK(result.complete);
    CHECK(result.values == Strings{"", "x"});
    CHECK(result.quoted == Flags{true, false});
}

TEST_CASE("TokenizeCsvLine keeps a quote inside an unquoted cell", "[csv_tokenize]")
{
    const Tokenized result = Tokenize("ab\"c,d");
    CHECK(result.complete);
    CHECK(result.values == Strings{"ab\"c", "d"});
}

TEST_CASE("TokenizeCsvLine skips bytes after a closing quote", "[csv_tokenize]")
{
    CHECK(Tokenize("\"a\"x,b").values == Strings{"a", "b"});
    CHECK(Tokenize("\"a\"x").values == Strings{"a"});
}

TEST_CASE("TokenizeCsvLine reports an unterminated quote", "[csv_tokenize]")
{
    CHECK_FALSE(Tokenize("\"abc").complete);
    CHECK_FALSE(Tokenize("a,\"b,c").complete);
    CHECK_FALSE(Tokenize("\"").complete);
    // A doubled quote at the end does not close the cell.
    CHECK_FALSE(Tokenize("\"abc\"\"").complete);

    // The cells before the broken one were still delivered.
    const Tokenized partial = Tokenize("a,\"b");
    CHECK(partial.values == Strings{"a"});
}
