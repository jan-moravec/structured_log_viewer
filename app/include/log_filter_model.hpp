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
        Qt::MatchFlags flags = Qt::MatchStartsWith | Qt::MatchWrap,
        bool forward = true,
        int skipFirstN = 0
    ) const;

    void SetFilterRules(std::vector<std::unique_ptr<FilterRule>> &&filterRules)
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
        // Qt 6.9+: limit invalidation to rows only (our filter never affects columns).
        beginFilterChange();
        mFilterRules = std::move(filterRules);
        endFilterChange(QSortFilterProxyModel::Direction::Rows);
#else
        mFilterRules = std::move(filterRules);
        invalidateFilter();
#endif
    }

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override
    {
        for (const auto &rule : mFilterRules)
        {
            QModelIndex index = sourceModel()->index(sourceRow, rule->FilteredColumn(), sourceParent);
            if (!index.isValid())
            {
                continue;
            }
            // Per-rule data needs: the common case (TextFilterRule
            // only) skips the `EnumValueRole` query entirely, saving
            // a `LogModel::data()` round-trip per (row, rule).
            // `EnumFilterRule` uses the bitset fast path when the
            // slot is `DictRef`-encoded.
            const QVariant displayOrSort = rule->NeedsDisplayOrSort()
                                               ? sourceModel()->data(index, LogModelItemDataRole::SortRole)
                                               : QVariant{};
            const QVariant enumValueId = rule->NeedsEnumValueId()
                                             ? sourceModel()->data(index, LogModelItemDataRole::EnumValueRole)
                                             : QVariant{};
            if (!rule->Matches(displayOrSort, enumValueId))
            {
                return false;
            }
        }

        return true; // All filter rules have a match, accept the row.
    }

private:
    std::vector<std::unique_ptr<FilterRule>> mFilterRules;

private:
    static bool Matches(const QVariant &data, const QVariant &value, Qt::MatchFlags flags);
};
