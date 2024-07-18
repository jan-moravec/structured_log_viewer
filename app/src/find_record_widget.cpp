#include "find_record_widget.hpp"

#include <QHBoxLayout>
#include <QLineEdit>
#include <QCheckBox>
#include <QPushButton>
#include <QTimer>

FindRecordWidget::FindRecordWidget(QWidget *parent)
    : QWidget{parent}
{
    //this->setWindowTitle("Find");
    //this->setWindowFlags(Qt::WindowStaysOnTopHint | Qt::Tool); // Qt::FramelessWindowHint |

    QHBoxLayout *hLayout = new QHBoxLayout(this);

    //QVBoxLayout *inputLayout = new QVBoxLayout();
    //hLayout->addLayout(inputLayout);
    mEdit = new QLineEdit(this);
    hLayout->addWidget(mEdit);
    //inputLayout->addWidget(mEdit);

    //QHBoxLayout *configLayout = new QHBoxLayout();
    //inputLayout->addLayout(configLayout);
    mCheckBoxWildcards = new QCheckBox("Wildcards", this);
    hLayout->addWidget(mCheckBoxWildcards);
    //configLayout->addWidget(mCheckBoxWildcards);
    mCheckBoxRegularExpressions = new QCheckBox("Regular Expressions", this);
    hLayout->addWidget(mCheckBoxRegularExpressions);
    //configLayout->addWidget(mCheckBoxRegularExpressions);

    //QVBoxLayout *buttonLayout = new QVBoxLayout();
    //hLayout->addLayout(buttonLayout);

    mButtonNext = new QPushButton("Next", this);
    hLayout->addWidget(mButtonNext);
    //buttonLayout->addWidget(mButtonNext);
    mButtonPrevious = new QPushButton("Previous", this);
    hLayout->addWidget(mButtonPrevious);
    //buttonLayout->addWidget(mButtonPrevious);

    this->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
    mEdit->setFocus();

    QTimer::singleShot(0, mEdit, SLOT(setFocus()));

    connect(mButtonNext, &QPushButton::clicked, this, &FindRecordWidget::FindNext);
    connect(mButtonPrevious, &QPushButton::clicked, this, &FindRecordWidget::FindPrevious);
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
