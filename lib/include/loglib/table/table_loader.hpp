/**
 * @brief Loads parsed sources into a LogTable the way the GUI and slp do.
 *
 * A parser sends its output to a sink. TableSink is the sink that appends each batch to a LogTable, keeps the parse
 * errors and counts the lines read. ParseIntoTable adds a source to the table and runs a parser over it to the end.
 * ParserOptionsFor gives the parser the table's time columns and zone, so the parser converts timestamps on its own
 * threads. FinishLoading ends type detection after the last source. Together they are the one way to fill a table from
 * a parser without a GUI.
 *
 * Threads: ParseIntoTable returns when the parse is done, and nothing else may touch the table until then. For a
 * FileLineSource the sink is called from the last serial stage of the parse pipeline. TBB runs that stage on any
 * thread of its pool, one call at a time and in file order, while the calling thread waits. So the table is appended
 * to on a pool thread, not on the caller's. For a StreamLineSource the streaming loop and the sink run on the calling
 * thread. Do not keep thread-affine state in a sink.
 *
 * To open a path, including compressed files and session bundles, see output/file_loading.hpp.
 *
 * This is part of the table module.
 */
#pragma once

#include "loglib/core/file_line_source.hpp"
#include "loglib/core/row_batch.hpp"
#include "loglib/core/stop_token.hpp"
#include "loglib/core/stream_line_source.hpp"
#include "loglib/parse/log_parse_sink.hpp"
#include "loglib/parse/log_parser.hpp"
#include "loglib/parse/parser_options.hpp"
#include "loglib/table/log_table.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace loglib
{

/**
 * @brief Appends the batches of one parse to a table, and keeps its errors and line coverage.
 *
 * Use one sink for each parse. The errors of a batch are taken out of it before it is appended, so the table never
 * sees them. The table must outlive the sink and must not be used by anything else while the parse runs.
 */
class TableSink final : public LogParseSink
{
public:
    explicit TableSink(LogTable &table) noexcept;

    /** @brief The key index of the table, so parser threads intern keys where the rows will read them. */
    KeyIndex &Keys() override;

    void OnStarted() override;

    /** @brief Appends the rows, applies the column moves the append asks for and keeps the batch errors. */
    void OnBatch(RowBatch batch) override;

    void OnFinished(bool cancelled) override;

    /** @brief Returns the errors collected so far and clears them. */
    [[nodiscard]] std::vector<ParseError> TakeErrors();

    /**
     * @brief The number of physical lines this parse read, which is the highest last line number of its batches.
     *
     * It counts lines of the input, not rows. Lines of continuation, blank lines and lines that failed to parse are
     * included.
     */
    [[nodiscard]] std::uint64_t LinesConsumed() const noexcept;

    /** @brief True when the parse ended because its stop token fired. Valid after OnFinished. */
    [[nodiscard]] bool Cancelled() const noexcept;

private:
    LogTable *mTable;
    std::vector<ParseError> mErrors;
    std::uint64_t mLinesConsumed = 0;
    bool mCancelled = false;
};

/** @brief What one ParseIntoTable call found. */
struct ParseOutcome
{
    std::vector<ParseError> errors;

    /** @brief The physical lines the parse read. See TableSink::LinesConsumed. */
    std::uint64_t linesConsumed = 0;

    /** @brief True when the stop token ended the parse early. The table holds the rows that arrived. */
    bool cancelled = false;
};

/**
 * @brief Parser options that match what the table expects: its time columns, its default source zone and the given
 * stop token.
 *
 * With them the parser converts the table's time columns while it parses. Without them the rows are still correct,
 * but the table converts the columns itself, which is slower. Set threads, the batch size or other options on the
 * result before parsing.
 */
[[nodiscard]] ParserOptions ParserOptionsFor(const LogTable &table, StopToken stop = {});

/**
 * @brief Adds a file source to the table and parses it to the end before returning.
 *
 * The table decides whether the source is the first: a table without a source begins a new stream, and a table that has
 * sources appends. The table owns the source from here on. Errors in the data are returned in the outcome. Resource
 * failures and OperationCancelled propagate. A parse that throws has already finished its sink, and the table holds the
 * rows that arrived.
 */
ParseOutcome ParseIntoTable(
    LogTable &table, const LogParser &parser, std::unique_ptr<FileLineSource> source, ParserOptions options
);

/** @brief Like the file overload, for a stream. The parse ends with the stream or when the stop token fires. */
ParseOutcome ParseIntoTable(
    LogTable &table, const LogParser &parser, std::unique_ptr<StreamLineSource> source, ParserOptions options
);

/**
 * @brief Finishes type detection after the last source and moves the level columns into place.
 *
 * Call it once, after the last ParseIntoTable. Columns that were still undecided get their final type.
 */
void FinishLoading(LogTable &table);

} // namespace loglib
