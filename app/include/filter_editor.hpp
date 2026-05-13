#pragma once

#include "log_model.hpp"

#include <loglib/log_configuration.hpp>

#include <QCheckBox>
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

#include <optional>

class FilterEditor : public QDialog
{
    Q_OBJECT

public:
    FilterEditor(const LogModel &model, QString filterID, QWidget *parent = nullptr);

    void Load(int row, const QString &filterString, int matchType);
    void Load(int row, qint64 begin, qint64 end);
    /// Preselect @p selectedValues; values absent from the current
    /// dictionary are skipped.
    void Load(int row, const QStringList &selectedValues);
    /// Restore a numeric-range filter. `std::nullopt` on either bound
    /// leaves that side unbounded.
    void Load(int row, std::optional<double> minValue, std::optional<double> maxValue);
    /// Restore a boolean filter.
    void Load(int row, bool includeTrue, bool includeFalse);

    int GetRowToFilter() const;
    QString GetStringToFilter() const;
    int GetMatchType() const;
    QStringList GetSelectedEnumValues() const;
    std::optional<double> GetNumericRangeMin() const;
    std::optional<double> GetNumericRangeMax() const;
    bool GetBooleanIncludeTrue() const;
    bool GetBooleanIncludeFalse() const;

    /// Test-only accessors for the enum-picker page widgets. Tests cannot
    /// rely on `findChildren<...>()` to locate them: on the Linux runner
    /// with Qt 6.8 + offscreen QPA, QObject-tree traversal returns empty
    /// for QDialog descendants the same way it does for `QAction`s in the
    /// `MainWindow` `.ui` (see `MainWindow::FindUiAction`).
    [[nodiscard]] QListView *EnumPickerView() const
    {
        return mEnumValuesView;
    }
    [[nodiscard]] QSortFilterProxyModel *EnumPickerProxy() const
    {
        return mEnumValuesProxy;
    }
    [[nodiscard]] QLineEdit *EnumSearchEdit() const
    {
        return mEnumSearchEdit;
    }
    [[nodiscard]] QLabel *EnumEmptyPlaceholder() const
    {
        return mEnumEmptyPlaceholder;
    }
    [[nodiscard]] QPushButton *OkButton() const
    {
        return mOkButton;
    }

signals:
    void FilterSubmitted(const QString &filterID, int row, const QString &filterString, int matchType);
    void FilterTimeStampSubmitted(const QString &filterID, int row, qint64 beginTimeStamp, qint64 endTimeStamp);
    void FilterEnumSubmitted(const QString &filterID, int row, const QStringList &selectedValues);
    /// Numeric range filter. `std::nullopt` bounds are unbounded.
    void FilterNumericRangeSubmitted(
        const QString &filterID, int row, std::optional<double> minValue, std::optional<double> maxValue
    );
    /// Boolean filter; at least one of @p includeTrue / @p includeFalse is set.
    void FilterBooleanSubmitted(const QString &filterID, int row, bool includeTrue, bool includeFalse);

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

    /// Multi-select picker for `Type::Enumeration` columns. Items are
    /// alphabetised; Select/Clear All operate on visible rows only.
    QListView *mEnumValuesView;
    QStandardItemModel *mEnumValuesModel;
    QSortFilterProxyModel *mEnumValuesProxy;
    QLineEdit *mEnumSearchEdit;
    QPushButton *mEnumSelectAllButton;
    QPushButton *mEnumClearAllButton;
    QLabel *mEnumSelectionCount;
    /// Shown when the dictionary is empty; OK stays disabled while visible.
    QLabel *mEnumEmptyPlaceholder;

    /// Numeric-range page: an empty line edit on either side means
    /// "unbounded" on that side; `OnOkClicked` rejects only the
    /// both-empty case (which would match every numeric row).
    QLineEdit *mNumericMinEdit;
    QLineEdit *mNumericMaxEdit;

    /// Boolean page: one checkbox per side (independent toggles --
    /// user may pick either, both, or neither).
    QCheckBox *mBoolIncludeTrue;
    QCheckBox *mBoolIncludeFalse;

    QPushButton *mOkButton;
    QPushButton *mCancelButton;

    void SetupLayout();
    void SetBeginEnd(qint64 begin, qint64 end);
    /// Repopulate the enum picker from the column's current dictionary.
    void PopulateEnumValues(int columnIndex);
    /// Refresh the "N of M selected" label and OK gating.
    void UpdateEnumSelectionCount();
    /// Drop the red warning border from the empty-submit guard.
    void ClearWarningStyles();
    static QDateTime ConvertToQDateTime(qint64 timestamp);
    static qint64 ConvertToTimeStamp(const QDate &date, const QTime &time);

private slots:
    void OnOkClicked();
    void UpdateSelectedColumn(int index);
};
