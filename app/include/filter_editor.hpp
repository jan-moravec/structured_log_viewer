#pragma once

#include <log_configuration.hpp>

#include <QComboBox>
#include <QDialog>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include <optional>
#include <vector>

class FilterEditor : public QDialog
{
    Q_OBJECT

public:
    FilterEditor(const std::vector<loglib::LogConfiguration::Column> &columns, QWidget *parent = nullptr);

    void Load(const QString &filterID, int row, const QString &filterString, int matchType);

    int GetRowToFilter() const;
    QString GetStringToFilter() const;
    int GetMatchType() const;

signals:
    void FilterSubmitted(const QString &filterID, int row, const QString &filterString, int matchType);

private:
    QComboBox *rowComboBox;
    QLineEdit *stringLineEdit;
    QComboBox *matchTypeComboBox;
    QPushButton *okButton;
    QPushButton *cancelButton;
    std::optional<QString> mFilterID;

    void SetupLayout();

private slots:
    void OnOkClicked();
};
