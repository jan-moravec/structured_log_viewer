#pragma once

#include "filter_rule.hpp"
#include "log_model.hpp"

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

    void SetFilterRules(std::vector<std::unique_ptr<FilterRule>> &&filterRules)
    {
        mFilterRules = std::move(filterRules);
        invalidateFilter(); // Call this to reapply the filter with the new regex list.
    }

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override
    {
        for (const auto &rule : mFilterRules)
        {
            QModelIndex index = sourceModel()->index(sourceRow, rule->FilteredColumn(), sourceParent);
            if (index.isValid())
            {
                if (!rule->Matches(sourceModel()->data(index, LogModelItemDataRole::SortRole)))
                {
                    return false;
                }
            }
        }

        return true; // All filter rules have a match, accept the row.
    }

private:
    std::vector<std::unique_ptr<FilterRule>> mFilterRules;

private:
    bool Matches(const QVariant &data, const QVariant &value, Qt::MatchFlags flags) const;
};
