// Golden-output tests of the parsers. Each case is an input and a parser. The dump of what the parser reports (rows,
// storage kinds, errors, line offsets, multi-line spans and key order) must equal the file kept in the repository, for
// the file pipeline at several batch sizes and thread counts and for the live-stream loop at several read sizes.
// Set LOGLIB_UPDATE_GOLDEN in the environment to write the files again.

#include "common.hpp"
#include "golden_dump.hpp"

#include <loglib/parse/parsers/csv_parser.hpp>
#include <loglib/parse/parsers/json_parser.hpp>
#include <loglib/parse/parsers/logfmt_parser.hpp>
#include <loglib/parse/parsers/regex_parser.hpp>
#include <loglib/parse/regex_templates.hpp>

#include <catch2/catch_all.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace loglib;

namespace
{

struct GoldenCase
{
    GoldenCase(
        std::string caseName,
        std::shared_ptr<const LogParser> caseParser,
        std::string caseInput,
        std::vector<std::string> caseTimeKeys = {}
    )
        : name(std::move(caseName)),
          parser(std::move(caseParser)),
          input(std::move(caseInput)),
          timeKeys(std::move(caseTimeKeys))
    {
    }

    std::string name;
    std::shared_ptr<const LogParser> parser;
    std::string input;
    std::vector<std::string> timeKeys;
};

/**
 * Cases whose file output changes when a batch holds a single line: a record held across a batch edge reports its
 * dropped continuation differently from one inside a batch. They run with batches of at least 64 bytes.
 */
bool NeedsLargeBatches(const std::string &name)
{
    return name == "logfmt_orphan_after_error" || name == "logfmt_continuation_targets" ||
           name == "regex_optional_last_group";
}

/** The input made of count blocks, each built from its index. */
std::string Repeat(int count, const std::function<std::string(int)> &block)
{
    std::string text;
    for (int i = 0; i < count; ++i)
    {
        text += block(i);
    }
    return text;
}

std::string ReadFile(const std::filesystem::path &path)
{
    std::ostringstream text;
    text << std::ifstream(path, std::ios::binary).rdbuf();
    return text.str();
}

/** Drops carriage returns, so a golden file checked out with other line endings still compares equal. */
std::string WithoutCarriageReturns(std::string text)
{
    std::erase(text, '\r');
    return text;
}

std::string CrLf(const std::string &text)
{
    std::string out;
    for (const char c : text)
    {
        if (c == '\n')
        {
            out += '\r';
        }
        out += c;
    }
    return out;
}

void AddRegexCases(std::vector<GoldenCase> &cases)
{
    for (const RegexTemplate &tmpl : RegexTemplateCatalog::Builtin().Builtins())
    {
        std::string name;
        for (const char c : tmpl.name)
        {
            name += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
        }
        const auto parser = std::make_shared<RegexParser>(tmpl.regex);
        std::string samples;
        for (const std::string &line : tmpl.sampleLines)
        {
            samples += line + "\n";
        }
        // Samples, a blank line, a line that does not match, and the samples again so several batches hold rows.
        std::string input = samples;
        input += "\nthis line matches no template\n";
        input += samples;
        input += samples;
        cases.emplace_back("regex_template_" + name, parser, input);
    }

    const RegexTemplate *java = nullptr;
    for (const RegexTemplate &tmpl : RegexTemplateCatalog::Builtin().Builtins())
    {
        if (tmpl.name == "Java / log4j / SLF4J Logback")
        {
            java = &tmpl;
        }
    }
    REQUIRE(java != nullptr);
    const auto javaParser = std::make_shared<RegexParser>(java->regex);
    const std::string &javaHeader = java->sampleLines.front();
    const std::string javaTrace = Repeat(12, [&](int i) {
        std::string record = javaHeader + "\n";
        if (i % 3 == 0)
        {
            record += "\tat com.example.Service.run(Service.java:" + std::to_string(i) + ")\n";
            record += "\tat com.example.Main.main(Main.java:1)\n";
        }
        if (i % 4 == 1)
        {
            record += "    at indented with spaces\n\n\tafter a blank line\n";
        }
        if (i % 5 == 2)
        {
            record += "not indented line is another record or an error\n";
        }
        return record;
    });
    cases.emplace_back("regex_java_multiline", javaParser, javaTrace);
    cases.emplace_back("regex_java_orphan_start", javaParser, "\tat orphan 1\n\n\tat orphan 2\n" + javaTrace);
    cases.emplace_back("regex_java_no_final_newline", javaParser, javaTrace.substr(0, javaTrace.size() - 1));
    cases.emplace_back("regex_java_crlf", javaParser, CrLf(javaTrace));

    const std::string pythonPattern =
        R"(^(?<level>TRACE|DEBUG|INFO|WARN|WARNING|ERROR|CRITICAL|FATAL): (?<message>.*)$)";
    const std::string pythonAnchor = R"(^(?:TRACE|DEBUG|INFO|WARN|WARNING|ERROR|CRITICAL|FATAL): )";
    const std::string traceback = Repeat(14, [](int i) {
        std::string record = "INFO: record " + std::to_string(i) + "\n";
        if (i % 3 == 0)
        {
            record += "Traceback (most recent call last):\n  File \"x.py\", line " + std::to_string(i) +
                      ", in <module>\n    run()\nValueError: boom\n";
        }
        if (i % 5 == 4)
        {
            record += "\nERROR: after a blank line\n";
        }
        return record;
    });
    const auto untilHeader = std::make_shared<RegexParser>(
        RegexSettings{.pattern = pythonPattern, .continuation = ContinuationMode::UntilNextHeader}
    );
    const auto untilHeaderAnchored = std::make_shared<RegexParser>(RegexSettings{
        .pattern = pythonPattern, .continuation = ContinuationMode::UntilNextHeader, .headerAnchor = pythonAnchor
    });
    cases.emplace_back("regex_python_traceback", untilHeader, traceback);
    cases.emplace_back("regex_python_traceback_anchored", untilHeaderAnchored, traceback);
    cases.emplace_back("regex_python_orphan_start", untilHeader, "stray 1\nstray 2\n\nstray 3\n" + traceback);
    cases.emplace_back("regex_bom_and_crlf", untilHeader, "\xEF\xBB\xBF" + CrLf(traceback));

    const auto optionalGroup = std::make_shared<RegexParser>(RegexSettings{
        .pattern = R"(^(?<level>\w+) (?<code>\d+)(?: (?<note>.*))?$)", .continuation = ContinuationMode::Indented
    });
    cases.push_back(
        {"regex_optional_last_group",
         optionalGroup,
         Repeat(8, [](int i) { return std::format("INFO {}{}\n  continues {}\n", i, i % 2 == 0 ? " note" : "", i); }),
         {}}
    );
    cases.push_back(
        {"regex_numeric_last_group",
         std::make_shared<RegexParser>(RegexSettings{
             .pattern = R"(^(?<level>\w+) (?<message>.*) (?<code>\d+)$)", .continuation = ContinuationMode::Indented
         }),
         Repeat(6, [](int i) { return std::format("INFO text {}\n  more {}\n", i, i); }),
         {}}
    );
    cases.emplace_back("regex_empty_pattern", std::make_shared<RegexParser>(RegexSettings{}), "a\nb\n");
}

void AddJsonCases(std::vector<GoldenCase> &cases)
{
    const auto json = std::make_shared<JsonParser>();
    cases.push_back(
        {"json_basic",
         json,
         Repeat(
             40,
             [](int i) {
                 return std::format(
                     R"({{"timestamp":"2026-01-01T00:00:{:02}.123+02:00","level":"info","n":{},"ok":true,"ratio":{}.5,"none":null,"text":"row {}"}})"
                     "\n",
                     i % 60,
                     i,
                     i,
                     i
                 );
             }
         ),
         {}}
    );
    cases.push_back(
        {"json_time_columns",
         json,
         Repeat(
             30,
             [](int i) {
                 return std::format(
                     R"({{"timestamp":"2026-01-01T00:00:{:02}Z","n":{}}})"
                     "\n",
                     i % 60,
                     i
                 );
             }
         ) + "{\"timestamp\":\"not a time\",\"n\":1}\n",
         {"timestamp"}}
    );
    cases.push_back(
        {"json_escapes_and_types",
         json,
         "{\"a\":\"quote \\\" slash \\\\ newline \\n tab \\t unicode \\u00e9 \\ud83d\\ude00\"}\n"
         "{\"k\\u0065y\":\"escaped "
         "key\",\"big\":18446744073709551615,\"neg\":-5,\"huge\":123456789012345678901234567890}\n"
         "{\"arr\":[1,2,{\"x\":3}],\"obj\":{\"y\":[true,null]},\"empty\":\"\"}\n"
         "{}\n"
         "[1,2,3]\n"
         "\"just a string\"\n"
         "42\n"
         "{\"broken\": \n"
         "{\"a\":1} trailing\n"
         "{\"dup\":1,\"dup\":2}\n",
         {}}
    );
    const std::string rows = Repeat(25, [](int i) { return std::format("{{\"n\":{},\"s\":\"v{}\"}}\n", i, i); });
    cases.emplace_back("json_blank_lines", json, "\n\n" + rows + "\n\n" + rows + "\n");
    cases.emplace_back("json_no_final_newline", json, rows.substr(0, rows.size() - 1));
    cases.emplace_back("json_crlf", json, CrLf(rows));
    cases.emplace_back("json_bom", json, "\xEF\xBB\xBF" + rows);
    cases.emplace_back("json_empty", json, "");
    cases.emplace_back("json_only_blank", json, "\n\n\n");
}

void AddLogfmtCases(std::vector<GoldenCase> &cases)
{
    const auto multiline = std::make_shared<LogfmtParser>(true);
    const auto single = std::make_shared<LogfmtParser>(false);
    const std::string rows = Repeat(30, [](int i) {
        return std::format("ts=2026-01-01T00:00:{:02}Z level=info n={} msg=\"row {}\"\n", i % 60, i, i);
    });
    cases.emplace_back("logfmt_basic", multiline, rows);
    cases.emplace_back("logfmt_single_line_mode", single, rows + "  indented is a record start\n");
    cases.emplace_back("logfmt_time_columns", multiline, rows, std::vector<std::string>{"ts"});
    const std::string continued = Repeat(24, [](int i) {
        std::string record = std::format("level=info n={} msg=\"row {}\"\n", i, i);
        if (i % 3 == 0)
        {
            record += "  continuation of " + std::to_string(i) + "\n\n  and more\n";
        }
        if (i % 7 == 1)
        {
            record += "\t tab continuation\n";
        }
        return record;
    });
    cases.emplace_back("logfmt_multiline", multiline, continued);
    cases.emplace_back("logfmt_orphan_start", multiline, "  orphan one\n\n  orphan two\n" + continued);
    cases.push_back(
        {"logfmt_orphan_after_error", multiline, "bad=\"unterminated\n  orphan\nok=1 last=text\n  fine\n", {}}
    );
    cases.emplace_back("logfmt_no_final_newline", multiline, continued.substr(0, continued.size() - 1));
    cases.emplace_back("logfmt_crlf", multiline, CrLf(continued));
    cases.emplace_back("logfmt_bom", multiline, "\xEF\xBB\xBF" + continued);
    cases.push_back(
        {"logfmt_continuation_targets",
         multiline,
         "a=1 n=5\n  after a number\nb=2 t=\n  after an empty value\nc=3 s=\"text\"\n  after a string\n"
         "d=4 e=true\n  after a bool\n\nf=6 g=x\n\n\n  after blanks\n",
         {}}
    );
    cases.push_back(
        {"logfmt_quotes_and_errors",
         multiline,
         "a=\"esc \\\" \\\\ \\n \\t\" b=bare c= d e=\"\" f=1.5 g=-3 h=true i=false j=null\n"
         "no pairs here\n"
         "=stray key\n"
         "k=\"unterminated\n"
         "z=1\n",
         {}}
    );
}

void AddCsvCases(std::vector<GoldenCase> &cases)
{
    const auto csv = std::make_shared<CsvParser>();
    const std::string rows = Repeat(30, [](int i) {
        return std::format("2026-01-01T00:00:{:02}Z,{},\"quoted, {}\",{}.5,true\n", i % 60, i, i, i);
    });
    const std::string basic = "timestamp,n,text,ratio,flag\n" + rows;
    cases.emplace_back("csv_basic", csv, basic);
    cases.emplace_back("csv_time_columns", csv, basic, std::vector<std::string>{"timestamp"});
    cases.emplace_back("csv_bom", csv, "\xEF\xBB\xBF" + basic);
    cases.emplace_back("csv_crlf", csv, CrLf(basic));
    cases.emplace_back("csv_no_final_newline", csv, basic.substr(0, basic.size() - 1));
    cases.emplace_back("csv_leading_blank_lines", csv, "\n\n" + basic);
    cases.emplace_back("csv_duplicate_headers", csv, "a,b,a,a_2,b\n1,2,3,4,5\n6,7,8,9,10\n");
    cases.emplace_back("csv_header_only", csv, "a,b,c\n");
    cases.emplace_back("csv_empty_cells", csv, "a,b,c\n,,\n1,,3\n\"\",x,\n");
    cases.push_back(
        {"csv_errors",
         csv,
         "a,b,c\n1,2,3\n1,2\n1,2,3,4,5\n\"unterminated,2,3\n\"quoted \"\"inner\"\" \",2,3\n7,8,9\n",
         {}}
    );
    cases.emplace_back("csv_no_header_cells", csv, "\"\n1,2\n");
    cases.emplace_back("csv_empty", csv, "");
}

void AddFuzzCorpusCases(std::vector<GoldenCase> &cases)
{
    const std::filesystem::path root = LOGLIB_TEST_FUZZ_CORPUS_DIR;
    const struct
    {
        const char *folder;
        std::shared_ptr<const LogParser> parser;
    } formats[] = {
        {"json", std::make_shared<JsonParser>()},
        {"logfmt", std::make_shared<LogfmtParser>()},
        {"csv", std::make_shared<CsvParser>()},
    };
    for (const auto &format : formats)
    {
        std::vector<std::filesystem::path> files;
        for (const auto &entry : std::filesystem::directory_iterator(root / format.folder))
        {
            if (entry.is_regular_file() && entry.file_size() <= 8192)
            {
                files.push_back(entry.path());
            }
        }
        std::ranges::sort(files);
        if (files.size() > 4)
        {
            files.resize(4);
        }
        int index = 0;
        for (const auto &path : files)
        {
            cases.emplace_back(std::format("fuzz_{}_{}", format.folder, index++), format.parser, ReadFile(path));
        }
    }
}

std::vector<GoldenCase> BuildCases()
{
    std::vector<GoldenCase> cases;
    AddJsonCases(cases);
    AddLogfmtCases(cases);
    AddCsvCases(cases);
    AddRegexCases(cases);
    AddFuzzCorpusCases(cases);
    return cases;
}

/** True when LOGLIB_UPDATE_GOLDEN is set, which makes the tests write the golden files instead of reading them. */
bool UpdateRequested()
{
#ifdef _WIN32
    char *value = nullptr;
    size_t length = 0;
    const bool set = _dupenv_s(&value, &length, "LOGLIB_UPDATE_GOLDEN") == 0 && value != nullptr;
    std::free(value);
    return set;
#else
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    return std::getenv("LOGLIB_UPDATE_GOLDEN") != nullptr;
#endif
}

void CompareWithGolden(
    const std::string &caseName,
    const std::string &mode,
    const std::string &actual,
    const std::string &description,
    bool canonical
)
{
    const std::filesystem::path path = std::filesystem::path(LOGLIB_TEST_GOLDEN_DIR) / (caseName + "." + mode + ".txt");
    if (UpdateRequested())
    {
        // Only the default run writes the file, so the other runs are compared with it afterwards.
        if (canonical)
        {
            std::filesystem::create_directories(path.parent_path());
            std::ofstream out(path, std::ios::binary);
            out << actual;
        }
        return;
    }
    INFO(description << " " << path.string());
    REQUIRE(std::filesystem::exists(path));
    CHECK(WithoutCarriageReturns(ReadFile(path)) == actual);
}

} // namespace

TEST_CASE("Parsers report the same output as the golden files through the file pipeline", "[golden_output][file]")
{
    InitializeTimezoneData();
    for (const GoldenCase &testCase : BuildCases())
    {
        for (const std::size_t batchBytes :
             {std::size_t{1}, std::size_t{7}, std::size_t{64}, std::size_t{4096}, std::size_t{0}})
        {
            if (batchBytes != 0 && batchBytes < 64 && NeedsLargeBatches(testCase.name))
            {
                continue;
            }
            for (const unsigned threads : {1U, 2U, 8U})
            {
                const std::string dump = test_support::DumpFileParse(
                    *testCase.parser, testCase.input, testCase.timeKeys, threads, batchBytes
                );
                CompareWithGolden(
                    testCase.name,
                    "file",
                    dump,
                    std::format("{} batch bytes {} threads {}", testCase.name, batchBytes, threads),
                    batchBytes == 0 && threads == 1
                );
            }
        }
    }
}

TEST_CASE("Parsers report the same output as the golden files through the stream loop", "[golden_output][stream]")
{
    InitializeTimezoneData();
    for (const GoldenCase &testCase : BuildCases())
    {
        for (const std::size_t chunkBytes : {std::size_t{1}, std::size_t{7}, std::size_t{4096}})
        {
            const std::string dump =
                test_support::DumpStreamParse(*testCase.parser, testCase.input, testCase.timeKeys, chunkBytes);
            CompareWithGolden(
                testCase.name,
                "stream",
                dump,
                std::format("{} chunk bytes {}", testCase.name, chunkBytes),
                chunkBytes == 4096
            );
        }
    }
}
