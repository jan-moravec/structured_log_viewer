#pragma once

#include <QRegularExpression>
#include <QVariant>

class FilterRule
{
public:
    FilterRule(int filteredColumn) : mFilteredColumn(filteredColumn)
    {
    }
    virtual ~FilterRule() = default;
    virtual int FilteredColumn() const
    {
        return mFilteredColumn;
    }
    virtual bool Matches(const QVariant &mValue) const = 0;

private:
    int mFilteredColumn = 0;
};

class TextFilterRule : public FilterRule
{
public:
    TextFilterRule(int filteredColumn, const QString &value, Qt::MatchFlags flags)
        : FilterRule(filteredColumn), mValue(value), mFlags(flags)
    {
    }

    bool Matches(const QVariant &data) const override
    {
        if (mFlags.testFlag(Qt::MatchExactly))
        {
            return data == mValue;
        }
        if (mFlags.testFlag(Qt::MatchStartsWith))
        {
            return data.toString().startsWith(mValue);
        }
        if (mFlags.testFlag(Qt::MatchEndsWith))
        {
            return data.toString().endsWith(mValue);
        }
        if (mFlags.testFlag(Qt::MatchContains))
        {
            return data.toString().contains(mValue);
        }
        if (mFlags.testFlag(Qt::MatchRegularExpression))
        {
            QRegularExpression regex(mValue);
            return regex.match(data.toString()).hasMatch();
        }
        if (mFlags.testFlag(Qt::MatchWildcard))
        {
            QRegularExpression regex(QRegularExpression::wildcardToRegularExpression(mValue));
            return regex.match(data.toString()).hasMatch();
        }
        return false;
    }

private:
    QString mValue;
    Qt::MatchFlags mFlags;
};
