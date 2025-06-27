#include "common.hpp"

#include <loglib/json_parser.hpp>
#include <loglib/log_factory.hpp>
#include <loglib/log_table.hpp>

#include <catch2/catch_all.hpp>
#include <date/date.h>
#include <glaze/glaze.hpp>

#include <chrono>
#include <random>
#include <string>
#include <vector>

using namespace loglib;

// Helper function to generate random JSON log entries
std::vector<TestJsonLogFile::Line> GenerateRandomJsonLogs(size_t count)
{
    std::vector<TestJsonLogFile::Line> logs;
    logs.reserve(count);

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> level_dist(0, 3);
    std::uniform_int_distribution<> component_dist(0, 4);
    std::uniform_int_distribution<> words_count_dist(5, 20); // Random number of words per message

    static const std::array<std::string, 6> LEVELS = {"trace", "debug", "info", "warning", "error", "fatal"};
    static const std::array<std::string, 5> COMPONENTS = {"app", "network", "database", "ui", "system"};
    static const std::array<std::string, 20> WORDS = {"lorem",       "ipsum",      "dolor",      "sit",    "amet",
                                                      "consectetur", "adipiscing", "elit",       "sed",    "do",
                                                      "eiusmod",     "tempor",     "incididunt", "ut",     "labore",
                                                      "et",          "dolore",     "magna",      "aliqua", "ut"};

    for (size_t i = 0; i < count; ++i)
    {
        // Generate a random message from words
        std::string message;
        int word_count = words_count_dist(gen);
        for (int j = 0; j < word_count; ++j)
        {
            std::uniform_int_distribution<> word_dist(0, static_cast<int>(WORDS.size() - 1));
            if (!message.empty())
            {
                message += " ";
            }
            message += WORDS[word_dist(gen)];
        }

        glz::json_t json;
        json["timestamp"] =
            date::format("%FT%T", date::floor<std::chrono::milliseconds>(std::chrono::system_clock::now()));
        json["level"] = LEVELS[level_dist(gen)];
        json["message"] = message;
        json["thread_id"] = i % 16;
        json["component"] = COMPONENTS[component_dist(gen)];
        logs.emplace_back(json);
    }

    return logs;
}

TEST_CASE("Parse and load JSON log", "[benchmark][json_parser]")
{
    BENCHMARK_ADVANCED("Parse 10'000 JSON log entries")(Catch::Benchmark::Chronometer meter)
    {
        // 1.7 MB per 10'000 lines of data
        auto logs = GenerateRandomJsonLogs(10'000);
        const TestJsonLogFile testFile(logs);
        const JsonParser parser;

        meter.measure([&]() {
            LogTable table;
            ParseResult result = parser.Parse(testFile.GetFilePath());
            REQUIRE(result.data.Lines().size() == testFile.Lines().size());
            REQUIRE(result.errors.empty());
            table.Update(std::move(result.data));
            return table;
        });
    };
}
