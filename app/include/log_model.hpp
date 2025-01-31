#pragma once

#include <loglib/log_data.hpp>
#include <loglib/log_table.hpp>

#include <QAbstractTableModel>

#include <memory>
#include <optional>

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

    void AddData(loglib::LogData &&logData, const LogConfiguration &configuration);
    void Clear();

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    template <typename T> std::optional<std::pair<T, T>> GetMinMaxValues(int column) const;

    const loglib::LogData &LogData() const;
    const LogConfiguration &Configuration() const;

private:
    loglib::LogData mLogData;
    std::unique_ptr<LogTable> mLogTable;

    static QString ConvertToSingleLineCompactQString(const std::string &string);
};
