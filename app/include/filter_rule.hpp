#pragma once

#include <loglib/log_configuration.hpp>

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
    TextFilterRule(int filteredColumn, const QString &value, loglib::LogConfiguration::LogFilter::Match match)
        : FilterRule(filteredColumn), mValue(value), mMatch(match)
    {
    }

    bool Matches(const QVariant &data) const override
    {
        switch (mMatch)
        {
        case loglib::LogConfiguration::LogFilter::Match::Exactly:
            return data == mValue;
        case loglib::LogConfiguration::LogFilter::Match::Contains:
            return data.toString().contains(mValue);
        case loglib::LogConfiguration::LogFilter::Match::RegularExpression: {
            QRegularExpression regex(mValue);
            return regex.match(data.toString()).hasMatch();
        }
        case loglib::LogConfiguration::LogFilter::Match::Wildcard: {
            QRegularExpression regex(QRegularExpression::wildcardToRegularExpression(mValue));
            return regex.match(data.toString()).hasMatch();
        }
        default:
            return false;
        }
    }

private:
    QString mValue;
    loglib::LogConfiguration::LogFilter::Match mMatch;
};

class TimeStampFilterRule : public FilterRule
{
public:
    TimeStampFilterRule(int filteredColumn, qint64 begin, qint64 end)
        : FilterRule(filteredColumn), mBegin(begin), mEnd(end)
    {
    }

    bool Matches(const QVariant &data) const override
    {
        bool ok = false;
        qint64 timeStamp = data.toLongLong(&ok);
        if (ok && timeStamp >= mBegin && timeStamp <= mEnd)
        {
            return true;
        }
        return false;
    }

private:
    qint64 mBegin;
    qint64 mEnd;
};
