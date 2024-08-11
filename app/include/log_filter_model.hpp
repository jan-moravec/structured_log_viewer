#pragma once

#include <QSortFilterProxyModel>

class LogFilterModel : public QSortFilterProxyModel
{
public:
    explicit LogFilterModel(QObject *parent = nullptr);

    QList<QModelIndex> MatchRow(
        const QModelIndex &start,
        int role,
        const QVariant &value,
        int hits = 1,
        Qt::MatchFlags flags = Qt::MatchFlags(Qt::MatchStartsWith | Qt::MatchWrap),
        bool forward = true,
        int skipFirstN = 0
    ) const;

private:
    bool Matches(const QVariant &data, const QVariant &value, Qt::MatchFlags flags) const;
};
