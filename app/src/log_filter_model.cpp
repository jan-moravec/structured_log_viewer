#include "log_filter_model.hpp"

#include <loglib/enum_dictionary.hpp>
#include <loglib/log_compare.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_filter.hpp>
#include <loglib/log_table.hpp>

#include <QAbstractItemModel>
#include <QAbstractProxyModel>
#include <QModelIndex>
#include <QRegularExpression>
#include <QString>
#include <QVariant>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <utility>

LogFilterModel::LogFilterModel(QObject *parent)
    : QSortFilterProxyModel{parent}
{
}

void LogFilterModel::SetLogModel(LogModel *logModel)
{
    mLogModel = logModel;
    mEnumRanks.clear();
}

void LogFilterModel::setSourceModel(QAbstractItemModel *sourceModel)
{
    QSortFilterProxyModel::setSourceModel(sourceModel);
    RefreshSourceProxyCache();
}

void LogFilterModel::RefreshSourceProxyCache()
{
    mImmediateProxy = qobject_cast<QAbstractProxyModel *>(sourceModel());
}

void LogFilterModel::SetFilterRules(std::vector<std::unique_ptr<loglib::RowPredicate>> &&filterRules)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    beginFilterChange();
    mFilterRules = std::move(filterRules);
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
#else
    mFilterRules = std::move(filterRules);
    invalidateFilter();
#endif
}

void LogFilterModel::InvalidateEnumRanks()
{
    mEnumRanks.clear();
}

bool LogFilterModel::filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const
{
    if (mFilterRules.empty())
    {
        return true;
    }
    if (mLogModel == nullptr)
    {
        // Predicates can't run without the table; treat as no-filter
        // rather than rejecting every row, matching the empty-rules
        // semantics above.
        return true;
    }
    const int logRow = MapToLogModelRow(sourceRow, sourceParent);
    if (logRow < 0)
    {
        return false;
    }
    const auto row = static_cast<size_t>(logRow);
    const loglib::LogTable &table = mLogModel->Table();
    return std::ranges::all_of(mFilterRules, [&table, row](const auto &rule) {
        return rule->MatchesRow(table, row);
    });
}

bool LogFilterModel::lessThan(const QModelIndex &source_left, const QModelIndex &source_right) const
{
    if (mLogModel == nullptr)
    {
        return QSortFilterProxyModel::lessThan(source_left, source_right);
    }
    const int leftRow = MapModelIndexToLogModelRow(source_left);
    const int rightRow = MapModelIndexToLogModelRow(source_right);
    if (leftRow < 0 || rightRow < 0)
    {
        return QSortFilterProxyModel::lessThan(source_left, source_right);
    }
    const int columnIndex = source_left.column();
    const auto &columns = mLogModel->Configuration().columns;
    if (columnIndex < 0 || static_cast<size_t>(columnIndex) >= columns.size())
    {
        return QSortFilterProxyModel::lessThan(source_left, source_right);
    }
    const loglib::EnumDictRank *rank = nullptr;
    if (columns[columnIndex].type == loglib::LogConfiguration::Type::Enumeration)
    {
        rank = EnumRankFor(columnIndex);
    }
    const int cmp = loglib::CompareRows(
        mLogModel->Table(),
        static_cast<size_t>(leftRow),
        static_cast<size_t>(rightRow),
        static_cast<size_t>(columnIndex),
        rank
    );
    return cmp < 0;
}

int LogFilterModel::MapToLogModelRow(int sourceRow, const QModelIndex &sourceParent) const
{
    QAbstractItemModel *src = sourceModel();
    if (src == nullptr)
    {
        return -1;
    }
    return MapModelIndexToLogModelRow(src->index(sourceRow, 0, sourceParent));
}

int LogFilterModel::MapModelIndexToLogModelRow(QModelIndex idx) const
{
    if (!idx.isValid())
    {
        return -1;
    }
    // Fast path: cached immediate proxy is the single hop between this
    // filter and the underlying `LogModel`. Avoids the per-call
    // `qobject_cast` that QSortFilterProxyModel::sort would otherwise
    // pay O(N log N) times.
    if (mImmediateProxy != nullptr && idx.model() == mImmediateProxy)
    {
        QModelIndex mapped = mImmediateProxy->mapToSource(idx);
        if (mapped.model() == mLogModel)
        {
            return mapped.row();
        }
        idx = mapped;
    }
    // Generic fall-through: walk proxy layers down to the underlying
    // `LogModel`. Covers deeper chains and `setSourceModel` calls that
    // happened before the cache caught up.
    while (const auto *proxy = qobject_cast<const QAbstractProxyModel *>(idx.model()))
    {
        QModelIndex mapped = proxy->mapToSource(idx);
        if (!mapped.isValid())
        {
            return -1;
        }
        idx = mapped;
    }
    if (idx.model() != mLogModel)
    {
        return -1;
    }
    return idx.row();
}

const loglib::EnumDictRank *LogFilterModel::EnumRankFor(int columnIndex) const
{
    if (mLogModel == nullptr)
    {
        return nullptr;
    }
    const auto &columns = mLogModel->Configuration().columns;
    if (columnIndex < 0 || static_cast<size_t>(columnIndex) >= columns.size())
    {
        return nullptr;
    }
    const auto &column = columns[columnIndex];
    if (column.keys.empty())
    {
        return nullptr;
    }
    const loglib::KeyId canonicalKeyId = mLogModel->Table().Keys().Find(column.keys.front());
    if (canonicalKeyId == loglib::INVALID_KEY_ID)
    {
        return nullptr;
    }
    const loglib::EnumDictionary *dictionary = mLogModel->Table().EnumDictionaries().Find(canonicalKeyId);
    if (dictionary == nullptr)
    {
        return nullptr;
    }
    auto it = mEnumRanks.find(columnIndex);
    if (it != mEnumRanks.end() && it.value() && it.value()->DictSize() >= dictionary->Size())
    {
        return it.value().get();
    }
    auto rank = std::make_shared<const loglib::EnumDictRank>(*dictionary);
    const loglib::EnumDictRank *raw = rank.get();
    mEnumRanks.insert(columnIndex, std::move(rank));
    return raw;
}

QList<QModelIndex> LogFilterModel::MatchRow(
    const QModelIndex &start, int role, const QVariant &value, int hits, Qt::MatchFlags flags, bool forward, int skipFirstN
) const
{
    QList<QModelIndex> result;
    const int rowCount = this->rowCount(start.parent());
    const int columnCount = this->columnCount(start.parent());

    const bool wrap = flags.testFlag(Qt::MatchWrap);
    const int startRow = start.row();
    const int startColumn = start.column();

    if (forward)
    {
        for (int row = skipFirstN; row < rowCount; ++row)
        {
            const int actualRow = (startRow + row) % rowCount;

            for (int col = 0; col < columnCount; ++col)
            {
                const int actualColumn = (startColumn + col) % columnCount;
                const QModelIndex index = this->index(actualRow, actualColumn, start.parent());
                const QVariant data = this->data(index, role);

                if (Matches(data, value, flags))
                {
                    result.append(index);
                    if (result.size() == hits)
                    {
                        return result;
                    }
                    break;
                }
            }

            if (!wrap && actualRow == rowCount - 1)
            {
                break;
            }
        }
    }
    else
    {
        for (int row = skipFirstN; row < rowCount; ++row)
        {
            int actualRow = startRow - row;
            if (actualRow < 0)
            {
                actualRow += rowCount;
            }

            for (int col = 0; col < columnCount; ++col)
            {
                const int actualColumn = (startColumn + col) % columnCount;
                const QModelIndex index = this->index(actualRow, actualColumn, start.parent());
                const QVariant data = this->data(index, role);

                if (Matches(data, value, flags))
                {
                    result.append(index);
                    if (result.size() == hits)
                    {
                        return result;
                    }
                    break;
                }
            }

            if (!wrap && actualRow == 0)
            {
                break;
            }
        }
    }

    return result;
}

bool LogFilterModel::Matches(const QVariant &data, const QVariant &value, Qt::MatchFlags flags)
{
    if (flags.testFlag(Qt::MatchExactly))
    {
        return data == value;
    }
    if (flags.testFlag(Qt::MatchStartsWith))
    {
        return data.toString().startsWith(value.toString());
    }
    if (flags.testFlag(Qt::MatchEndsWith))
    {
        return data.toString().endsWith(value.toString());
    }
    if (flags.testFlag(Qt::MatchContains))
    {
        return data.toString().contains(value.toString());
    }
    if (flags.testFlag(Qt::MatchRegularExpression))
    {
        const QRegularExpression regex(value.toString());
        return regex.match(data.toString()).hasMatch();
    }
    if (flags.testFlag(Qt::MatchWildcard))
    {
        const QRegularExpression regex(QRegularExpression::wildcardToRegularExpression(value.toString()));
        return regex.match(data.toString()).hasMatch();
    }
    return false;
}
