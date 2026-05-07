#pragma once

#include <loglib/enum_dictionary.hpp>
#include <loglib/log_configuration.hpp>

#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariant>

#include <vector>

class FilterRule
{
public:
    FilterRule(int filteredColumn)
        : mFilteredColumn(filteredColumn)
    {
    }
    virtual ~FilterRule() = default;
    virtual int FilteredColumn() const
    {
        return mFilteredColumn;
    }

    /// True iff the row passes. @p displayOrSort is the formatted /
    /// typed value (`SortRole`); @p enumValueId is the raw
    /// `EnumValueId` for `DictRef` slots (used only by
    /// `EnumFilterRule`).
    virtual bool Matches(const QVariant &displayOrSort, const QVariant &enumValueId) const = 0;

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

    bool Matches(const QVariant &data, const QVariant & /*enumValueId*/) const override
    {
        switch (mMatch)
        {
        case loglib::LogConfiguration::LogFilter::Match::exactly:
            return data == mValue;
        case loglib::LogConfiguration::LogFilter::Match::contains:
            return data.toString().contains(mValue);
        case loglib::LogConfiguration::LogFilter::Match::regularExpression:
        {
            QRegularExpression regex(mValue);
            return regex.match(data.toString()).hasMatch();
        }
        case loglib::LogConfiguration::LogFilter::Match::wildcard:
        {
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

    bool Matches(const QVariant &data, const QVariant & /*enumValueId*/) const override
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

/// Multi-select equality filter for `Type::enumeration` columns.
///
/// Fast path: pre-resolves selected strings to `EnumValueId`s at
/// construction; per-row matching is a single bit test on a
/// `vector<bool>` keyed by id.
///
/// Fallback: rows without a `DictRef` slot (numeric, monostate, or
/// not-yet-encoded `OwnedString`) match via `QSet<QString>` against
/// the display string. Empty selection matches no rows.
class EnumFilterRule : public FilterRule
{
public:
    EnumFilterRule(int filteredColumn, const QStringList &selectedValues, const loglib::EnumDictionary *dictionary)
        : FilterRule(filteredColumn), mSelected(selectedValues.cbegin(), selectedValues.cend())
    {
        if (dictionary == nullptr)
        {
            return;
        }
        // Index directly by id. Values added past `Size()` later fall
        // back to the string path until the rule is rebuilt.
        mSelectedIds.assign(static_cast<size_t>(dictionary->Size()), false);
        for (const QString &value : selectedValues)
        {
            const QByteArray utf8 = value.toUtf8();
            const std::string_view bytes(utf8.constData(), static_cast<size_t>(utf8.size()));
            const loglib::EnumValueId id = dictionary->Find(bytes);
            if (id == loglib::INVALID_ENUM_VALUE_ID)
            {
                continue;
            }
            const auto idx = static_cast<size_t>(id);
            if (idx >= mSelectedIds.size())
            {
                continue;
            }
            mSelectedIds[idx] = true;
            mFastPathArmed = true;
        }
    }

    bool Matches(const QVariant &displayOrSort, const QVariant &enumValueId) const override
    {
        if (mSelected.isEmpty())
        {
            // Empty selection hides every row.
            return false;
        }

        if (mFastPathArmed && enumValueId.isValid())
        {
            bool ok = false;
            const qint32 raw = enumValueId.toInt(&ok);
            if (ok && raw >= 0)
            {
                const auto idx = static_cast<size_t>(raw);
                if (idx < mSelectedIds.size())
                {
                    return mSelectedIds[idx];
                }
            }
        }

        return mSelected.contains(displayOrSort.toString());
    }

private:
    QSet<QString> mSelected;
    /// Bitset indexed by `EnumValueId`; empty when no dictionary was
    /// supplied or no selected string matched one.
    std::vector<bool> mSelectedIds;
    /// True when at least one selected string resolved to an id.
    /// Otherwise the bitset would always say "no match" and shadow
    /// the `mSelected` fallback.
    bool mFastPathArmed = false;
};
