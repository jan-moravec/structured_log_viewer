#pragma once

#include <log_data.hpp>
#include <log_table.hpp>

#include <QAbstractTableModel>
#include <QModelIndex>
#include <QVariant>
#include <QString>
#include <QDateTime>
#include <QBrush>
#include <QRegularExpression>

#include <memory>

using namespace loglib;

enum LogModelItemDataRole
{
    UserRole = Qt::UserRole,
    SortRole,
    CopyLine
};

class LogModel : public QAbstractTableModel
{
public:
    explicit LogModel(QObject *parent = nullptr);

    void AddData(LogData &&logData, const LogConfiguration &configuration)
    {
        beginResetModel();

        mLogData.Merge(std::move(logData));

        mLogTable = std::make_unique<LogTable>(mLogData, configuration);

        endResetModel();
    }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override
    {
        Q_UNUSED(parent);
        if (mLogTable != nullptr)
        {
            return static_cast<int>(mLogTable->RowCount());
        }
        return 0;
        //return static_cast<int>(mLogData.GetLines().size());
    }

    int columnCount(const QModelIndex &parent = QModelIndex()) const override
    {
        Q_UNUSED(parent);
        if (mLogTable != nullptr)
        {
            return static_cast<int>(mLogTable->ColumnCount());
        }
        return 0;
        //return static_cast<int>(mLogData.GetKeys().size());
    }

    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override
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
            //return QString::fromStdString(mLogData.GetKeys()[static_cast<size_t>(section)]);
        }

        return QVariant();
    }

    static QString ConvertToSingleLineCompactQString(const std::string &string)
    {
        QString qString = QString::fromStdString(string);
        qString.replace("\n", " ");
        qString.replace("\r", " ");
        return qString.simplified();
    }

    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override
    {
        if (!index.isValid() || index.row() >= rowCount() || index.column() >= columnCount() || mLogTable == nullptr)
        {
            return QVariant();
        }

        if (role == Qt::DisplayRole)
        {
            //LogValue value = mLogData.GetLines()[static_cast<size_t>(index.row())]->GetValue(mLogData.GetKeys()[static_cast<size_t>(index.column())]);


            return ConvertToSingleLineCompactQString(mLogTable->GetFormattedValue(static_cast<size_t>(index.row()), static_cast<size_t>(index.column())));
        }
        else if (role == LogModelItemDataRole::SortRole)
        {
            LogValue value = mLogTable->GetValue(static_cast<size_t>(index.row()), static_cast<size_t>(index.column()));
            return std::visit(
                [](auto &&arg)
                {
                    using T = std::decay_t<decltype(arg)>;
                    if constexpr (std::is_same_v<T, std::string>)
                    {
                        return QVariant(ConvertToSingleLineCompactQString(arg));
                    }
                    else if constexpr (std::is_same_v<T, int64_t>)
                    {
                        return QVariant(arg);
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
                        return QVariant(static_cast<int64_t>(arg.time_since_epoch().count()));
                    }
                    else if constexpr (std::is_same_v<T, std::monostate>)
                    {
                        return QVariant();
                    }
                    else
                    {
                        static_assert(false, "non-exhaustive visitor!");
                    }
                },
                value);
        }
        else if (role == LogModelItemDataRole::CopyLine)
        {
            return QVariant(QString::fromStdString(mLogData.GetLines()[static_cast<size_t>(index.row())]->GetLine()));
        }
        else if (role == Qt::BackgroundRole)
        {
            //return QBrush(Qt::yellow);
        }

        return QVariant();
    }

private:
    LogData mLogData;
    std::unique_ptr<LogTable> mLogTable;
};
