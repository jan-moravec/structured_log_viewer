#include "common.hpp"

#include <loglib/file_line_source.hpp>
#include <loglib/internal/buffering_sink.hpp>
#include <loglib/log_data.hpp>
#include <loglib/log_file.hpp>
#include <loglib/log_parser.hpp>
#include <loglib/parse_file.hpp>
#include <loglib/parser_options.hpp>
#include <loglib/parsers/logfmt_parser.hpp>

#include <catch2/catch_all.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <variant>
#include <vector>

namespace
{

/// Tiny RAII helper for raw-text fixture files. `TestJsonLogFile`
/// always wraps lines in JSON-aware machinery, so logfmt fixtures get
/// their own minimal helper that just writes the bytes.
class TestLogfmtFile
{
public:
    explicit TestLogfmtFile(std::string content, std::string path = "test.logfmt")
        : mPath(std::move(path))
    {
        std::ofstream out(mPath, std::ios::binary);
        REQUIRE(out.is_open());
        out << content;
    }

    ~TestLogfmtFile() noexcept
    {
        std::error_code ec;
        std::filesystem::remove(mPath, ec);
    }

    TestLogfmtFile(const TestLogfmtFile &) = delete;
    TestLogfmtFile &operator=(const TestLogfmtFile &) = delete;
    TestLogfmtFile(TestLogfmtFile &&) = delete;
    TestLogfmtFile &operator=(TestLogfmtFile &&) = delete;

    [[nodiscard]] const std::string &Path() const noexcept
    {
        return mPath;
    }

private:
    std::string mPath;
};

} // namespace

TEST_CASE("Validate non-existent file [logfmt]", "[logfmt_parser]")
{
    const loglib::LogfmtParser parser;
    CHECK_FALSE(parser.IsValid("non_existent_file.logfmt"));
}

TEST_CASE("Validate empty file [logfmt]", "[logfmt_parser]")
{
    const loglib::LogfmtParser parser;
    const TestLogfmtFile file("");
    CHECK_FALSE(parser.IsValid(file.Path()));
}

TEST_CASE("Validate JSON-shaped first line [logfmt]", "[logfmt_parser]")
{
    // A line that looks like JSON must be rejected so JSON keeps
    // priority in the auto-detect loop.
    const loglib::LogfmtParser parser;
    const TestLogfmtFile file(R"({"key": "value"})"
                              "\n");
    CHECK_FALSE(parser.IsValid(file.Path()));
}

TEST_CASE("Validate logfmt-shaped first line [logfmt]", "[logfmt_parser]")
{
    const loglib::LogfmtParser parser;
    const TestLogfmtFile file("level=info msg=\"hello world\"\n");
    CHECK(parser.IsValid(file.Path()));
}

TEST_CASE("Validate file with no key=value pair [logfmt]", "[logfmt_parser]")
{
    const loglib::LogfmtParser parser;
    const TestLogfmtFile file("just some plain text without equals\n");
    CHECK_FALSE(parser.IsValid(file.Path()));
}

TEST_CASE("Parse single bare key/value pair [logfmt]", "[logfmt_parser]")
{
    const loglib::LogfmtParser parser;
    const TestLogfmtFile file("key=value\n");

    auto result = loglib::ParseFile(parser, file.Path());
    CHECK(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 1);
    CHECK(loglib::AsStringView(result.data.Lines()[0].GetValue("key")) == std::string_view{"value"});
}

TEST_CASE("Parse typed bare values [logfmt]", "[logfmt_parser]")
{
    const loglib::LogfmtParser parser;
    const TestLogfmtFile file("i=-12 u=10000000000000000000 d=3.14 b=true f=false n=\n");

    auto result = loglib::ParseFile(parser, file.Path());
    REQUIRE(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 1);

    const auto values = result.data.Lines()[0].Values();
    CHECK(std::get<int64_t>(values.at("i")) == -12);
    CHECK(std::get<uint64_t>(values.at("u")) == 10000000000000000000ULL);
    CHECK(std::get<double>(values.at("d")) == Catch::Approx(3.14));
    CHECK(std::get<bool>(values.at("b")) == true);
    CHECK(std::get<bool>(values.at("f")) == false);
    CHECK(std::holds_alternative<std::monostate>(values.at("n")));
}

TEST_CASE("Quoted values stay strings [logfmt]", "[logfmt_parser]")
{
    // `pid="42"` must round-trip as the literal string "42", not int.
    const loglib::LogfmtParser parser;
    const TestLogfmtFile file("pid=\"42\" msg=\"hello world\"\n");

    auto result = loglib::ParseFile(parser, file.Path());
    REQUIRE(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 1);

    const auto values = result.data.Lines()[0].Values();
    CHECK(loglib::AsStringView(values.at("pid")) == std::string_view{"42"});
    CHECK(loglib::AsStringView(values.at("msg")) == std::string_view{"hello world"});
}

TEST_CASE("Quoted value with C-style escapes [logfmt]", "[logfmt_parser]")
{
    const loglib::LogfmtParser parser;
    const TestLogfmtFile file("msg=\"a \\\"quoted\\\" word\\nnew line\\ttab\\\\back\"\n");

    auto result = loglib::ParseFile(parser, file.Path());
    REQUIRE(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 1);

    const auto values = result.data.Lines()[0].Values();
    const std::string expected = "a \"quoted\" word\nnew line\ttab\\back";
    CHECK(loglib::AsStringView(values.at("msg")) == std::string_view{expected});
}

TEST_CASE("Unterminated quoted value reports a parse error [logfmt]", "[logfmt_parser]")
{
    const loglib::LogfmtParser parser;
    const TestLogfmtFile file("key=\"unterminated\n");

    auto result = loglib::ParseFile(parser, file.Path());
    CHECK(result.errors.size() == 1);
    CHECK(result.errors[0].contains("Unterminated quoted value"));
}

TEST_CASE("Bare key with no '=' is treated as null [logfmt]", "[logfmt_parser]")
{
    const loglib::LogfmtParser parser;
    const TestLogfmtFile file("flag level=info\n");

    auto result = loglib::ParseFile(parser, file.Path());
    REQUIRE(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 1);

    const auto values = result.data.Lines()[0].Values();
    CHECK(std::holds_alternative<std::monostate>(values.at("flag")));
    CHECK(loglib::AsStringView(values.at("level")) == std::string_view{"info"});
}

TEST_CASE("Repeated keys: last write wins [logfmt]", "[logfmt_parser]")
{
    const loglib::LogfmtParser parser;
    const TestLogfmtFile file("key=first key=second key=third\n");

    auto result = loglib::ParseFile(parser, file.Path());
    REQUIRE(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 1);
    CHECK(loglib::AsStringView(result.data.Lines()[0].GetValue("key")) == std::string_view{"third"});
}

TEST_CASE("Multiple lines parse independently [logfmt]", "[logfmt_parser]")
{
    const loglib::LogfmtParser parser;
    const TestLogfmtFile file("level=info msg=\"first line\"\n"
                              "level=warn msg=\"second line\" code=42\n"
                              "level=error msg=\"third line\"\n");

    auto result = loglib::ParseFile(parser, file.Path());
    CHECK(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 3);

    CHECK(loglib::AsStringView(result.data.Lines()[0].GetValue("level")) == std::string_view{"info"});
    CHECK(loglib::AsStringView(result.data.Lines()[1].GetValue("msg")) == std::string_view{"second line"});
    CHECK(std::get<uint64_t>(result.data.Lines()[1].GetValue("code")) == 42u);
    CHECK(loglib::AsStringView(result.data.Lines()[2].GetValue("level")) == std::string_view{"error"});
}

TEST_CASE("Blank lines are skipped [logfmt]", "[logfmt_parser]")
{
    const loglib::LogfmtParser parser;
    const TestLogfmtFile file("\n\nkey=value\n\n");

    auto result = loglib::ParseFile(parser, file.Path());
    CHECK(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 1);
    CHECK(loglib::AsStringView(result.data.Lines()[0].GetValue("key")) == std::string_view{"value"});
}

TEST_CASE("Last line lacks trailing newline [logfmt]", "[logfmt_parser]")
{
    const loglib::LogfmtParser parser;
    const TestLogfmtFile file("a=1\nb=2");

    auto result = loglib::ParseFile(parser, file.Path());
    CHECK(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 2);
    CHECK(std::get<uint64_t>(result.data.Lines()[0].GetValue("a")) == 1u);
    CHECK(std::get<uint64_t>(result.data.Lines()[1].GetValue("b")) == 2u);
}

TEST_CASE("Plain text is parsed permissively as null-valued bare keys [logfmt]", "[logfmt_parser]")
{
    // kr/logfmt grammar treats every whitespace-separated word as a
    // (key, null) pair, so a line of plain prose is "valid" logfmt
    // with N null-valued bare keys. We mirror that behaviour rather
    // than surfacing it as a parse error.
    const loglib::LogfmtParser parser;
    const TestLogfmtFile file("level=info\nplain text line\nlevel=error\n");

    auto result = loglib::ParseFile(parser, file.Path());
    CHECK(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 3);
    CHECK(std::holds_alternative<std::monostate>(result.data.Lines()[1].GetValue("plain")));
    CHECK(std::holds_alternative<std::monostate>(result.data.Lines()[1].GetValue("text")));
}

TEST_CASE("Line of only '=' / '\"' surfaces as parse error [logfmt]", "[logfmt_parser]")
{
    // No emitable key/value pairs: the scanner should report the line
    // rather than silently produce an empty record.
    const loglib::LogfmtParser parser;
    const TestLogfmtFile file("level=info\n====\nlevel=error\n");

    auto result = loglib::ParseFile(parser, file.Path());
    REQUIRE(result.errors.size() == 1);
    CHECK(result.errors[0].contains("line 2"));
    CHECK(result.errors[0].contains("Not a logfmt record"));
    REQUIRE(result.data.Lines().size() == 2);
}

TEST_CASE("ToString round-trips bare and quoted values [logfmt]", "[logfmt_parser]")
{
    const loglib::LogfmtParser parser;
    const TestLogfmtFile file("level=info msg=\"hello world\" code=42 ratio=3.14\n");

    auto result = loglib::ParseFile(parser, file.Path());
    REQUIRE(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 1);

    const std::string out = parser.ToString(result.data.Lines()[0]);
    // IndexedValues are KeyId-ordered (insertion order through `KeyIndex`).
    CHECK(out.contains("level=info"));
    CHECK(out.contains("msg=\"hello world\""));
    CHECK(out.contains("code=42"));
    CHECK(out.contains("ratio=3.14"));
}

TEST_CASE("ToString quotes values with whitespace and special bytes [logfmt]", "[logfmt_parser]")
{
    using namespace loglib;
    const std::string emitted = LogfmtParser::ToString(LogMap{
        {"safe", LogValue{std::string_view{"value"}}},
        {"spaced", LogValue{std::string_view{"hello world"}}},
        {"with_quote", LogValue{std::string_view{"a\"b"}}},
        {"with_newline", LogValue{std::string_view{"line1\nline2"}}}
    });

    CHECK(emitted.contains("safe=value"));
    CHECK(emitted.contains("spaced=\"hello world\""));
    CHECK(emitted.contains("with_quote=\"a\\\"b\""));
    CHECK(emitted.contains("with_newline=\"line1\\nline2\""));
}

TEST_CASE("Parse file via FactoryParser auto-detect [logfmt]", "[logfmt_parser]")
{
    const TestLogfmtFile file("level=info msg=\"hello\"\n");
    auto result = loglib::ParseFile(file.Path());
    CHECK(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 1);
    CHECK(loglib::AsStringView(result.data.Lines()[0].GetValue("msg")) == std::string_view{"hello"});
}
