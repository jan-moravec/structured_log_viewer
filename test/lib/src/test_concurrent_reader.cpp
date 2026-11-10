#include "common.hpp"

#include <loglib/core/key_index.hpp>
#include <loglib/core/row_batch.hpp>
#include <loglib/core/stream_line_source.hpp>
#include <loglib/parse/log_parse_sink.hpp>
#include <loglib/parse/parser_options.hpp>
#include <loglib/parse/parsers/json_parser.hpp>
#include <loglib/schema/column_schema.hpp>
#include <loglib/table/log_table.hpp>

#include <catch2/catch_all.hpp>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace loglib;

namespace
{

/**
 * A sink with no Qt in it. It runs on the pipeline thread and passes each batch to another thread through a queue,
 * which is how a viewer receives rows.
 */
class HandOffSink final : public LogParseSink
{
public:
    explicit HandOffSink(KeyIndex &keys)
        : mKeys(&keys)
    {
    }

    KeyIndex &Keys() override
    {
        return *mKeys;
    }

    void OnStarted() override
    {
    }

    void OnBatch(RowBatch batch) override
    {
        {
            const std::scoped_lock lock(mMutex);
            mQueue.push_back(std::move(batch));
        }
        mReady.notify_one();
    }

    void OnFinished(bool /*cancelled*/) override
    {
        {
            const std::scoped_lock lock(mMutex);
            mFinished = true;
        }
        mReady.notify_one();
    }

    /** Waits for the next batch. Returns nothing once the parse has finished and the queue is empty. */
    std::optional<RowBatch> Next()
    {
        std::unique_lock lock(mMutex);
        mReady.wait(lock, [&] { return !mQueue.empty() || mFinished; });
        if (mQueue.empty())
        {
            return std::nullopt;
        }
        RowBatch batch = std::move(mQueue.front());
        mQueue.pop_front();
        return batch;
    }

private:
    KeyIndex *mKeys;
    std::mutex mMutex;
    std::condition_variable mReady;
    std::deque<RowBatch> mQueue;
    bool mFinished = false;
};

} // namespace

TEST_CASE(
    "A reader thread appends batches and reads cells while an enumeration column is demoted",
    "[threading][concurrent_reader]"
)
{
    constexpr int STABLE_ROWS = 600;
    constexpr int TOTAL_ROWS = 3000;
    std::string bytes;
    for (int i = 0; i < TOTAL_ROWS; ++i)
    {
        // The first rows hold four service names, so the column becomes an enumeration. After that every row has a new
        // name, which overflows the dictionary and demotes the column.
        const std::string service =
            i < STABLE_ROWS ? "svc-" + std::to_string(i % 4) : "svc-unique-" + std::to_string(i);
        bytes += R"({"id":)" + std::to_string(i) + R"(,"svc":")" + service + R"(","msg":"message )" +
                 std::to_string(i) + "\"}\n";
    }

    auto source = std::make_unique<StreamLineSource>("memory.log", std::make_unique<InMemoryBytesProducer>(bytes, 512));
    StreamLineSource *sourcePtr = source.get();

    LogTable table;
    table.SetEnumValueCap(8);
    table.BeginStreaming(std::move(source));
    HandOffSink sink(table.Keys());

    std::exception_ptr parseFailure;
    std::thread pipeline([&] {
        try
        {
            JsonParser{}.ParseStreaming(*sourcePtr, sink, ParserOptions{});
        }
        catch (...)
        {
            parseFailure = std::current_exception();
        }
    });

    // The table belongs to this thread. It appends each batch and then reads every cell that exists so far.
    std::size_t cellsRead = 0;
    std::size_t badCells = 0;
    bool sawEnumeration = false;
    bool sawDemotion = false;
    while (std::optional<RowBatch> batch = sink.Next())
    {
        const LogTable::AppendBatchResult result = table.AppendBatch(std::move(*batch));
        for (const EnumColumnEvent &event : result.enumEvents)
        {
            sawDemotion = sawDemotion || event.change == EnumColumnChange::Demoted;
        }
        const std::size_t columns = table.ColumnCount();
        for (std::size_t column = 0; column < columns; ++column)
        {
            sawEnumeration = sawEnumeration ||
                             (table.GetHeader(column) == "svc" &&
                              table.Configuration().Configuration().columns.at(column).type == ColumnType::Enumeration);
        }
        for (std::size_t row = 0; row < table.RowCount(); ++row)
        {
            for (std::size_t column = 0; column < columns; ++column)
            {
                const std::string text = table.GetFormattedValue(row, column);
                static_cast<void>(table.GetEnumValueId(row, column));
                ++cellsRead;
                if (table.GetHeader(column) == "id" && text != std::to_string(row))
                {
                    ++badCells;
                }
            }
        }
    }
    pipeline.join();
    if (parseFailure)
    {
        std::rethrow_exception(parseFailure);
    }
    static_cast<void>(table.FinalizeAutoDetection());

    REQUIRE(table.RowCount() == TOTAL_ROWS);
    CHECK(badCells == 0);
    CHECK(cellsRead > 0);
    // The reader saw the column as an enumeration and then saw it demoted, so the reads did cover both states.
    CHECK(sawEnumeration);
    CHECK(sawDemotion);

    // After the demotion the column holds its text again, whatever it was when a row was read.
    std::size_t svcColumn = table.ColumnCount();
    for (std::size_t column = 0; column < table.ColumnCount(); ++column)
    {
        if (table.GetHeader(column) == "svc")
        {
            svcColumn = column;
        }
    }
    REQUIRE(svcColumn < table.ColumnCount());
    CHECK(table.Configuration().Configuration().columns.at(svcColumn).type != ColumnType::Enumeration);
    CHECK(table.GetFormattedValue(0, svcColumn) == "svc-0");
    CHECK(table.GetFormattedValue(STABLE_ROWS - 1, svcColumn) == "svc-3");
    CHECK(table.GetFormattedValue(TOTAL_ROWS - 1, svcColumn) == "svc-unique-" + std::to_string(TOTAL_ROWS - 1));
}
