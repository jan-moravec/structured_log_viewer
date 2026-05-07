#include "filter_editor.hpp"

#include <loglib/enum_dictionary.hpp>
#include <loglib/log_processing.hpp>

#include <QLabel>
#include <QListWidgetItem>
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

    mEnumValuesList = new QListWidget(this);
    mEnumValuesList->setSelectionMode(QAbstractItemView::NoSelection);

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

void FilterEditor::Load(int row, const QStringList &selectedValues)
{
    mRowComboBox->setCurrentIndex(row);
    PopulateEnumValues(row);
    for (int i = 0; i < mEnumValuesList->count(); ++i)
    {
        QListWidgetItem *item = mEnumValuesList->item(i);
        const Qt::CheckState state = selectedValues.contains(item->text()) ? Qt::Checked : Qt::Unchecked;
        item->setCheckState(state);
    }
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

QStringList FilterEditor::GetSelectedEnumValues() const
{
    QStringList selected;
    for (int i = 0; i < mEnumValuesList->count(); ++i)
    {
        const QListWidgetItem *item = mEnumValuesList->item(i);
        if (item->checkState() == Qt::Checked)
        {
            selected.append(item->text());
        }
    }
    return selected;
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

    auto *thirdPage = new QWidget(this);
    auto *thirdPageLayout = new QVBoxLayout(thirdPage);
    thirdPageLayout->addWidget(new QLabel("Values to include:", this));
    thirdPageLayout->addWidget(mEnumValuesList);
    thirdPage->setLayout(thirdPageLayout);

    mStackedWidget->addWidget(firstPage);
    mStackedWidget->addWidget(secondPage);
    mStackedWidget->addWidget(thirdPage);
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
    constexpr qint64 MICROSECONDS_PER_MILLISECOND = 1000;
    const QDateTime dateTime(date, time, QTimeZone::systemTimeZone());
    return dateTime.toMSecsSinceEpoch() * MICROSECONDS_PER_MILLISECOND;
}

void FilterEditor::OnOkClicked()
{
    const int index = mRowComboBox->currentIndex();
    if (index < 0 || static_cast<size_t>(index) >= mModel.Configuration().columns.size())
    {
        return;
    }

    const auto &column = mModel.Configuration().columns[static_cast<size_t>(index)];

    if (column.type == LogConfiguration::Type::time)
    {
        emit FilterTimeStampSubmitted(
            mFilterID,
            index,
            ConvertToTimeStamp(mBeginDateEdit->date(), mBeginTimeEdit->time()),
            ConvertToTimeStamp(mEndDateEdit->date(), mEndTimeEdit->time())
        );
    }
    else if (column.type == LogConfiguration::Type::enumeration)
    {
        const QStringList selected = GetSelectedEnumValues();
        if (selected.isEmpty())
        {
            // Empty selection would filter every row out; warn rather
            // than silently swallow the click.
            mEnumValuesList->setStyleSheet("border: 1px solid red");
            return;
        }
        emit FilterEnumSubmitted(mFilterID, index, selected);
    }
    else
    {
        if (mStringLineEdit->text().isEmpty())
        {
            mStringLineEdit->setStyleSheet("border: 1px solid red");
            return;
        }
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
    const auto &column = mModel.Configuration().columns[static_cast<size_t>(index)];
    switch (column.type)
    {
    case LogConfiguration::Type::time:
    {
        mStackedWidget->setCurrentIndex(1);
        const auto minMax = mModel.GetMinMaxValues<qint64>(index);
        if (minMax.has_value())
        {
            SetBeginEnd(minMax->first, minMax->second);
        }
        break;
    }
    case LogConfiguration::Type::enumeration:
        mStackedWidget->setCurrentIndex(2);
        PopulateEnumValues(index);
        break;
    case LogConfiguration::Type::any:
    default:
        mStackedWidget->setCurrentIndex(0);
        break;
    }
}

void FilterEditor::PopulateEnumValues(int columnIndex)
{
    mEnumValuesList->clear();
    if (columnIndex < 0 || static_cast<size_t>(columnIndex) >= mModel.Configuration().columns.size())
    {
        return;
    }
    const auto &column = mModel.Configuration().columns[static_cast<size_t>(columnIndex)];
    if (column.type != LogConfiguration::Type::enumeration)
    {
        return;
    }
    // Resolve any of the column's keys against the registry; the
    // canonical/alias indirection means any registered KeyId returns
    // the same dictionary.
    const auto &registry = mModel.Table().EnumDictionaries();
    const auto &keys = mModel.Table().Keys();
    const EnumDictionary *dict = nullptr;
    for (const std::string &key : column.keys)
    {
        const KeyId id = keys.Find(key);
        if (id == INVALID_KEY_ID)
        {
            continue;
        }
        dict = registry.Find(id);
        if (dict != nullptr)
        {
            break;
        }
    }
    if (dict == nullptr)
    {
        return;
    }
    for (const std::string &value : dict->Values())
    {
        auto *item = new QListWidgetItem(QString::fromStdString(value), mEnumValuesList);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Unchecked);
    }
}
