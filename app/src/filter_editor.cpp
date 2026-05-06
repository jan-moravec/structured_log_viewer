#include "filter_editor.hpp"

#include <loglib/log_processing.hpp>

#include <QLabel>
#include <QMessageBox>
#include <QTimeZone>

using namespace loglib;

FilterEditor::FilterEditor(const LogModel &model, QString filterID, QWidget *parent)
    : QDialog(parent), mModel(model), mFilterID(std::move(filterID))
{
    setWindowTitle("Filter Editor");
    mRowComboBox = new QComboBox(this);

    mStringLineEdit = new QLineEdit(this);
    mMatchTypeComboBox = new QComboBox(this);

    mBeginDateEdit = new QDateEdit(this);
    mBeginTimeEdit = new QTimeEdit(this);
    mEndDateEdit = new QDateEdit(this);
    mEndTimeEdit = new QTimeEdit(this);

    mStackedWidget = new QStackedWidget(this);

    mOkButton = new QPushButton("Ok", this);
    mCancelButton = new QPushButton("Cancel", this);

    for (const auto &column : mModel.Configuration().columns)
    {
        mRowComboBox->addItem(QString::fromStdString(column.header));
    }

    mMatchTypeComboBox->addItem("Exactly", static_cast<int>(LogConfiguration::LogFilter::Match::exactly));
    mMatchTypeComboBox->addItem("Contains", static_cast<int>(LogConfiguration::LogFilter::Match::contains));
    mMatchTypeComboBox->addItem(
        "Regular Expression", static_cast<int>(LogConfiguration::LogFilter::Match::regularExpression)
    );
    mMatchTypeComboBox->addItem("Wildcards", static_cast<int>(LogConfiguration::LogFilter::Match::wildcard));

    SetupLayout();

    connect(mOkButton, &QPushButton::clicked, this, &FilterEditor::OnOkClicked);
    connect(mCancelButton, &QPushButton::clicked, this, &FilterEditor::reject);
    connect(mRowComboBox, &QComboBox::currentIndexChanged, this, &FilterEditor::UpdateSelectedColumn);

    // Keep min/max date and time edits in sync as a contiguous range.
    QObject::connect(mBeginDateEdit, &QDateEdit::dateChanged, [this](const QDate &date) {
        mEndDateEdit->setMinimumDate(date);
    });
    QObject::connect(mBeginTimeEdit, &QTimeEdit::timeChanged, [this](const QTime &time) {
        mEndTimeEdit->setMinimumTime(time);
    });
    QObject::connect(mEndDateEdit, &QDateEdit::dateChanged, [this](const QDate &date) {
        mBeginDateEdit->setMaximumDate(date);
    });
    QObject::connect(mEndTimeEdit, &QTimeEdit::timeChanged, [this](const QTime &time) {
        mBeginTimeEdit->setMaximumTime(time);
    });

    UpdateSelectedColumn(0);
}

void FilterEditor::Load(int row, const QString &filterString, int matchType)
{
    mRowComboBox->setCurrentIndex(row);
    mStringLineEdit->setText(filterString);
    mMatchTypeComboBox->setCurrentIndex(mMatchTypeComboBox->findData(QVariant(matchType)));
}

void FilterEditor::Load(int row, const qint64 begin, qint64 end)
{
    mRowComboBox->setCurrentIndex(row);
    SetBeginEnd(begin, end);
}

int FilterEditor::GetRowToFilter() const
{
    return mRowComboBox->currentIndex();
}

QString FilterEditor::GetStringToFilter() const
{
    return mStringLineEdit->text();
}

int FilterEditor::GetMatchType() const
{
    return mMatchTypeComboBox->currentData().toInt();
}

void FilterEditor::SetupLayout()
{
    auto *mainLayout = new QVBoxLayout(this);

    auto *rowLayout = new QHBoxLayout();
    rowLayout->addWidget(new QLabel("Row to filter:", this));
    rowLayout->addWidget(mRowComboBox);
    mainLayout->addLayout(rowLayout);

    auto *stringLayout = new QHBoxLayout();
    stringLayout->addWidget(new QLabel("String to filter:", this));
    stringLayout->addWidget(mStringLineEdit);

    auto *matchLayout = new QHBoxLayout();
    matchLayout->addWidget(new QLabel("Match type:", this));
    matchLayout->addWidget(mMatchTypeComboBox);

    auto *firstPage = new QWidget(this);
    auto *firstLayout = new QVBoxLayout(firstPage);
    firstLayout->addLayout(stringLayout);
    firstLayout->addLayout(matchLayout);
    firstPage->setLayout(firstLayout);

    mBeginTimeEdit->setDisplayFormat("HH:mm:ss.zzz");
    mEndTimeEdit->setDisplayFormat("HH:mm:ss.zzz");

    auto *secondPage = new QWidget(this);
    auto *secondPageLayout = new QVBoxLayout(secondPage);
    auto *beginLayout = new QHBoxLayout;
    beginLayout->addWidget(new QLabel("Begin Date and Time:", this));
    beginLayout->addWidget(mBeginDateEdit);
    beginLayout->addWidget(mBeginTimeEdit);

    auto *endLayout = new QHBoxLayout;
    endLayout->addWidget(new QLabel("End Date and Time:", this));
    endLayout->addWidget(mEndDateEdit);
    endLayout->addWidget(mEndTimeEdit);

    secondPageLayout->addLayout(beginLayout);
    secondPageLayout->addLayout(endLayout);

    mStackedWidget->addWidget(firstPage);
    mStackedWidget->addWidget(secondPage);
    mainLayout->addWidget(mStackedWidget);

    auto *buttonLayout = new QHBoxLayout();
    buttonLayout->addWidget(mOkButton);
    buttonLayout->addWidget(mCancelButton);
    mainLayout->addStretch(1);
    mainLayout->addLayout(buttonLayout);
}

void FilterEditor::SetBeginEnd(qint64 begin, qint64 end)
{
    const QDateTime beginDateTime = ConvertToQDateTime(begin);
    const QDateTime endDateTime = ConvertToQDateTime(end);

    mBeginDateEdit->setDateTime(beginDateTime);
    mBeginDateEdit->setMinimumDateTime(beginDateTime);
    mBeginTimeEdit->setDateTime(beginDateTime);
    mBeginTimeEdit->setMinimumDateTime(beginDateTime);

    mEndDateEdit->setDateTime(endDateTime);
    mEndDateEdit->setMaximumDateTime(endDateTime);
    mEndTimeEdit->setDateTime(endDateTime);
    mEndTimeEdit->setMaximumDateTime(endDateTime);
}

QDateTime FilterEditor::ConvertToQDateTime(qint64 timestamp)
{
    return QDateTime::fromMSecsSinceEpoch(UtcMicrosecondsToLocalMilliseconds(timestamp), QTimeZone::systemTimeZone());
}

qint64 FilterEditor::ConvertToTimeStamp(const QDate &date, const QTime &time)
{
    const QDateTime dateTime(date, time, QTimeZone::systemTimeZone());
    return dateTime.toMSecsSinceEpoch() * 1000;
}

void FilterEditor::OnOkClicked()
{
    const int index = mRowComboBox->currentIndex();
    if (index < 0 || static_cast<size_t>(index) >= mModel.Configuration().columns.size())
    {
        return;
    }

    const auto &column = mModel.Configuration().columns[static_cast<size_t>(index)];
    if (column.type != LogConfiguration::Type::time && mStringLineEdit->text().isEmpty())
    {
        mStringLineEdit->setStyleSheet("border: 1px solid red");
        return;
    }

    if (column.type == LogConfiguration::Type::time)
    {
        emit FilterTimeStampSubmitted(
            mFilterID,
            index,
            ConvertToTimeStamp(mBeginDateEdit->date(), mBeginTimeEdit->time()),
            ConvertToTimeStamp(mEndDateEdit->date(), mEndTimeEdit->time())
        );
    }
    else
    {
        emit FilterSubmitted(mFilterID, index, GetStringToFilter(), GetMatchType());
    }

    accept();
}

void FilterEditor::UpdateSelectedColumn(int index)
{
    if (index < 0 || static_cast<size_t>(index) >= mModel.Configuration().columns.size())
    {
        return;
    }
    if (mModel.Configuration().columns[static_cast<size_t>(index)].type == LogConfiguration::Type::time)
    {
        mStackedWidget->setCurrentIndex(1);

        const auto minMax = mModel.GetMinMaxValues<qint64>(index);
        if (minMax.has_value())
        {
            SetBeginEnd(minMax->first, minMax->second);
        }
    }
    else
    {
        mStackedWidget->setCurrentIndex(0);
    }
}
