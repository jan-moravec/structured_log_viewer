#pragma once

#include "log_model.hpp"

#include <log_configuration.hpp>

#include <QComboBox>
#include <QDateTime>
#include <QDateTimeEdit>
#include <QDialog>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QTimeEdit>
#include <QVBoxLayout>

#include <optional>

class FilterEditor : public QDialog
{
    Q_OBJECT

public:
    FilterEditor(const LogModel &model, QWidget *parent = nullptr);

    void Load(const QString &filterID, int row, const QString &filterString, int matchType);

    int GetRowToFilter() const;
    QString GetStringToFilter() const;
    int GetMatchType() const;

signals:
    void FilterSubmitted(const QString &filterID, int row, const QString &filterString, int matchType);
    void FilterTimeStampSubmitted(const QString &filterID, int row, qint64 beginTimeStamp, qint64 endTimeStamp);

private:
    const LogModel &mModel;

    QStackedWidget *mStackedWidget;
    QComboBox *mRowComboBox;
    QLineEdit *mStringLineEdit;
    QComboBox *mMatchTypeComboBox;

    QDateEdit *mBeginDateEdit;
    QTimeEdit *mBeginTimeEdit;
    QDateEdit *mEndDateEdit;
    QTimeEdit *mEndTimeEdit;

    QPushButton *mOkButton;
    QPushButton *mCancelButton;
    std::optional<QString> mFilterID;

    void SetupLayout();
    static QDateTime ConvertToQDateTime(qint64 timestamp);
    static qint64 ConvertToTimeStamp(const QDate &date, const QTime &time);

private slots:
    void OnOkClicked();
    void UpdateSelectedColumn(int index);
};
