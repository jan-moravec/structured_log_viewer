#include "loglib/table/table_loader.hpp"

#include "loglib/schema/log_configuration.hpp"

#include <algorithm>
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
    TableSink sink(table);
    parser.ParseStreaming(sourceRef, sink, std::move(options));
    return ParseOutcome{
        .errors = sink.TakeErrors(),
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
    for (ParseError &error : batch.errors)
    {
        mErrors.push_back(std::move(error));
    }
    batch.errors.clear();
    mTable->AppendBatchArranged(std::move(batch));
}

void TableSink::OnFinished(bool cancelled)
{
    mCancelled = cancelled;
}

std::vector<ParseError> TableSink::TakeErrors()
{
    return std::exchange(mErrors, {});
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
    table.BubbleLevelColumns(table.FinalizeAutoDetection().levelBubbleKeys);
}

} // namespace loglib
