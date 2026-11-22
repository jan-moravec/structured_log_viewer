// Benchmarks of loading a file whose time column is detected by its name, with no configuration. They measure who
// converts the time text and what it costs: the table sink that reports detected columns back to the parser, a sink
// that does not (the desktop application), and a table that has its time columns before the parse starts. The same file
// goes through each path, so the differences are the cost of the conversion. Further files change the format, the text
// of the time, the values of the time key and the number of time keys. Every result is checked outside the timed part.

#include "benchmark_common.hpp"
#include "benchmark_registry.hpp"
#include "common.hpp"
#include "time_conversion_paths.hpp"

#include <loglib/core/file_line_source.hpp>
#include <loglib/parse/parsers/json_parser.hpp>
#include <loglib/parse/parsers/logfmt_parser.hpp>

#include <test_common/log_format.hpp>
#include <test_common/log_generator.hpp>

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

using namespace loglib;
using test_support::LoadFileViaPath;
using test_support::PathLoad;
using test_support::TimePath;

namespace bench
{

namespace
{

/** What the time keys of the generated file hold. */
enum class Shape
{
    JsonOffset,
    JsonNaive,
    JsonInteger,
    JsonTwoKeys,
    LogfmtOffset,
};

[[nodiscard]] std::string ShapeName(Shape shape)
{
    switch (shape)
    {
    case Shape::JsonOffset:
        return "json_offset";
    case Shape::JsonNaive:
        return "json_naive";
    case Shape::JsonInteger:
        return "json_integer";
    case Shape::JsonTwoKeys:
        return "json_two_keys";
    case Shape::LogfmtOffset:
        return "logfmt_offset";
    }
    return "unknown";
}

[[nodiscard]] std::vector<std::string> TimeKeysOf(Shape shape)
{
    switch (shape)
    {
    case Shape::JsonOffset:
    case Shape::JsonNaive:
        return {"timestamp"};
    case Shape::JsonInteger:
        return {"time"};
    case Shape::JsonTwoKeys:
        return {"timestamp", "created_at"};
    case Shape::LogfmtOffset:
        return {"ts"};
    }
    return {};
}

/** A generated log file that removes itself. */
class GeneratedFile
{
public:
    GeneratedFile(Shape shape, std::size_t rows)
        : mRows(rows), mPath("bench_time_" + ShapeName(shape) + "_" + std::to_string(rows) + ".log")
    {
        const test_common::LogFormat format =
            shape == Shape::LogfmtOffset ? test_common::Logfmt() : test_common::JsonLines();
        {
            std::ofstream file(mPath, std::ios::binary);
            std::mt19937 rng(LARGE_FIXTURE_SEED);
            for (std::size_t i = 0; i < rows; ++i)
            {
                test_common::LogRecord record =
                    test_common::GenerateRandomLogRecord(rng, i, DeterministicBenchmarkTimestamps());
                // The generator writes the time as text without an offset, under the key timestamp.
                const std::string text = record["timestamp"].get_string();
                record.get_object().erase("timestamp");
                switch (shape)
                {
                case Shape::JsonOffset:
                    record["timestamp"] = text + "+02:00";
                    break;
                case Shape::JsonNaive:
                    record["timestamp"] = text;
                    break;
                case Shape::JsonInteger:
                    record["time"] = static_cast<std::int64_t>(i);
                    break;
                case Shape::JsonTwoKeys:
                    record["timestamp"] = text + "+02:00";
                    record["created_at"] = text + "-05:30";
                    break;
                case Shape::LogfmtOffset:
                    record["ts"] = text + "+02:00";
                    break;
                }
                file << format.writeLine(record) << '\n';
            }
            if (!file.good())
            {
                throw std::runtime_error("Failed to write " + mPath);
            }
        }
        mBytes = static_cast<std::size_t>(std::filesystem::file_size(mPath));
    }

    GeneratedFile(const GeneratedFile &) = delete;
    GeneratedFile &operator=(const GeneratedFile &) = delete;

    ~GeneratedFile() noexcept
    {
        std::error_code ec;
        std::filesystem::remove(mPath, ec);
    }

    [[nodiscard]] const std::string &Path() const
    {
        return mPath;
    }

    [[nodiscard]] std::size_t Rows() const
    {
        return mRows;
    }

    [[nodiscard]] std::size_t Bytes() const
    {
        return mBytes;
    }

private:
    std::size_t mRows;
    std::string mPath;
    std::size_t mBytes = 0;
};

/**
 * The file of the benchmark that runs. The benchmarks that read one file are registered one after another, so keeping
 * the last file built means each file is generated once.
 */
[[nodiscard]] std::shared_ptr<GeneratedFile> FileFor(Shape shape, std::size_t rows)
{
    static std::shared_ptr<GeneratedFile> current;
    static Shape currentShape = Shape::JsonOffset;
    static std::size_t currentRows = 0;
    if (!current || currentShape != shape || currentRows != rows)
    {
        current.reset();
        current = std::make_shared<GeneratedFile>(shape, rows);
        currentShape = shape;
        currentRows = rows;
    }
    return current;
}

struct TimeInput
{
    std::shared_ptr<GeneratedFile> file;
    PathLoad last;
};

/**
 * Checks the table of the last load: the row count, the time columns, and what each row holds in them. A key whose
 * values are integers must keep them as numbers.
 */
[[nodiscard]] std::string CheckLoad(
    const PathLoad &load, std::size_t rows, const std::vector<std::string> &keys, bool numbers
)
{
    if (!load.table || load.table->RowCount() != rows)
    {
        return "loaded the wrong number of rows";
    }
    if (!load.errors.empty())
    {
        return "the load reported errors";
    }
    const LogTable &table = *load.table;
    for (const std::string &key : keys)
    {
        std::size_t found = table.ColumnCount();
        for (std::size_t column = 0; column < table.ColumnCount(); ++column)
        {
            if (table.GetHeader(column) == key)
            {
                found = column;
                break;
            }
        }
        if (found == table.ColumnCount())
        {
            return "no column " + key;
        }
        if (!numbers && table.Layout().Columns()[found].type != ColumnType::Time)
        {
            return "column " + key + " is not a Time column";
        }
        for (std::size_t row = 0; row < rows; ++row)
        {
            const LogValue value = table.GetValue(row, found);
            const bool isTime = std::holds_alternative<TimeStamp>(value);
            const bool isNumber = std::holds_alternative<std::int64_t>(value) ||
                                  std::holds_alternative<std::uint64_t>(value) || std::holds_alternative<double>(value);
            if (numbers ? !isNumber : !isTime)
            {
                return "row " + std::to_string(row) + " of " + key + " holds the wrong kind of value";
            }
        }
    }
    return {};
}

struct Variant
{
    std::string name;
    Shape shape;
    TimePath path;
    unsigned threads = 0;
    std::size_t rows = FULL_TIER.rows;
    bool smoke = false;

    /** One thread is slow enough to time once. The others take about 150 ms, so a few iterations average out noise. */
    [[nodiscard]] int Iterations() const
    {
        return smoke || threads == 1 ? 1 : MEDIUM_LARGE_ITERATIONS;
    }
};

void RegisterVariant(const Variant &variant)
{
    const bool logfmt = variant.shape == Shape::LogfmtOffset;
    RegisterLarge<TimeInput>(
        variant.name,
        [variant] {
            auto input = std::make_unique<TimeInput>();
            input->file = FileFor(variant.shape, variant.rows);
            InitializeTimezoneData();
            return input;
        },
        [variant, logfmt](TimeInput &input, benchmark::State &state, bool first) {
            const std::vector<std::string> keys = TimeKeysOf(variant.shape);
            const JsonParser json;
            const LogfmtParser logfmtParser;
            const LogParser &parser = logfmt ? static_cast<const LogParser &>(logfmtParser) : json;
            const auto load = [&] {
                return LoadFileViaPath(
                    variant.path,
                    parser,
                    std::make_unique<FileLineSource>(input.file->Path()),
                    variant.threads,
                    /*batchBytes=*/0,
                    keys
                );
            };
            if (first)
            {
                static_cast<void>(load());
            }
            for (const auto _ : state)
            {
                input.last = load();
            }
            const std::string problem =
                CheckLoad(input.last, input.file->Rows(), keys, variant.shape == Shape::JsonInteger);
            input.last = {};
            if (!problem.empty())
            {
                Fail(state, problem);
                return;
            }
            const auto iterations = static_cast<std::int64_t>(state.iterations());
            state.SetBytesProcessed(iterations * static_cast<std::int64_t>(input.file->Bytes()));
            state.SetItemsProcessed(iterations * static_cast<std::int64_t>(input.file->Rows()));
        },
        LargeOptions{
            .repetitions = variant.smoke ? SMOKE_TIER.largeRepetitions : LARGE_REPETITIONS,
            .iterations = variant.Iterations(),
        }
    );
}

[[nodiscard]] std::string Title(const std::string &format, const std::string &what, const std::string &path)
{
    return "Time conversion: " + format + " " + CountText(FULL_TIER.rows) + ", " + what + path;
}

} // namespace

void RegisterTimeConversionBenchmarks()
{
    constexpr const char *TABLE_SINK = "table sink, no configuration";
    constexpr const char *NO_FEEDBACK = "sink without feedback, no configuration";
    constexpr const char *UP_FRONT = "columns given up front";

    // Smoke: the table sink path at the smoke size.
    RegisterVariant(
        Variant{
            .name = TierName("Time conversion: JSON " + CountText(SMOKE_TIER.rows) + ", " + TABLE_SINK, SMOKE_TIER),
            .shape = Shape::JsonOffset,
            .path = TimePath::TableSink,
            .rows = SMOKE_TIER.rows,
            .smoke = true,
        }
    );

    // The three paths over the same JSON file, then the same at one and eight threads.
    RegisterVariant({Title("JSON", "", TABLE_SINK), Shape::JsonOffset, TimePath::TableSink});
    RegisterVariant({Title("JSON", "", NO_FEEDBACK), Shape::JsonOffset, TimePath::NoFeedback});
    RegisterVariant({Title("JSON", "", UP_FRONT), Shape::JsonOffset, TimePath::UpFront});
    for (const unsigned threads : {1U, 8U})
    {
        const std::string suffix = ", " + std::to_string(threads) + (threads == 1 ? " thread" : " threads");
        RegisterVariant({Title("JSON", "", TABLE_SINK) + suffix, Shape::JsonOffset, TimePath::TableSink, threads});
        RegisterVariant({Title("JSON", "", NO_FEEDBACK) + suffix, Shape::JsonOffset, TimePath::NoFeedback, threads});
        RegisterVariant({Title("JSON", "", UP_FRONT) + suffix, Shape::JsonOffset, TimePath::UpFront, threads});
    }

    RegisterVariant({Title("logfmt", "", TABLE_SINK), Shape::LogfmtOffset, TimePath::TableSink});
    RegisterVariant({Title("JSON", "naive time text, ", TABLE_SINK), Shape::JsonNaive, TimePath::TableSink});
    RegisterVariant({Title("JSON", "integer time values, ", TABLE_SINK), Shape::JsonInteger, TimePath::TableSink});
    RegisterVariant({Title("JSON", "two time keys, ", TABLE_SINK), Shape::JsonTwoKeys, TimePath::TableSink});
}

} // namespace bench
