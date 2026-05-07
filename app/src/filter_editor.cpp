#include "filter_editor.hpp"

#include <loglib/enum_dictionary.hpp>
#include <loglib/log_processing.hpp>

#include <QMessageBox>
#include <QStandardItem>
#include <QTimeZone>

#include <algorithm>

using namespace loglib;

namespace
{
constexpr int ENUM_PICKER_MAX_HEIGHT_PX = 320;
}

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

    // Picker model + proxy: model owns one checkable item per dict
    // value, sorted alphabetically; proxy filters by the search box.
    // `setUniformItemSizes(true)` keeps the layout tractable when the
    // dictionary holds the full `MAX_ENUM_VALUES = 1024` entries.
    mEnumValuesModel = new QStandardItemModel(this);
    mEnumValuesProxy = new QSortFilterProxyModel(this);
    mEnumValuesProxy->setSourceModel(mEnumValuesModel);
    mEnumValuesProxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
    mEnumValuesProxy->setSortCaseSensitivity(Qt::CaseInsensitive);
    mEnumValuesProxy->sort(0, Qt::AscendingOrder);
    mEnumValuesView = new QListView(this);
    mEnumValuesView->setModel(mEnumValuesProxy);
    mEnumValuesView->setSelectionMode(QAbstractItemView::NoSelection);
    mEnumValuesView->setUniformItemSizes(true);
    mEnumValuesView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    mEnumValuesView->setMaximumHeight(ENUM_PICKER_MAX_HEIGHT_PX);

    mEnumSearchEdit = new QLineEdit(this);
    mEnumSearchEdit->setPlaceholderText("Filter values...");
    mEnumSearchEdit->setClearButtonEnabled(true);

    mEnumSelectAllButton = new QPushButton("Select All", this);
    mEnumClearAllButton = new QPushButton("Clear All", this);
    mEnumSelectionCount = new QLabel("0 of 0 selected", this);

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

    // Search box → proxy filter; also clears the warning border and
    // refreshes the selection-count denominator.
    connect(mEnumSearchEdit, &QLineEdit::textChanged, this, [this](const QString &text) {
        mEnumValuesProxy->setFilterFixedString(text);
        ClearWarningStyles();
    });

    // Select All / Clear All operate on *visible* rows so the bulk
    // action respects the active search filter (the typical UX in
    // Excel-style multi-select pickers).
    connect(mEnumSelectAllButton, &QPushButton::clicked, this, [this]() {
        for (int row = 0; row < mEnumValuesProxy->rowCount(); ++row)
        {
            const QModelIndex sourceIndex = mEnumValuesProxy->mapToSource(mEnumValuesProxy->index(row, 0));
            if (auto *item = mEnumValuesModel->itemFromIndex(sourceIndex); item != nullptr)
            {
                item->setCheckState(Qt::Checked);
            }
        }
    });
    connect(mEnumClearAllButton, &QPushButton::clicked, this, [this]() {
        for (int row = 0; row < mEnumValuesProxy->rowCount(); ++row)
        {
            const QModelIndex sourceIndex = mEnumValuesProxy->mapToSource(mEnumValuesProxy->index(row, 0));
            if (auto *item = mEnumValuesModel->itemFromIndex(sourceIndex); item != nullptr)
            {
                item->setCheckState(Qt::Unchecked);
            }
        }
    });

    // `dataChanged` fires on every check-state toggle; refresh the
    // count label and clear the warning border so the user sees both
    // the new "N of M" total and a clean picker as they edit.
    connect(
        mEnumValuesModel,
        &QStandardItemModel::dataChanged,
        this,
        [this](const QModelIndex &, const QModelIndex &, const QList<int> &roles) {
            if (roles.isEmpty() || roles.contains(Qt::CheckStateRole))
            {
                UpdateEnumSelectionCount();
                ClearWarningStyles();
            }
        }
    );

    // Same intent for the empty-string warning on the text editor.
    connect(mStringLineEdit, &QLineEdit::textChanged, this, [this](const QString &) { ClearWarningStyles(); });

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
    // Build a `QSet` for O(1) membership checks while we walk the
    // model. The selection list typically holds a handful of values,
    // but the dictionary can hold up to `MAX_ENUM_VALUES = 1024` so
    // an O(rows * selection.size) `contains` would be wasteful.
    const QSet<QString> selectionSet(selectedValues.cbegin(), selectedValues.cend());
    for (int i = 0; i < mEnumValuesModel->rowCount(); ++i)
    {
        QStandardItem *item = mEnumValuesModel->item(i);
        if (item == nullptr)
        {
            continue;
        }
        item->setCheckState(selectionSet.contains(item->text()) ? Qt::Checked : Qt::Unchecked);
    }
    UpdateEnumSelectionCount();
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
    // Walk the source model, not the proxy: the user's selection
    // includes items currently hidden by the search filter.
    for (int i = 0; i < mEnumValuesModel->rowCount(); ++i)
    {
        const QStandardItem *item = mEnumValuesModel->item(i);
        if (item != nullptr && item->checkState() == Qt::Checked)
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
    thirdPageLayout->addWidget(mEnumSearchEdit);
    auto *enumActionLayout = new QHBoxLayout();
    enumActionLayout->addWidget(mEnumSelectAllButton);
    enumActionLayout->addWidget(mEnumClearAllButton);
    enumActionLayout->addStretch(1);
    enumActionLayout->addWidget(mEnumSelectionCount);
    thirdPageLayout->addLayout(enumActionLayout);
    thirdPageLayout->addWidget(mEnumValuesView);
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
            // than silently swallow the click. The border is cleared
            // by `ClearWarningStyles` on the user's next interaction
            // (`mEnumValuesModel::dataChanged` /
            // `mEnumSearchEdit::textChanged`) so the picker doesn't
            // look broken after they fix the selection.
            mEnumValuesView->setStyleSheet("QListView { border: 1px solid red; }");
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
    mEnumValuesModel->clear();
    mEnumSearchEdit->clear();
    UpdateEnumSelectionCount();
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
        UpdateEnumSelectionCount();
        return;
    }
    // Snapshot the dictionary into a sorted `QStringList` once so the
    // proxy's later sort hits already-ordered data. Insertion order in
    // the dictionary is observation order, which is rarely what the
    // user wants to scan (alphabetic is the conventional choice for
    // multi-select pickers).
    QStringList sortedValues;
    sortedValues.reserve(static_cast<qsizetype>(dict->Values().size()));
    for (const std::string &value : dict->Values())
    {
        sortedValues.append(QString::fromStdString(value));
    }
    std::sort(sortedValues.begin(), sortedValues.end(), [](const QString &a, const QString &b) {
        return a.localeAwareCompare(b) < 0;
    });
    for (const QString &value : sortedValues)
    {
        auto *item = new QStandardItem(value);
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Unchecked);
        mEnumValuesModel->appendRow(item);
    }
    UpdateEnumSelectionCount();
}

void FilterEditor::UpdateEnumSelectionCount()
{
    int total = mEnumValuesModel->rowCount();
    int selected = 0;
    for (int i = 0; i < total; ++i)
    {
        const QStandardItem *item = mEnumValuesModel->item(i);
        if (item != nullptr && item->checkState() == Qt::Checked)
        {
            ++selected;
        }
    }
    mEnumSelectionCount->setText(QString("%1 of %2 selected").arg(selected).arg(total));
}

void FilterEditor::ClearWarningStyles()
{
    mEnumValuesView->setStyleSheet(QString());
    mStringLineEdit->setStyleSheet(QString());
}
