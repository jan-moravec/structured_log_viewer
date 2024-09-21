#pragma once

#include <log_configuration.hpp>

#include <QComboBox>
#include <QDialog>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include <vector>

class FilterEditor : public QDialog
{
    Q_OBJECT

public:
    FilterEditor(const std::vector<loglib::LogConfiguration::Column> &columns, QWidget *parent = nullptr);

    int GetRowToFilter() const;
    QString GetStringToFilter() const;
    Qt::MatchFlags GetMatchType() const;

signals:
    void FilterSubmitted(const QString &filterID, int row, const QString &filterString, Qt::MatchFlags matchType);

private:
    QComboBox *rowComboBox;
    QLineEdit *stringLineEdit;
    QComboBox *matchTypeComboBox;
    QPushButton *okButton;
    QPushButton *cancelButton;

    void SetupLayout();

private slots:
    void OnOkClicked();
};
