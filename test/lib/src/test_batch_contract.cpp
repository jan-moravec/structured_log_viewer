#include "common.hpp"

#include <loglib/core/file_line_source.hpp>
#include <loglib/core/key_index.hpp>
#include <loglib/core/log_file.hpp>
#include <loglib/core/row_batch.hpp>
#include <loglib/core/stream_line_source.hpp>
#include <loglib/parse/log_parse_sink.hpp>
#include <loglib/parse/log_parser.hpp>
#include <loglib/parse/parser_options.hpp>
#include <loglib/parse/parsers/csv_parser.hpp>
#include <loglib/parse/parsers/json_parser.hpp>
#include <loglib/parse/parsers/logfmt_parser.hpp>
#include <loglib/parse/parsers/regex_parser.hpp>
#include <loglib/schema/source_descriptor.hpp>

#include <catch2/catch_all.hpp>

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace loglib;

namespace
{

/** Keeps every batch of a parse, in order, with the number of OnStarted and OnFinished calls. */
class RecordingSink final : public LogParseSink
{
public:
    KeyIndex &Keys() override
    {
        return mKeys;
    }

    void OnStarted() override
    {
        ++started;
    }

    void OnBatch(RowBatch batch) override
    {
        batches.push_back(std::move(batch));
    }

    void OnFinished(bool cancelled) override
    {
        ++finished;
        wasCancelled = cancelled;
    }

    [[nodiscard]] std::size_t LastLineNumber() const
    {
        return batches.empty() ? 0 : batches.back().lastLineNumber;
    }

    [[nodiscard]] std::vector<ParseError> Errors() const
    {
        std::vector<ParseError> errors;
        for (const RowBatch &batch : batches)
        {
            errors.insert(errors.end(), batch.errors.begin(), batch.errors.end());
        }
        return errors;
    }

    [[nodiscard]] std::size_t RowCount() const
    {
        std::size_t rows = 0;
        for (const RowBatch &batch : batches)
        {
            rows += batch.lines.size();
        }
        return rows;
    }

    std::vector<RowBatch> batches;
    int started = 0;
    int finished = 0;
    bool wasCancelled = false;

private:
    KeyIndex mKeys;
};

/** Runs a parser over bytes as a file, with a given Stage A batch size, and keeps the sink for inspection. */
void ParseAsFile(
    const LogParser &parser,
    const std::string &fileName,
    const std::string &bytes,
    std::size_t batchSizeBytes,
    RecordingSink &sink,
    unsigned int threads = 0
)
{
    const TestLogFile file(fileName);
    file.Write(bytes);
    auto source = file.CreateFileLineSource();
    ParserOptions options;
    options.batchSizeBytes = batchSizeBytes;
    options.threads = threads;
    parser.ParseStreaming(*source, sink, options);
}

/** Runs a parser over bytes as a stream, delivered in chunks, and keeps the sink for inspection. */
void ParseAsStream(
    const LogParser &parser,
    const std::string &bytes,
    std::size_t chunkSize,
    RecordingSink &sink,
    ParserOptions options = {}
)
{
    StreamLineSource source("memory.log", std::make_unique<InMemoryBytesProducer>(bytes, chunkSize));
    parser.ParseStreaming(source, sink, options);
}

} // namespace

TEST_CASE("The last batch of a parse holds the number of lines read", "[batch_contract][line_count]")
{
    const JsonParser json;
    const LogfmtParser logfmt;

    struct Case
    {
        const char *name;
        const LogParser *parser;
        std::string bytes;
        std::size_t lines;
        std::size_t rows;
    };
    const std::vector<Case> cases = {
        {"three JSON lines", &json, "{\"a\":1}\n{\"a\":2}\n{\"a\":3}\n", 3, 3},
        {"logfmt records over five physical lines",
         &logfmt,
         "level=info msg=first\n  continued line\nlevel=info msg=second\n  more\nlevel=info msg=third\n",
         5,
         3},
        {"no final newline", &json, "{\"a\":1}\n{\"a\":2}", 2, 2},
        {"trailing blank lines", &json, "{\"a\":1}\n\n\n", 3, 1},
        {"a bad line", &json, "{\"a\":1}\nnot json\n{\"a\":3}\n", 3, 2},
        {"empty input", &json, "", 0, 0},
    };

    for (const Case &c : cases)
    {
        SECTION(c.name)
        {
            RecordingSink fileSink;
            ParseAsFile(*c.parser, "batch_contract_lines.log", c.bytes, 0, fileSink);
            CHECK(fileSink.finished == 1);
            CHECK(fileSink.LastLineNumber() == c.lines);
            CHECK(fileSink.RowCount() == c.rows);

            for (const std::size_t chunk : {std::size_t{1}, std::size_t{7}, std::string::npos})
            {
                RecordingSink streamSink;
                ParseAsStream(*c.parser, c.bytes, chunk, streamSink);
                INFO("chunk size " << chunk);
                CHECK(streamSink.finished == 1);
                CHECK(streamSink.LastLineNumber() == c.lines);
                CHECK(streamSink.RowCount() == c.rows);
            }
        }
    }
}

TEST_CASE("A stream counts an oversized line that it drops", "[batch_contract][line_count]")
{
    std::string bytes = "{\"a\":1}\n" + std::string(200, 'x') + "\n{\"a\":3}\n";
    ParserOptions options;
    options.maxLineBytes = 64;
    RecordingSink sink;
    ParseAsStream(JsonParser{}, bytes, 16, sink, options);

    CHECK(sink.LastLineNumber() == 3);
    CHECK(sink.RowCount() == 2);
    const auto errors = sink.Errors();
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].firstLine == 2);
    CHECK(errors[0].lastLine == 2);
}

TEST_CASE("The line number of every batch never goes backwards", "[batch_contract][line_count]")
{
    std::string bytes;
    for (int i = 0; i < 5000; ++i)
    {
        bytes += "{\"a\":" + std::to_string(i) + "}\n";
    }
    RecordingSink fileSink;
    ParseAsFile(JsonParser{}, "batch_contract_monotonic.jsonl", bytes, 4096, fileSink);
    RecordingSink streamSink;
    ParseAsStream(JsonParser{}, bytes, 1000, streamSink);

    for (const RecordingSink *sink : {&fileSink, &streamSink})
    {
        std::size_t previous = 0;
        for (const RowBatch &batch : sink->batches)
        {
            CHECK(batch.lastLineNumber >= previous);
            previous = batch.lastLineNumber;
        }
        CHECK(sink->LastLineNumber() == 5000);
    }
}
