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
                        return QVariant(arg.time_since_epoch().count());
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

    QList<QModelIndex> MatchRow(
        const QModelIndex &start, int role,
        const QVariant &value, int hits = 1,
        Qt::MatchFlags flags = Qt::MatchFlags(Qt::MatchStartsWith | Qt::MatchWrap),
        bool forwad = true,
        int skipFirstN = 0) const
    {
        QList<QModelIndex> result;
        const int rowCount = this->rowCount(start.parent());
        const int columnCount = this->columnCount(start.parent());

        bool wrap = flags.testFlag(Qt::MatchWrap);
        const int startRow = start.row();
        const int startColumn = start.column();

        if (forwad)
        {
            for (int row = skipFirstN; row < rowCount; ++row) {
                int actualRow = (startRow + row) % rowCount;

                for (int col = 0; col < columnCount; ++col) {
                    int actualColumn = (startColumn + col) % columnCount;
                    QModelIndex index = this->index(actualRow, actualColumn, start.parent());
                    QVariant data = this->data(index, role);

                    if (matches(data, value, flags)) {
                        result.append(index);
                        if (result.size() == hits) {
                            return result;
                        }
                        break;
                    }
                }

                if (!wrap && actualRow == rowCount - 1) {
                    break;
                }
            }
        }
        else
        {
            for (int row = skipFirstN; row < rowCount; ++row) {
                int actualRow = startRow - row;
                if (actualRow < 0)
                {
                    actualRow += rowCount;
                }

                for (int col = 0; col < columnCount; ++col) {
                    int actualColumn = (startColumn + col) % columnCount;
                    QModelIndex index = this->index(actualRow, actualColumn, start.parent());
                    QVariant data = this->data(index, role);

                    if (matches(data, value, flags)) {
                        result.append(index);
                        if (result.size() == hits) {
                            return result;
                        }
                        break;
                    }
                }

                if (!wrap && actualRow == 0) {
                    break;
                }
            }
        }

        return result;
    }

private:
    bool matches(const QVariant &data, const QVariant &value, Qt::MatchFlags flags) const {
        if (flags.testFlag(Qt::MatchExactly)) {
            return data == value;
        }
        if (flags.testFlag(Qt::MatchStartsWith)) {
            return data.toString().startsWith(value.toString());
        }
        if (flags.testFlag(Qt::MatchEndsWith)) {
            return data.toString().endsWith(value.toString());
        }
        if (flags.testFlag(Qt::MatchContains)) {
            return data.toString().contains(value.toString());
        }
        if (flags.testFlag(Qt::MatchRegularExpression)) {
            QRegularExpression regex(value.toString());
            return regex.match(data.toString()).hasMatch();
        }
        if (flags.testFlag(Qt::MatchWildcard)) {
            QRegularExpression regex(QRegularExpression::wildcardToRegularExpression(value.toString()));
            return regex.match(data.toString()).hasMatch();
        }
        return false;
    }

    // JsonLogs mLogs;
    LogData mLogData;
    std::unique_ptr<LogTable> mLogTable;
};
