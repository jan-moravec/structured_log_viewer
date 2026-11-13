#include "loglib/table/table_loader.hpp"

#include "loglib/schema/log_configuration.hpp"

#include <algorithm>
#include <format>
#include <string>
#include <type_traits>
#include <utility>

namespace loglib
{

namespace
{

// Starts a new stream for the first source of a table and appends the later ones.
template <typename Source> void AttachSource(LogTable &table, std::unique_ptr<Source> source)
{
    if (table.Data().Sources().empty())
    {
        table.BeginStreaming(std::move(source));
    }
    else
    {
        table.AppendStreaming(std::move(source));
    }
}

template <typename Source>
ParseOutcome RunParse(LogTable &table, const LogParser &parser, std::unique_ptr<Source> source, ParserOptions options)
{
    Source &sourceRef = *source;
    AttachSource(table, std::move(source));
    if constexpr (std::is_same_v<Source, FileLineSource>)
    {
        // A file parse converts with one reference time. Taking the table's, which attaching the source fixed, makes
        // the rows the parser converts agree with the rows the table converts itself, such as those of a time column
        // it detects while the parse runs.
        if (!options.referenceTime.has_value())
        {
            options.referenceTime = table.ReferenceTime();
        }
    }
    TableSink sink(table);
    parser.ParseStreaming(sourceRef, sink, std::move(options));
    return ParseOutcome{
        .errors = sink.TakeErrors(),
        .errorCount = sink.ErrorCount(),
        .linesConsumed = sink.LinesConsumed(),
        .cancelled = sink.Cancelled(),
    };
}

} // namespace

TableSink::TableSink(LogTable &table) noexcept
    : mTable(&table)
{
}

KeyIndex &TableSink::Keys()
{
    return mTable->Keys();
}

void TableSink::OnStarted()
{
}

void TableSink::OnBatch(RowBatch batch)
{
    mLinesConsumed = std::max(mLinesConsumed, static_cast<std::uint64_t>(batch.lastLineNumber));
    LogTable::AppendBatchResult result = mTable->AppendBatch(std::move(batch));
    mTable->ApplyPlacements(result.placements);
    mErrorCount += result.errors.size();
    for (ParseError &error : result.errors)
    {
        if (mErrors.size() < MAX_STORED_PARSE_ERRORS)
        {
            mErrors.push_back(std::move(error));
        }
        else
        {
            ++mDroppedErrors;
        }
    }
}

void TableSink::OnFinished(bool cancelled)
{
    mCancelled = cancelled;
}

std::vector<Column> TableSink::TimeColumnsAdded()
{
    std::vector<Column> added;
    for (const Column &column : mTable->Configuration().Configuration().columns)
    {
        if (column.type != ColumnType::Time || column.keys.empty() ||
            std::ranges::contains(mReportedTimeColumns, column.keys.front()))
        {
            continue;
        }
        mReportedTimeColumns.push_back(column.keys.front());
        added.push_back(column);
    }
    return added;
}

std::vector<ParseError> TableSink::TakeErrors()
{
    if (mDroppedErrors != 0)
    {
        mErrors.push_back(
            ParseError{
                .message = mDroppedErrors == 1 ? std::string("1 more parse error was not recorded.")
                                               : std::format("{} more parse errors were not recorded.", mDroppedErrors),
            }
        );
        mDroppedErrors = 0;
    }
    return std::exchange(mErrors, {});
}

std::uint64_t TableSink::ErrorCount() const noexcept
{
    return mErrorCount;
}

std::uint64_t TableSink::LinesConsumed() const noexcept
{
    return mLinesConsumed;
}

bool TableSink::Cancelled() const noexcept
{
    return mCancelled;
}

ParserOptions ParserOptionsFor(const LogTable &table, StopToken stop)
{
    ParserOptions options;
    options.stopToken = std::move(stop);
    options.timeColumns = TimeColumnsOf(table.Configuration().Configuration());
    options.defaultSourceTimeZone = table.DefaultSourceTimeZone();
    return options;
}

ParseOutcome ParseIntoTable(
    LogTable &table, const LogParser &parser, std::unique_ptr<FileLineSource> source, ParserOptions options
)
{
    return RunParse(table, parser, std::move(source), std::move(options));
}

ParseOutcome ParseIntoTable(
    LogTable &table, const LogParser &parser, std::unique_ptr<StreamLineSource> source, ParserOptions options
)
{
    return RunParse(table, parser, std::move(source), std::move(options));
}

void FinishLoading(LogTable &table)
{
    table.ApplyPlacements(table.FinalizeAutoDetection().placements);
}

} // namespace loglib
