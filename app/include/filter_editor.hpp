#pragma once

#include "log_model.hpp"

#include <loglib/log_configuration.hpp>

#include <QComboBox>
#include <QDate>
#include <QDateTime>
#include <QDateTimeEdit>
#include <QDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QPushButton>
#include <QSortFilterProxyModel>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QStringList>
#include <QTime>
#include <QTimeEdit>
#include <QVBoxLayout>

class FilterEditor : public QDialog
{
    Q_OBJECT

public:
    FilterEditor(const LogModel &model, QString filterID, QWidget *parent = nullptr);

    void Load(int row, const QString &filterString, int matchType);
    void Load(int row, qint64 begin, qint64 end);
    /// Pre-select the given enum values in the multi-select picker for
    /// `Type::enumeration` columns. Values not present in the column's
    /// current dictionary are silently skipped (rather than blocking
    /// the editor from opening).
    void Load(int row, const QStringList &selectedValues);

    int GetRowToFilter() const;
    QString GetStringToFilter() const;
    int GetMatchType() const;
    QStringList GetSelectedEnumValues() const;

signals:
    void FilterSubmitted(const QString &filterID, int row, const QString &filterString, int matchType);
    void FilterTimeStampSubmitted(const QString &filterID, int row, qint64 beginTimeStamp, qint64 endTimeStamp);
    void FilterEnumSubmitted(const QString &filterID, int row, const QStringList &selectedValues);

private:
    const LogModel &mModel;
    const QString mFilterID;

    QStackedWidget *mStackedWidget;
    QComboBox *mRowComboBox;
    QLineEdit *mStringLineEdit;
    QComboBox *mMatchTypeComboBox;

    QDateEdit *mBeginDateEdit;
    QTimeEdit *mBeginTimeEdit;
    QDateEdit *mEndDateEdit;
    QTimeEdit *mEndTimeEdit;

    /// Picker widgets for `Type::enumeration` columns. The model holds
    /// one checkable item per dictionary value (alphabetised) and is
    /// shared across `Load` calls; the proxy applies the search-box
    /// filter case-insensitively. Both Select All / Clear All operate
    /// over the *visible* (proxy-filtered) rows so users can scope a
    /// bulk action to the search hits.
    QListView *mEnumValuesView;
    QStandardItemModel *mEnumValuesModel;
    QSortFilterProxyModel *mEnumValuesProxy;
    QLineEdit *mEnumSearchEdit;
    QPushButton *mEnumSelectAllButton;
    QPushButton *mEnumClearAllButton;
    QLabel *mEnumSelectionCount;

    QPushButton *mOkButton;
    QPushButton *mCancelButton;

    void SetupLayout();
    void SetBeginEnd(qint64 begin, qint64 end);
    /// Repopulate the enum picker from the column's current dictionary.
    void PopulateEnumValues(int columnIndex);
    /// Refresh "N of M selected" label after every check/uncheck or
    /// search-box change. Cheap O(model rows) walk; the picker is
    /// capped at `MAX_ENUM_VALUES = 1024`.
    void UpdateEnumSelectionCount();
    /// Drop the sticky red warning border that the empty-submit guard
    /// stamps on `mEnumValuesView` / `mStringLineEdit`. Connected to
    /// `dataChanged`/`textChanged` so the warning clears on the user's
    /// next interaction rather than persisting until close.
    void ClearWarningStyles();
    static QDateTime ConvertToQDateTime(qint64 timestamp);
    static qint64 ConvertToTimeStamp(const QDate &date, const QTime &time);

private slots:
    void OnOkClicked();
    void UpdateSelectedColumn(int index);
};
