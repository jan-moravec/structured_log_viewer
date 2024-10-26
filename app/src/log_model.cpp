#include "log_model.hpp"

#include <QBrush>
#include <QDateTime>
#include <QModelIndex>
#include <QRegularExpression>
#include <QString>
#include <QVariant>

LogModel::LogModel(QObject *parent) : QAbstractTableModel{parent}
{
}

void LogModel::AddData(LogData &&logData, const LogConfiguration &configuration)
{
    beginResetModel();

    mLogData.Merge(std::move(logData));

    mLogTable = std::make_unique<LogTable>(mLogData, configuration);

    endResetModel();
}

void LogModel::Clear()
{
    beginResetModel();

    mLogData = LogData();
    mLogTable.reset();

    endResetModel();
}

int LogModel::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    if (mLogTable != nullptr)
    {
        return static_cast<int>(mLogTable->RowCount());
    }
    return 0;
}

int LogModel::columnCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    if (mLogTable != nullptr)
    {
        return static_cast<int>(mLogTable->ColumnCount());
    }
    return 0;
}

QVariant LogModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (role != Qt::DisplayRole)
    {
        return QVariant();
    }

    if (orientation == Qt::Horizontal && section >= 0 && section < columnCount())
    {
        if (mLogTable != nullptr)
        {
            return QString::fromStdString(mLogTable->GetHeader(static_cast<size_t>(section)));
        }
    }

    return QVariant();
}

QVariant LogModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= rowCount() || index.column() >= columnCount() || mLogTable == nullptr)
    {
        return QVariant();
    }

    if (role == Qt::DisplayRole)
    {
        return ConvertToSingleLineCompactQString(
            mLogTable->GetFormattedValue(static_cast<size_t>(index.row()), static_cast<size_t>(index.column()))
        );
    }
    else if (role == LogModelItemDataRole::SortRole)
    {
        LogValue value = mLogTable->GetValue(static_cast<size_t>(index.row()), static_cast<size_t>(index.column()));
        return std::visit(
            [](auto &&arg) -> QVariant {
                using T = std::decay_t<decltype(arg)>;
                if constexpr (std::is_same_v<T, std::string>)
                {
                    return QVariant(ConvertToSingleLineCompactQString(arg));
                }
                else if constexpr (std::is_same_v<T, int64_t>)
                {
                    return QVariant::fromValue<qlonglong>(arg);
                }
                else if constexpr (std::is_same_v<T, double>)
                {
                    return QVariant(arg);
                }
                else if constexpr (std::is_same_v<T, bool>)
                {
                    return QVariant(arg);
                }
                else if constexpr (std::is_same_v<T, TimeStamp>)
                {
                    return QVariant::fromValue<qlonglong>(arg.time_since_epoch().count());
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
        return QVariant(QString::fromStdString(mLogData.GetLines()[static_cast<size_t>(index.row())]->GetLine()));
    }
    else if (role == Qt::BackgroundRole)
    {
        // return QBrush(Qt::yellow);
    }

    return QVariant();
}

const LogConfiguration &LogModel::Configuration() const
{
    return mLogTable->Configuration();
}

QString LogModel::ConvertToSingleLineCompactQString(const std::string &string)
{
    QString qString = QString::fromStdString(string);
    qString.replace("\n", " ");
    qString.replace("\r", " ");
    return qString.simplified();
}
