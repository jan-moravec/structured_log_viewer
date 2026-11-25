// The parser converts a key to timestamps when its name marks a time and no configured column uses it. These tests
// check which keys that covers, that the result is the same in every batch, and that a key which shows up late is
// handled like one that was there from the start.

#include "common.hpp"
#include "time_conversion_paths.hpp"

#include <loglib/config/column.hpp>
#include <loglib/core/stream_line_source.hpp>
#include <loglib/parse/parser_options.hpp>
#include <loglib/parse/parsers/json_parser.hpp>
#include <loglib/table/log_table.hpp>
#include <loglib/table/table_loader.hpp>

#include <catch2/catch_all.hpp>

#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace loglib;
using test_support::LoadFileViaPath;
using test_support::PathLoad;
using test_support::TimePath;

namespace
{

/** The column of a table that has this header, or the column count when there is none. */
[[nodiscard]] size_t ColumnOf(const LogTable &table, const std::string &header)
{
    for (size_t column = 0; column < table.ColumnCount(); ++column)
    {
        if (table.GetHeader(column) == header)
        {
            return column;
        }
    }
    return table.ColumnCount();
}

} // namespace

TEST_CASE("Time key names are matched without regard to case", "[automatic_time_keys]")
{
    for (const char *name : {"timestamp", "TimeStamp", "time", "TIME", "ts", "@timestamp", "datetime", "created_at"})
    {
        CHECK(IsTimeKeyName(name));
    }
    for (const char *name : {"t", "times", "timestamps", "", "date", "message"})
    {
        CHECK_FALSE(IsTimeKeyName(name));
    }
}

TEST_CASE("A key that a configured column of another type uses is not converted", "[automatic_time_keys]")
{
    std::string bytes;
    for (int i = 0; i < 50; ++i)
    {
        bytes += std::format(R"({{"ts":"2026-01-01T10:00:{:02}Z","n":{}}})", i, i) + "\n";
    }
    const TestLogFile file("automatic_time_keys_string.jsonl");
    file.Write(bytes);

    LogTable table;
    table.Layout().SetColumns(
        {Column{.header = "ts", .keys = {"ts"}, .type = ColumnType::String, .autoDetect = false}}
    );
    const ParseOutcome outcome =
        ParseIntoTable(table, JsonParser{}, file.CreateFileLineSource(), ParserOptionsFor(table));
    FinishLoading(table);

    REQUIRE(outcome.errors.empty());
    REQUIRE(table.RowCount() == 50);
    const size_t column = ColumnOf(table, "ts");
    REQUIRE(column < table.ColumnCount());
    CHECK(table.Layout().Columns()[column].type == ColumnType::String);
    for (size_t row = 0; row < table.RowCount(); ++row)
    {
        CHECK_FALSE(std::holds_alternative<TimeStamp>(table.GetValue(row, column)));
    }
}

TEST_CASE("A time key that appears late in a large file is converted by the workers", "[automatic_time_keys]")
{
    constexpr int ROWS = 20'000;
    constexpr int FIRST_WITH_KEY = 12'000;
    std::string bytes;
    for (int i = 0; i < ROWS; ++i)
    {
        if (i < FIRST_WITH_KEY)
        {
            bytes += std::format(R"({{"n":{}}})", i);
        }
        else
        {
            bytes += std::format(R"({{"n":{},"timestamp":"2026-01-01T10:{:02}:{:02}Z"}})", i, (i / 60) % 60, i % 60);
        }
        bytes += '\n';
    }
    const TestLogFile file("automatic_time_keys_late.jsonl");
    file.Write(bytes);

    for (const unsigned threads : {1U, 4U})
    {
        INFO("threads " << threads);
        const JsonParser parser;
        std::vector<std::vector<KeyId>> claimed;
        {
            // Every batch of the parse is recorded to see what it claims.
            struct RecordingSink final : LogParseSink
            {
                explicit RecordingSink(LogTable &table)
                    : inner(table)
                {
                }
                KeyIndex &Keys() override
                {
                    return inner.Keys();
                }
                void OnStarted() override
                {
                }
                void OnBatch(RowBatch batch) override
                {
                    claims.push_back(batch.convertedTimeKeys);
                    inner.OnBatch(std::move(batch));
                }
                void OnFinished(bool cancelled) override
                {
                    inner.OnFinished(cancelled);
                }
                TableSink inner;
                std::vector<std::vector<KeyId>> claims;
            };

            LogTable table;
            table.SetDefaultSourceTimeZone(TestTimeZone());
            table.SetDisplayTimeZone(TestTimeZone());
            auto source = file.CreateFileLineSource();
            FileLineSource &sourceRef = *source;
            table.BeginStreaming(std::move(source));
            RecordingSink sink(table);
            ParserOptions options = ParserOptionsFor(table);
            options.threads = threads;
            options.batchSizeBytes = 16 * 1024;
            parser.Parse(sourceRef, sink, options);
            FinishLoading(table);

            REQUIRE(table.RowCount() == ROWS);
            REQUIRE(sink.claims.size() > 3);
            const size_t column = ColumnOf(table, "timestamp");
            REQUIRE(column < table.ColumnCount());
            CHECK(table.Layout().Columns()[column].type == ColumnType::Time);
            for (size_t row = 0; row < table.RowCount(); ++row)
            {
                const LogValue value = table.GetValue(row, column);
                if (row < FIRST_WITH_KEY)
                {
                    CHECK(std::holds_alternative<std::monostate>(value));
                }
                else
                {
                    CHECK(std::holds_alternative<TimeStamp>(value));
                }
            }
            claimed = sink.claims;
            // The first batches hold no such key, so they list nothing. Later batches list it, and once listed it
            // stays.
            CHECK(claimed.front().empty());
            REQUIRE_FALSE(claimed.back().empty());
            bool listed = false;
            for (const auto &keys : claimed)
            {
                if (!keys.empty())
                {
                    listed = true;
                }
                else
                {
                    CHECK_FALSE(listed);
                }
            }
        }
    }
}

TEST_CASE("A key named time that holds integers stays numbers in a Time column", "[automatic_time_keys]")
{
    std::string bytes;
    for (int i = 0; i < 3000; ++i)
    {
        bytes += std::format(R"({{"time":{},"n":{}}})", 1'700'000'000 + i, i) + "\n";
    }
    const TestLogFile file("automatic_time_keys_integers.jsonl");
    file.Write(bytes);

    const JsonParser parser;
    const PathLoad load = LoadFileViaPath(TimePath::TableSink, parser, file.CreateFileLineSource(), 4, 16 * 1024, {});
    const LogTable &table = *load.table;
    REQUIRE(table.RowCount() == 3000);
    const size_t column = ColumnOf(table, "time");
    REQUIRE(column < table.ColumnCount());
    CHECK(table.Layout().Columns()[column].type == ColumnType::Time);
    for (size_t row = 0; row < table.RowCount(); ++row)
    {
        const LogValue value = table.GetValue(row, column);
        REQUIRE((std::holds_alternative<std::int64_t>(value) || std::holds_alternative<std::uint64_t>(value)));
    }
}

TEST_CASE("A stream converts a time key that appears after the first batch", "[automatic_time_keys][stream]")
{
    std::string bytes;
    for (int i = 0; i < 600; ++i)
    {
        if (i < 400)
        {
            bytes += std::format(R"({{"n":{}}})", i);
        }
        else
        {
            bytes += std::format(R"({{"n":{},"ts":"2026-01-01T10:00:{:02}Z"}})", i, i % 60);
        }
        bytes += '\n';
    }

    LogTable table;
    table.SetDefaultSourceTimeZone(TestTimeZone());
    table.SetDisplayTimeZone(TestTimeZone());
    auto source =
        std::make_unique<StreamLineSource>("memory.log", std::make_unique<InMemoryBytesProducer>(bytes, 4096));
    const ParseOutcome outcome = ParseIntoTable(table, JsonParser{}, std::move(source), ParserOptionsFor(table));
    FinishLoading(table);

    REQUIRE(outcome.errors.empty());
    REQUIRE(table.RowCount() == 600);
    const size_t column = ColumnOf(table, "ts");
    REQUIRE(column < table.ColumnCount());
    CHECK(table.Layout().Columns()[column].type == ColumnType::Time);
    for (size_t row = 400; row < table.RowCount(); ++row)
    {
        CHECK(std::holds_alternative<TimeStamp>(table.GetValue(row, column)));
    }
}
