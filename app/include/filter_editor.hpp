#pragma once

#include "log_model.hpp"

#include <loglib/log_configuration.hpp>

#include <QComboBox>
#include <QDate>
#include <QDateTime>
#include <QDateTimeEdit>
#include <QDialog>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QStackedWidget>
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

    int GetRowToFilter() const;
    QString GetStringToFilter() const;
    int GetMatchType() const;

signals:
    void FilterSubmitted(const QString &filterID, int row, const QString &filterString, int matchType);
    void FilterTimeStampSubmitted(const QString &filterID, int row, qint64 beginTimeStamp, qint64 endTimeStamp);

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

    QPushButton *mOkButton;
    QPushButton *mCancelButton;

    void SetupLayout();
    void SetBeginEnd(qint64 begin, qint64 end);
    static QDateTime ConvertToQDateTime(qint64 timestamp);
    static qint64 ConvertToTimeStamp(const QDate &date, const QTime &time);

private slots:
    void OnOkClicked();
    void UpdateSelectedColumn(int index);
};
