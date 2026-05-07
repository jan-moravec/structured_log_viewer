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

    /// Decide whether the row's column passes the rule. @p displayOrSort
    /// is the formatted value (`Qt::DisplayRole` for text rules /
    /// `LogModelItemDataRole::SortRole` for typed rules). @p enumValueId
    /// is the raw `EnumValueId` from `LogModelItemDataRole::EnumValueRole`
    /// (a `QVariant<qint32>`), invalid for non-`DictRef` slots. Only
    /// `EnumFilterRule` consumes @p enumValueId; the other rules
    /// ignore it. Returning false hides the row.
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
/// Fast path: when the column is enum-encoded and the row's slot is a
/// `DictRef`, `LogModelItemDataRole::EnumValueRole` carries the raw
/// `EnumValueId`. The constructor pre-resolves every selected `QString`
/// to an id via the column's `EnumDictionary` and stores the result as
/// a `vector<bool>` keyed by id; per-row matching is then a single bit
/// test, regardless of how many strings the user selected.
///
/// Fallback path: rows whose slot is *not* a `DictRef` (numeric value,
/// `monostate`, or a not-yet-encoded `OwnedString` because the column
/// was promoted between batches) fall through to a `QSet<QString>`
/// lookup against `Qt::DisplayRole`/`SortRole`. This keeps partial
/// encoding correct -- any row the encoder could not promote is still
/// matched by string comparison.
///
/// An empty selection matches no rows (the rule is "show only the
/// chosen values"); an empty multi-select with no choices is treated
/// as "hide everything".
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
        // Pre-size for the dict's current capacity so the bitset can
        // be indexed by id directly. Future inserts past `Size()` will
        // fall back to the string path until the rule is rebuilt; the
        // editor reconstructs rules whenever the user re-opens the
        // picker so the lag is bounded.
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
            // Empty selection => zero rows match: the multi-select is
            // unchecked. Distinct from the text rules' empty-substring
            // contract (which matches all rows); the multi-select has
            // no "no constraint" sentinel so empty = zero matches.
            return false;
        }

        // Fast path: bitset lookup keyed by `EnumValueId`. Triggered
        // when the model returned a valid `EnumValueRole` *and* the
        // rule actually resolved at least one selected string against
        // the dictionary at construction (`mFastPathArmed`).
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

        // Fallback: a row whose slot is not enum-encoded (numeric,
        // monostate, post-demote `OwnedString`) goes through the
        // string-set path so partial encoding stays correct.
        return mSelected.contains(displayOrSort.toString());
    }

private:
    QSet<QString> mSelected;
    /// Bitset of selected `EnumValueId`s; `mSelectedIds[id] == true`
    /// iff the user picked the value with that id. Empty when no
    /// dictionary was passed (rule built before promotion) or when
    /// the user's selection contains only strings that are not in the
    /// dictionary.
    std::vector<bool> mSelectedIds;
    /// Set when at least one selected string resolved to a dictionary
    /// id. Avoids the bitset path when the rule has only fallback
    /// strings (bitset would always say "no match" and shadow the
    /// `mSelected` lookup).
    bool mFastPathArmed = false;
};
