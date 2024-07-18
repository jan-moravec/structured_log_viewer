#pragma once

#include <QWidget>
#include <QLineEdit>
#include <QCheckBox>
#include <QPushButton>

class FindRecordWidget : public QWidget
{
    Q_OBJECT
public:
    explicit FindRecordWidget(QWidget *parent = nullptr);

public slots:
    void SetEditFocus();

protected:
    void closeEvent(QCloseEvent *event) override;

signals:
    void FindRecords(const QString &text, bool next, bool wildcards, bool regularExpressions);
    void Closed();

private slots:
    void FindNext();
    void FindPrevious();

private:
    QLineEdit *mEdit;
    QCheckBox *mCheckBoxWildcards;
    QCheckBox *mCheckBoxRegularExpressions;
    QPushButton *mButtonNext;
    QPushButton *mButtonPrevious;
};
