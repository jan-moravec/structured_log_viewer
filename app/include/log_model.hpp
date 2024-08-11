#pragma once

#include <log_data.hpp>
#include <log_table.hpp>

#include <QAbstractTableModel>

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

    void AddData(LogData &&logData, const LogConfiguration &configuration);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;

private:
    LogData mLogData;
    std::unique_ptr<LogTable> mLogTable;

    static QString ConvertToSingleLineCompactQString(const std::string &string);
};
