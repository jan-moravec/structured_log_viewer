/**
 * @brief The three ways a file can reach a table with its time columns converted, for tests and benchmarks.
 *
 * A time column is converted by the parser's worker threads. TimePath names the three loads that exist: the table sink
 * of the library, a sink that only forwards to it, as the desktop application's sink does for its model, and a table
 * that already has its time columns when the parse starts. LoadFileViaPath runs one of them on a file with fixed
 * options, so the equivalence test and the time conversion benchmarks drive exactly the same code.
 *
 * This is test support. It is not part of the library.
 */
#pragma once

#include "common.hpp"

#include <loglib/config/column_layout.hpp>
#include <loglib/core/file_line_source.hpp>
#include <loglib/core/row_batch.hpp>
#include <loglib/parse/log_parse_sink.hpp>
#include <loglib/parse/log_parser.hpp>
#include <loglib/parse/parser_options.hpp>
#include <loglib/table/log_table.hpp>
#include <loglib/table/table_loader.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace test_support
{

/** @brief How the time columns of a file get converted. */
enum class TimePath
{
    /** @brief TableSink: the parser converts the time keys it finds by name. */
    TableSink,
    /** @brief A sink that only forwards to a TableSink, with the table holding no columns before the parse. */
    NoFeedback,
    /** @brief The table has its time columns before the parse, so the parser gets them in the options. */
    UpFront,
};

[[nodiscard]] inline const char *TimePathName(TimePath path)
{
    switch (path)
    {
    case TimePath::TableSink:
        return "table sink";
    case TimePath::NoFeedback:
        return "sink without feedback";
    case TimePath::UpFront:
        return "columns up front";
    }
    return "";
}

/** @brief A sink that forwards every call to a TableSink and nothing else. */
class NoFeedbackSink final : public loglib::LogParseSink
{
public:
    explicit NoFeedbackSink(loglib::LogTable &table)
        : mInner(table)
    {
    }

    loglib::KeyIndex &Keys() override
    {
        return mInner.Keys();
    }

    void OnStarted() override
    {
        mInner.OnStarted();
    }

    void OnBatch(loglib::RowBatch batch) override
    {
        mInner.OnBatch(std::move(batch));
    }

    void OnFinished(bool cancelled) override
    {
        mInner.OnFinished(cancelled);
    }

    [[nodiscard]] loglib::TableSink &Inner()
    {
        return mInner;
    }

private:
    loglib::TableSink mInner;
};

/** @brief The reference time every path uses, so formats without a year read the same year. */
[[nodiscard]] inline std::chrono::system_clock::time_point FixedReferenceTime()
{
    return std::chrono::sys_days{std::chrono::year{2026} / 6 / 15};
}

/** @brief A table and what loading a file into it found. */
struct PathLoad
{
    std::unique_ptr<loglib::LogTable> table;
    std::vector<loglib::ParseError> errors;
    std::uint64_t errorCount = 0;
    std::uint64_t linesConsumed = 0;
};

/**
 * @brief Loads a file into a new table through one path.
 *
 * Threads and batchBytes are passed to the parser as they are, and zero keeps the defaults. For TimePath::UpFront the
 * table starts with the Time columns that the key names in timeKeys make. The table reads naive time text in the zone
 * the tests use.
 */
[[nodiscard]] inline PathLoad LoadFileViaPath(
    TimePath path,
    const loglib::LogParser &parser,
    std::unique_ptr<loglib::FileLineSource> source,
    unsigned threads,
    std::size_t batchBytes,
    const std::vector<std::string> &timeKeys
)
{
    PathLoad load;
    loglib::ColumnLayout layout;
    if (path == TimePath::UpFront)
    {
        layout.AppendKeys(timeKeys);
    }
    load.table = std::make_unique<loglib::LogTable>(std::move(layout));
    loglib::LogTable &table = *load.table;
    table.SetReferenceTime(FixedReferenceTime());
    table.SetDisplayTimeZone(TestTimeZone());
    table.SetDefaultSourceTimeZone(TestTimeZone());

    loglib::ParserOptions options = loglib::ParserOptionsFor(table);
    options.threads = threads;
    options.batchSizeBytes = batchBytes;

    if (path == TimePath::NoFeedback)
    {
        loglib::FileLineSource &sourceRef = *source;
        table.BeginStreaming(std::move(source));
        options.referenceTime = table.ReferenceTime();
        NoFeedbackSink sink(table);
        parser.Parse(sourceRef, sink, std::move(options));
        load.errors = sink.Inner().TakeErrors();
        load.errorCount = sink.Inner().ErrorCount();
        load.linesConsumed = sink.Inner().LinesConsumed();
    }
    else
    {
        loglib::ParseOutcome outcome = loglib::ParseIntoTable(table, parser, std::move(source), std::move(options));
        load.errors = std::move(outcome.errors);
        load.errorCount = outcome.errorCount;
        load.linesConsumed = outcome.linesConsumed;
    }
    loglib::FinishLoading(table);
    return load;
}

} // namespace test_support
