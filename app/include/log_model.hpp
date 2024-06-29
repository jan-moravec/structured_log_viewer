#pragma once

#include <log_message.hpp>
#include <QAbstractTableModel>
#include <QModelIndex>
#include <QVariant>
#include <QString>
#include <QDateTime>

#include <set>

using LogSet = std::multiset<LogMessage, LogMessageComparator>;

class LogModel : public QAbstractTableModel
{
public:
    explicit LogModel(QObject *parent = nullptr);

    void setLogData(const LogSet &logData, const QStringList &headerLabels) {
        beginResetModel();
        this->logData = logData;
        this->headerLabels = headerLabels;
        endResetModel();
    }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override {
        Q_UNUSED(parent);
        return static_cast<int>(logData.size());
    }

    int columnCount(const QModelIndex &parent = QModelIndex()) const override {
        Q_UNUSED(parent);
        return static_cast<int>(headerLabels.size());
    }

    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override {
        if (role != Qt::DisplayRole)
            return QVariant();

        if (orientation == Qt::Horizontal && section >= 0 && section < headerLabels.size()) {
            return headerLabels[section];
        }

        return QVariant();
    }

    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override {
        if (!index.isValid() || index.row() >= static_cast<int>(logData.size()) || index.column() >= columnCount()) {
            return QVariant();
        }

        if (role == Qt::DisplayRole) {
            auto it = logData.begin();
            std::advance(it, index.row());
            const LogMessage &log = *it;

            switch (index.column()) {
            case 0: return log.timestamp;
            case 1: return QString::fromStdString(log.severity);
            case 2: return QString::fromStdString(log.message);
            default: return QVariant();
            }
        }

        return QVariant();
    }

private:
    LogSet logData;
    QStringList headerLabels;
    LogMessage mLogMessage;
};
