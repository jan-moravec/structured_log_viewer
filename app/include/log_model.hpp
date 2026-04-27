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
     * @brief Initialises the model for a streaming parse against @p file.
     *
     * Resets row/column counts, installs @p file as the table's source via
     * `LogTable::BeginStreaming`, obtains a fresh `std::stop_token` from
     * the bridging sink (returned for installation on
     * `ParserOptions::stopToken`), and reserves line-offset capacity
     * proportional to the file size so per-batch appends stay amortised
     * O(1).
     *
     * @param file Already-opened `LogFile` whose mmap backs the parse.
     *             Ownership transfers to the model.
     * @return     Cooperative-cancellation token for the upcoming parse.
     */
    std::stop_token BeginStreaming(std::unique_ptr<loglib::LogFile> file);

    /**
     * @brief Appends one streamed batch and drives Qt model-change signals.
     *
     * Captures old row/column counts and `errors.size()` *before* moving
     * the batch; calls `LogTable::AppendBatch`; emits
     * `beginInsertColumns`/`endInsertColumns` and
     * `beginInsertRows`/`endInsertRows` for any positive delta (empty-rows
     * batches are tolerated); if `LogTable::LastBackfillRange()` is set,
     * emits `dataChanged` over that column range AFTER `endInsertRows`;
     * updates `mLineCount` / `mErrorCount` and emits `lineCountChanged`
     * (always) plus `errorCountChanged` when `capturedErrorCount > 0`.
     */
    void AppendBatch(loglib::StreamedBatch batch);

    /**
     * @brief Finalises the streaming parse. Emits `streamingFinished`.
     *        Does NOT call `ParseTimestamps` — the streaming pipeline and
     *        `AppendBatch` have already promoted every timestamp.
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
     * @brief Returns the GUI-side bridging sink. Lifetime-tied to the model.
     *
     * Exposed so `MainWindow` can hand the same sink instance to
     * `LogParser::ParseStreaming` after `BeginStreaming` has installed a
     * fresh `stop_token` on it.
     */
    QtStreamingLogSink *Sink();

    /**
     * @brief Per-line errors collected from streamed batches since the last
     *        `Clear`/`BeginStreaming`.
     *
     * `LogTable::AppendBatch` discards `batch.errors` (loglib has no view
     * of how the GUI wants to surface them) so `LogModel` peels them off
     * here first. `MainWindow` consumes this on the
     * `streamingFinished(false)` slot to show the post-parse summary.
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
