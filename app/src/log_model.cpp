#include "log_model.hpp"

#include "qt_streaming_log_sink.hpp"

#include <QModelIndex>
#include <QString>
#include <QVariant>

#include <utility>

LogModel::LogModel(QObject *parent) : QAbstractTableModel{parent}
{
    // The bridging sink lives on the GUI thread alongside the model and is
    // parented to it so its lifetime tracks the model's automatically.
    mSink = new QtStreamingLogSink(this, this);
}

LogModel::~LogModel() = default;

void LogModel::AddData(loglib::LogData &&logData)
{
    beginResetModel();

    mLogTable.Update(std::move(logData));

    endResetModel();
}

void LogModel::Clear()
{
    // Cancel any in-flight streaming parse first (PRD req. 4.3.30). Non-blocking;
    // the parser thread is allowed to keep running for a brief moment as it
    // walks out of parallel_pipeline. The combination of generation bump (in
    // RequestStop) plus the upcoming model reset means any straggler OnBatch
    // calls that reach the GUI event loop are dropped before they can touch the
    // freshly reset model.
    if (mSink)
    {
        mSink->RequestStop();
    }

    beginResetModel();

    mLogTable = loglib::LogTable();
    mErrorCount = 0;
    mStreamingErrors.clear();

    endResetModel();

    emit lineCountChanged(0);
    emit errorCountChanged(0);
}

std::stop_token LogModel::BeginStreaming(std::unique_ptr<loglib::LogFile> file)
{
    // Reset model state via beginResetModel (rather than per-row remove signals)
    // because BeginStreaming is the natural point to swap the entire table out.
    // After endResetModel, the streaming AppendBatch path takes over and grows
    // the model row-by-row via beginInsertRows/endInsertRows (PRD req. 4.3.27).
    beginResetModel();

    if (file)
    {
        // Reserve the line-offset table proportionally to the file size so each
        // batch's vector::insert is amortised O(1). The "file_size / 100" factor
        // is the same heuristic the PRD recommends (req. 4.1.6a) and matches the
        // ~100-byte average JSON-line size we observed in the benchmark fixture.
        const size_t reserveCount = file->Size() / 100;
        mLogTable.BeginStreaming(std::move(file));
        if (reserveCount > 0 && !mLogTable.Data().Files().empty())
        {
            mLogTable.Data().Files().front()->ReserveLineOffsets(reserveCount);
        }
    }
    else
    {
        mLogTable.BeginStreaming(nullptr);
    }
    mErrorCount = 0;
    mStreamingErrors.clear();

    endResetModel();

    emit lineCountChanged(0);
    emit errorCountChanged(0);

    return mSink->BeginParse();
}

void LogModel::AppendBatch(loglib::StreamedBatch batch)
{
    // Capture the errors BEFORE moving the batch — LogTable::AppendBatch
    // discards batch.errors (loglib has no notion of how the GUI surfaces
    // them) so this is the model's only opportunity to peel them off.
    const qsizetype capturedErrorCount = static_cast<qsizetype>(batch.errors.size());
    if (!batch.errors.empty())
    {
        mStreamingErrors.reserve(mStreamingErrors.size() + batch.errors.size());
        for (auto &error : batch.errors)
        {
            mStreamingErrors.emplace_back(std::move(error));
        }
        batch.errors.clear();
    }

    const int oldRowCount = static_cast<int>(mLogTable.RowCount());
    const int oldColumnCount = static_cast<int>(mLogTable.ColumnCount());

    mLogTable.AppendBatch(std::move(batch));

    const int newRowCount = static_cast<int>(mLogTable.RowCount());
    const int newColumnCount = static_cast<int>(mLogTable.ColumnCount());

    // Step 4 — column delta. Always before rows so any new column header is in
    // place by the time inserted rows query data() for it.
    if (newColumnCount > oldColumnCount)
    {
        beginInsertColumns(QModelIndex(), oldColumnCount, newColumnCount - 1);
        endInsertColumns();
    }

    // Step 5 — row delta. Skip when there were no new rows; an empty-rows-only
    // batch (errors only, or new keys only) is a normal occurrence and
    // beginInsertRows with last < first would be a Qt assertion failure.
    if (newRowCount > oldRowCount)
    {
        beginInsertRows(QModelIndex(), oldRowCount, newRowCount - 1);
        endInsertRows();
    }

    // Step 6 — back-fill notify. After beginInsertRows/endInsertRows the views
    // already know about the new rows, so dataChanged covers both the previously
    // visible rows and the just-inserted ones in a single emit.
    if (const auto &range = mLogTable.LastBackfillRange(); range.has_value() && newRowCount > 0)
    {
        const int firstColumn = static_cast<int>(range->first);
        const int lastColumn = static_cast<int>(range->second);
        emit dataChanged(
            index(0, firstColumn),
            index(newRowCount - 1, lastColumn),
            {Qt::DisplayRole, static_cast<int>(LogModelItemDataRole::SortRole)}
        );
    }

    // Step 7 — counters. lineCountChanged fires unconditionally (req. 4.3.27)
    // so the status-bar progress label can keep ticking even on batches that
    // added zero rows but, say, increased the error count.
    mErrorCount += capturedErrorCount;
    emit lineCountChanged(static_cast<qsizetype>(newRowCount));
    if (capturedErrorCount > 0)
    {
        emit errorCountChanged(mErrorCount);
    }
}

void LogModel::EndStreaming(bool cancelled)
{
    emit streamingFinished(cancelled);
}

int LogModel::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return static_cast<int>(mLogTable.RowCount());
}

int LogModel::columnCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return static_cast<int>(mLogTable.ColumnCount());
}

QVariant LogModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (role != Qt::DisplayRole)
    {
        return QVariant();
    }

    if (orientation == Qt::Horizontal && section >= 0 && section < columnCount())
    {
        return QString::fromStdString(mLogTable.GetHeader(static_cast<size_t>(section)));
    }

    return QVariant();
}

QVariant LogModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= rowCount() || index.column() >= columnCount())
    {
        return QVariant();
    }

    if (role == Qt::DisplayRole)
    {
        return ConvertToSingleLineCompactQString(
            mLogTable.GetFormattedValue(static_cast<size_t>(index.row()), static_cast<size_t>(index.column()))
        );
    }
    else if (role == LogModelItemDataRole::SortRole)
    {
        loglib::LogValue value =
            mLogTable.GetValue(static_cast<size_t>(index.row()), static_cast<size_t>(index.column()));
        return std::visit(
            [](auto &&arg) -> QVariant {
                using T = std::decay_t<decltype(arg)>;
                if constexpr (std::is_same_v<T, std::string>)
                {
                    return QVariant(ConvertToSingleLineCompactQString(arg));
                }
                else if constexpr (std::is_same_v<T, std::string_view>)
                {
                    // string_view alternative carries values that point directly into the
                    // mmap (PRD req. 4.1.6). Promote to std::string so the QString factory
                    // never observes the borrowed view past the LogValue's lifetime.
                    return QVariant(ConvertToSingleLineCompactQString(std::string(arg)));
                }
                else if constexpr (std::is_same_v<T, int64_t>)
                {
                    return QVariant::fromValue<qlonglong>(arg);
                }
                else if constexpr (std::is_same_v<T, uint64_t>)
                {
                    return QVariant::fromValue<qulonglong>(arg);
                }
                else if constexpr (std::is_same_v<T, double>)
                {
                    return QVariant(arg);
                }
                else if constexpr (std::is_same_v<T, bool>)
                {
                    return QVariant(arg);
                }
                else if constexpr (std::is_same_v<T, loglib::TimeStamp>)
                {
                    return QVariant::fromValue<qint64>(arg.time_since_epoch().count());
                }
                else if constexpr (std::is_same_v<T, std::monostate>)
                {
                    return QVariant();
                }
                else
                {
                    static_assert(std::is_same_v<T, void>, "non-exhaustive visitor!");
                }
            },
            value
        );
    }
    else if (role == LogModelItemDataRole::CopyLine)
    {
        return QVariant(
            QString::fromStdString(mLogTable.Data().Lines()[static_cast<size_t>(index.row())].FileReference().GetLine())
        );
    }

    return QVariant();
}

template <typename T> std::optional<std::pair<T, T>> LogModel::GetMinMaxValues(int column) const
{
    if (column < 0 || column >= columnCount() || rowCount() <= 0)
    {
        return std::nullopt;
    }

    const QVariant initialValue = data(index(0, column), LogModelItemDataRole::SortRole);
    if (!initialValue.isValid())
    {
        return std::nullopt;
    }

    auto minVal = initialValue.value<T>();
    auto maxVal = initialValue.value<T>();

    for (int row = 1; row < rowCount(); ++row)
    {
        const QVariant valueVariant = data(index(row, column), LogModelItemDataRole::SortRole);
        if (valueVariant.isValid())
        {
            const auto value = valueVariant.value<T>();
            if (value < minVal)
            {
                minVal = value;
            }
            if (value > maxVal)
            {
                maxVal = value;
            }
        }
    }

    return std::make_pair(minVal, maxVal);
}

template std::optional<std::pair<qint64, qint64>> LogModel::GetMinMaxValues<qint64>(int column) const;

const loglib::LogTable &LogModel::Table() const
{
    return mLogTable;
}

loglib::LogTable &LogModel::Table()
{
    return mLogTable;
}

const loglib::LogData &LogModel::Data() const
{
    return mLogTable.Data();
}

const loglib::LogConfiguration &LogModel::Configuration() const
{
    return mLogTable.Configuration().Configuration();
}

loglib::LogConfigurationManager &LogModel::ConfigurationManager()
{
    return mLogTable.Configuration();
}

QtStreamingLogSink *LogModel::Sink()
{
    return mSink;
}

const std::vector<std::string> &LogModel::StreamingErrors() const
{
    return mStreamingErrors;
}

QString LogModel::ConvertToSingleLineCompactQString(const std::string &string)
{
    QString qString = QString::fromStdString(string);
    qString.replace("\n", " ");
    qString.replace("\r", " ");
    return qString.simplified();
}
