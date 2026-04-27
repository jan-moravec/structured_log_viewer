#pragma once

#include <loglib/log_data.hpp>
#include <loglib/log_file.hpp>
#include <loglib/log_table.hpp>
#include <loglib/streaming_log_sink.hpp>

#include <QAbstractTableModel>

#include <memory>
#include <optional>
#include <string>
#include <vector>

class QtStreamingLogSink;

enum LogModelItemDataRole
{
    UserRole = Qt::UserRole,
    SortRole,
    CopyLine
};

class LogModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    explicit LogModel(QObject *parent = nullptr);
    ~LogModel() override;

    void AddData(loglib::LogData &&logData);
    void Clear();

    /**
     * @brief Initialises the model for an upcoming streaming parse against
     *        @p file (PRD req. 4.3.27).
     *
     * Resets row/column counts, installs the file as the table's source via
     * `LogTable::BeginStreaming`, calls `mSink->BeginParse()` to obtain a
     * fresh `std::stop_token` (returned to the caller for installation on
     * `ParserOptions::stopToken`) and reserves capacity in the
     * file-backed line-offset table proportional to the file size so the
     * per-batch appends stay amortised O(1).
     *
     * @param file Already-opened `LogFile` whose mmap will back the parse.
     *             Ownership transfers to the model (and to its `LogTable`).
     * @return     The cooperative-cancellation token to install on the
     *             `ParserOptions` for the upcoming parse.
     */
    std::stop_token BeginStreaming(std::unique_ptr<loglib::LogFile> file);

    /**
     * @brief Appends one streamed batch to the table, driving the
     *        appropriate Qt model-change signals (PRD req. 4.3.27).
     *
     * Implements the seven-step delta drive: capture old row/column counts
     * and `errors.size()` BEFORE moving the batch; call
     * `LogTable::AppendBatch(std::move(batch))`; capture the new counts;
     * issue `beginInsertColumns`/`endInsertColumns` if the column delta is
     * positive; issue `beginInsertRows`/`endInsertRows` if the row delta is
     * positive (skip both if delta == 0 — empty-rows-only batches are
     * tolerated); if `LogTable::LastBackfillRange()` is non-null emit
     * `dataChanged` over the affected column range AFTER `endInsertRows`;
     * update the running `mLineCount` / `mErrorCount` and emit
     * `lineCountChanged` (always) and `errorCountChanged` (only when
     * `capturedErrorCount > 0`).
     */
    void AppendBatch(loglib::StreamedBatch batch);

    /**
     * @brief Finalises the streaming parse. Emits `streamingFinished(bool)`.
     *        Does NOT call `ParseTimestamps` — Stage B and `AppendBatch`
     *        have already done all timestamp work (PRD req. 4.2.21,
     *        4.1.13b).
     */
    void EndStreaming(bool cancelled);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    template <typename T> std::optional<std::pair<T, T>> GetMinMaxValues(int column) const;

    const loglib::LogTable &Table() const;
    loglib::LogTable &Table();
    const loglib::LogData &Data() const;
    const loglib::LogConfiguration &Configuration() const;
    loglib::LogConfigurationManager &ConfigurationManager();

    /**
     * @brief Returns the GUI-side bridging sink. Lifetime-tied to this model.
     *
     * Exposed so `MainWindow` can hand the same sink instance to
     * `JsonParser::ParseStreaming` after `BeginStreaming` has already
     * installed a fresh `stop_token` on it.
     */
    QtStreamingLogSink *Sink();

    /**
     * @brief Returns the per-line errors collected from streamed batches
     *        since the last `Clear`/`BeginStreaming`.
     *
     * `LogTable::AppendBatch` discards `batch.errors` (loglib has no
     * notion of how the GUI wants to surface them) so `LogModel` peels them
     * off into this vector first. `MainWindow` consumes it on the
     * `streamingFinished(false)` slot to show the post-parse summary
     * (PRD req. 4.3.29).
     */
    const std::vector<std::string> &StreamingErrors() const;

signals:
    /// Emitted after every `AppendBatch` whose `batch.errors` was non-empty.
    /// Carries the cumulative error count since the last `Clear`/`BeginStreaming`.
    void errorCountChanged(qsizetype count);

    /// Emitted after every `AppendBatch`, even when no rows were added (so the
    /// status bar can still tick on errors-only batches). Carries the current
    /// row count of the table.
    void lineCountChanged(qsizetype count);

    /// Emitted from `EndStreaming(cancelled)` so `MainWindow` can flip its
    /// status bar, re-enable configuration UI, and show the post-parse error
    /// summary on `cancelled == false`.
    void streamingFinished(bool cancelled);

private:
    loglib::LogTable mLogTable;
    QtStreamingLogSink *mSink = nullptr;

    /// Cumulative error count for the active streaming parse. Reset on
    /// `Clear`/`BeginStreaming`. Equal to `mStreamingErrors.size()` after every
    /// `AppendBatch` — kept as a separate counter so `errorCountChanged`
    /// signal callers can read it cheaply without grabbing the whole vector.
    qsizetype mErrorCount = 0;

    /// Per-line errors accumulated from `StreamedBatch::errors` of the
    /// in-flight parse. `LogTable::AppendBatch` discards them, so we peel
    /// them off here before forwarding the batch.
    std::vector<std::string> mStreamingErrors;

    static QString ConvertToSingleLineCompactQString(const std::string &string);
};
