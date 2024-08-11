#include "log_filter_model.hpp"

LogFilterModel::LogFilterModel(QObject *parent) : QSortFilterProxyModel{parent}
{
}

QList<QModelIndex> LogFilterModel::MatchRow(
    const QModelIndex &start,
    int role,
    const QVariant &value,
    int hits,
    Qt::MatchFlags flags,
    bool forward,
    int skipFirstN
) const
{
    QList<QModelIndex> result;
    const int rowCount = this->rowCount(start.parent());
    const int columnCount = this->columnCount(start.parent());

    bool wrap = flags.testFlag(Qt::MatchWrap);
    const int startRow = start.row();
    const int startColumn = start.column();

    if (forward)
    {
        for (int row = skipFirstN; row < rowCount; ++row)
        {
            int actualRow = (startRow + row) % rowCount;

            for (int col = 0; col < columnCount; ++col)
            {
                int actualColumn = (startColumn + col) % columnCount;
                QModelIndex index = this->index(actualRow, actualColumn, start.parent());
                QVariant data = this->data(index, role);

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
                int actualColumn = (startColumn + col) % columnCount;
                QModelIndex index = this->index(actualRow, actualColumn, start.parent());
                QVariant data = this->data(index, role);

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

bool LogFilterModel::Matches(const QVariant &data, const QVariant &value, Qt::MatchFlags flags) const
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
        QRegularExpression regex(value.toString());
        return regex.match(data.toString()).hasMatch();
    }
    if (flags.testFlag(Qt::MatchWildcard))
    {
        QRegularExpression regex(QRegularExpression::wildcardToRegularExpression(value.toString()));
        return regex.match(data.toString()).hasMatch();
    }
    return false;
}
