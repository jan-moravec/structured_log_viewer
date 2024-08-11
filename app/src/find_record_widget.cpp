#include "find_record_widget.hpp"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QTimer>

FindRecordWidget::FindRecordWidget(QWidget *parent) : QWidget{parent}
{
    QHBoxLayout *hLayout = new QHBoxLayout(this);

    mEdit = new QLineEdit(this);
    hLayout->addWidget(mEdit);
    mCheckBoxWildcards = new QCheckBox("Wildcards", this);
    hLayout->addWidget(mCheckBoxWildcards);
    mCheckBoxRegularExpressions = new QCheckBox("Regular Expressions", this);
    hLayout->addWidget(mCheckBoxRegularExpressions);
    mButtonNext = new QPushButton("Next", this);
    hLayout->addWidget(mButtonNext);
    mButtonPrevious = new QPushButton("Previous", this);
    hLayout->addWidget(mButtonPrevious);
    mButtonClose = new QToolButton(this);
    mButtonClose->setText("X");
    hLayout->addWidget(mButtonClose);

    this->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
    mEdit->setFocus();

    QTimer::singleShot(0, mEdit, SLOT(setFocus()));

    connect(mButtonNext, &QPushButton::clicked, this, &FindRecordWidget::FindNext);
    connect(mButtonPrevious, &QPushButton::clicked, this, &FindRecordWidget::FindPrevious);
    connect(mButtonClose, &QPushButton::clicked, this, &FindRecordWidget::hide);
}

void FindRecordWidget::SetEditFocus()
{
    mEdit->setFocus();
    mEdit->selectAll();
}

void FindRecordWidget::closeEvent(QCloseEvent *event)
{
    emit Closed();
    QWidget::closeEvent(event);
}

void FindRecordWidget::FindNext()
{
    if (mEdit->text().isEmpty())
    {
        return;
    }
    emit FindRecords(mEdit->text(), true, mCheckBoxWildcards->isChecked(), mCheckBoxRegularExpressions->isChecked());
}

void FindRecordWidget::FindPrevious()
{
    if (mEdit->text().isEmpty())
    {
        return;
    }
    emit FindRecords(mEdit->text(), false, mCheckBoxWildcards->isChecked(), mCheckBoxRegularExpressions->isChecked());
}
