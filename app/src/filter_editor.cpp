#include "filter_editor.hpp"

#include <loglib/enum_dictionary.hpp>
#include <loglib/log_processing.hpp>

#include <QDoubleValidator>
#include <QLocale>
#include <QMessageBox>
#include <QStandardItem>
#include <QTimeZone>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

using namespace loglib;

namespace
{
constexpr int ENUM_PICKER_MAX_HEIGHT_PX = 320;

/// Page indices in `mStackedWidget`. Order mirrors the `addWidget`
/// sequence in `FilterEditor::SetupLayout`; bumped together if a new
/// page is inserted so `UpdateSelectedColumn` and
/// `UpdateEnumSelectionCount` stay in lockstep.
constexpr int PAGE_STRING = 0;
constexpr int PAGE_TIME = 1;
constexpr int PAGE_ENUM = 2;
constexpr int PAGE_NUMERIC = 3;
constexpr int PAGE_BOOLEAN = 4;
} // namespace

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

    // Picker model + proxy. `setUniformItemSizes(true)` keeps layout
    // tractable up to `MAX_ENUM_VALUES`. The proxy filters by the
    // user's search text but does *not* re-sort: `PopulateEnumValues`
    // already inserts items in locale-aware order so the view matches
    // the table-column display ordering.
    mEnumValuesModel = new QStandardItemModel(this);
    mEnumValuesProxy = new QSortFilterProxyModel(this);
    mEnumValuesProxy->setSourceModel(mEnumValuesModel);
    mEnumValuesProxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
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
    mEnumEmptyPlaceholder = new QLabel("No values observed for this column yet.", this);
    mEnumEmptyPlaceholder->setAlignment(Qt::AlignCenter);
    mEnumEmptyPlaceholder->setWordWrap(true);
    mEnumEmptyPlaceholder->hide();

    // `QDoubleValidator` is shared by both numeric edits; locale is
    // forced to C so the decimal separator stays a dot regardless of
    // the user's locale.
    auto *numericValidator = new QDoubleValidator(this);
    QLocale cLocale = QLocale::c();
    cLocale.setNumberOptions(QLocale::RejectGroupSeparator);
    numericValidator->setLocale(cLocale);
    numericValidator->setNotation(QDoubleValidator::ScientificNotation);

    mNumericMinEdit = new QLineEdit(this);
    mNumericMinEdit->setPlaceholderText("Min value");
    mNumericMinEdit->setValidator(numericValidator);
    mNumericMinUnbounded = new QCheckBox("Unbounded (-inf)", this);

    mNumericMaxEdit = new QLineEdit(this);
    mNumericMaxEdit->setPlaceholderText("Max value");
    mNumericMaxEdit->setValidator(numericValidator);
    mNumericMaxUnbounded = new QCheckBox("Unbounded (+inf)", this);

    mBoolIncludeTrue = new QCheckBox("Include true", this);
    mBoolIncludeFalse = new QCheckBox("Include false", this);

    mOkButton = new QPushButton("Ok", this);
    mCancelButton = new QPushButton("Cancel", this);

    for (const auto &column : mModel.Configuration().columns)
    {
        mRowComboBox->addItem(QString::fromStdString(column.header));
    }

    mMatchTypeComboBox->addItem("Exactly", static_cast<int>(LogConfiguration::LogFilter::Match::Exactly));
    mMatchTypeComboBox->addItem("Contains", static_cast<int>(LogConfiguration::LogFilter::Match::Contains));
    mMatchTypeComboBox->addItem(
        "Regular Expression", static_cast<int>(LogConfiguration::LogFilter::Match::RegularExpression)
    );
    mMatchTypeComboBox->addItem("Wildcards", static_cast<int>(LogConfiguration::LogFilter::Match::Wildcard));

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

    connect(mEnumSearchEdit, &QLineEdit::textChanged, this, [this](const QString &text) {
        mEnumValuesProxy->setFilterFixedString(text);
        ClearWarningStyles();
    });

    // Select/Clear All operate on visible rows so they respect the search.
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

    connect(mStringLineEdit, &QLineEdit::textChanged, this, [this](const QString &) { ClearWarningStyles(); });

    // Numeric-range checkboxes toggle their line edits and clear any
    // red-border warning carried over from a previous OK click.
    connect(mNumericMinUnbounded, &QCheckBox::toggled, this, [this](bool checked) {
        mNumericMinEdit->setDisabled(checked);
        if (checked)
        {
            mNumericMinEdit->clear();
        }
        ClearWarningStyles();
    });
    connect(mNumericMaxUnbounded, &QCheckBox::toggled, this, [this](bool checked) {
        mNumericMaxEdit->setDisabled(checked);
        if (checked)
        {
            mNumericMaxEdit->clear();
        }
        ClearWarningStyles();
    });
    connect(mNumericMinEdit, &QLineEdit::textChanged, this, [this](const QString &) { ClearWarningStyles(); });
    connect(mNumericMaxEdit, &QLineEdit::textChanged, this, [this](const QString &) { ClearWarningStyles(); });

    // Boolean checkboxes: clear any warning border on toggle.
    connect(mBoolIncludeTrue, &QCheckBox::toggled, this, [this](bool) { ClearWarningStyles(); });
    connect(mBoolIncludeFalse, &QCheckBox::toggled, this, [this](bool) { ClearWarningStyles(); });

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

void FilterEditor::Load(int row, std::optional<double> minValue, std::optional<double> maxValue)
{
    mRowComboBox->setCurrentIndex(row);
    // `QLocale::c()` keeps the decimal separator a dot regardless of
    // the user's locale; the validator does the same, so the saved
    // round-trips byte-exactly.
    const QLocale cLocale = QLocale::c();
    if (minValue.has_value())
    {
        mNumericMinUnbounded->setChecked(false);
        mNumericMinEdit->setDisabled(false);
        mNumericMinEdit->setText(cLocale.toString(*minValue, 'g', std::numeric_limits<double>::max_digits10));
    }
    else
    {
        mNumericMinUnbounded->setChecked(true);
        mNumericMinEdit->clear();
        mNumericMinEdit->setDisabled(true);
    }
    if (maxValue.has_value())
    {
        mNumericMaxUnbounded->setChecked(false);
        mNumericMaxEdit->setDisabled(false);
        mNumericMaxEdit->setText(cLocale.toString(*maxValue, 'g', std::numeric_limits<double>::max_digits10));
    }
    else
    {
        mNumericMaxUnbounded->setChecked(true);
        mNumericMaxEdit->clear();
        mNumericMaxEdit->setDisabled(true);
    }
}

void FilterEditor::Load(int row, bool includeTrue, bool includeFalse)
{
    mRowComboBox->setCurrentIndex(row);
    mBoolIncludeTrue->setChecked(includeTrue);
    mBoolIncludeFalse->setChecked(includeFalse);
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
    // Source model so search-hidden checks are also included.
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

std::optional<double> FilterEditor::GetNumericRangeMin() const
{
    if (mNumericMinUnbounded->isChecked())
    {
        return std::nullopt;
    }
    bool ok = false;
    const double value = QLocale::c().toDouble(mNumericMinEdit->text(), &ok);
    if (!ok || std::isnan(value))
    {
        return std::nullopt;
    }
    return value;
}

std::optional<double> FilterEditor::GetNumericRangeMax() const
{
    if (mNumericMaxUnbounded->isChecked())
    {
        return std::nullopt;
    }
    bool ok = false;
    const double value = QLocale::c().toDouble(mNumericMaxEdit->text(), &ok);
    if (!ok || std::isnan(value))
    {
        return std::nullopt;
    }
    return value;
}

bool FilterEditor::GetBooleanIncludeTrue() const
{
    return mBoolIncludeTrue->isChecked();
}

bool FilterEditor::GetBooleanIncludeFalse() const
{
    return mBoolIncludeFalse->isChecked();
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
    // Toggled with `mEnumValuesView` by `UpdateEnumSelectionCount`.
    thirdPageLayout->addWidget(mEnumEmptyPlaceholder);
    thirdPage->setLayout(thirdPageLayout);

    auto *fourthPage = new QWidget(this);
    auto *fourthPageLayout = new QVBoxLayout(fourthPage);
    auto *minLayout = new QHBoxLayout();
    minLayout->addWidget(new QLabel("Min (>=):", this));
    minLayout->addWidget(mNumericMinEdit);
    minLayout->addWidget(mNumericMinUnbounded);
    auto *maxLayout = new QHBoxLayout();
    maxLayout->addWidget(new QLabel("Max (<=):", this));
    maxLayout->addWidget(mNumericMaxEdit);
    maxLayout->addWidget(mNumericMaxUnbounded);
    fourthPageLayout->addLayout(minLayout);
    fourthPageLayout->addLayout(maxLayout);
    fourthPage->setLayout(fourthPageLayout);

    auto *fifthPage = new QWidget(this);
    auto *fifthPageLayout = new QVBoxLayout(fifthPage);
    fifthPageLayout->addWidget(new QLabel("Boolean values to include:", this));
    fifthPageLayout->addWidget(mBoolIncludeTrue);
    fifthPageLayout->addWidget(mBoolIncludeFalse);
    fifthPageLayout->addStretch(1);
    fifthPage->setLayout(fifthPageLayout);

    // Insertion order must match the `PAGE_*` constants; an inserted
    // page would shift every later index.
    mStackedWidget->insertWidget(PAGE_STRING, firstPage);
    mStackedWidget->insertWidget(PAGE_TIME, secondPage);
    mStackedWidget->insertWidget(PAGE_ENUM, thirdPage);
    mStackedWidget->insertWidget(PAGE_NUMERIC, fourthPage);
    mStackedWidget->insertWidget(PAGE_BOOLEAN, fifthPage);
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

    if (column.type == LogConfiguration::Type::Time)
    {
        emit FilterTimeStampSubmitted(
            mFilterID,
            index,
            ConvertToTimeStamp(mBeginDateEdit->date(), mBeginTimeEdit->time()),
            ConvertToTimeStamp(mEndDateEdit->date(), mEndTimeEdit->time())
        );
    }
    else if (column.type == LogConfiguration::Type::Enumeration)
    {
        const QStringList selected = GetSelectedEnumValues();
        if (selected.isEmpty())
        {
            // Empty selection would hide every row.
            mEnumValuesView->setStyleSheet("QListView { border: 1px solid red; }");
            return;
        }
        emit FilterEnumSubmitted(mFilterID, index, selected);
    }
    else if (
        column.type == LogConfiguration::Type::Integer || column.type == LogConfiguration::Type::Floating ||
        column.type == LogConfiguration::Type::Number
    )
    {
        // Unbounded on both sides would match every row; insist on at
        // least one finite bound. Also reject a non-numeric typed
        // value when the corresponding side isn't marked unbounded.
        const bool minBounded = !mNumericMinUnbounded->isChecked();
        const bool maxBounded = !mNumericMaxUnbounded->isChecked();
        if (!minBounded && !maxBounded)
        {
            // Style the Unbounded checkboxes, not the disabled line
            // edits: the platform style overrides a border on a
            // disabled `QLineEdit`, so the user wouldn't see the
            // warning otherwise.
            mNumericMinUnbounded->setStyleSheet("QCheckBox { color: red; }");
            mNumericMaxUnbounded->setStyleSheet("QCheckBox { color: red; }");
            return;
        }
        // `QLocale::toDouble` accepts "nan" / "inf" / "+inf" / "-inf"
        // (and the validator lets them through). Either bound as
        // `NaN` would silently degenerate to "unbounded on that side"
        // via `NumericRangeRowPredicate`'s NaN-collapse, and `±inf`
        // would collapse the predicate to "reject everything"; reject
        // both at submit time so the user sees the dialog block.
        const QLocale cLocale = QLocale::c();
        auto parseFinite = [&cLocale](const QString &text) -> std::optional<double> {
            bool ok = false;
            const double value = cLocale.toDouble(text, &ok);
            if (!ok || std::isnan(value) || std::isinf(value))
            {
                return std::nullopt;
            }
            return value;
        };
        std::optional<double> minValue;
        std::optional<double> maxValue;
        if (minBounded)
        {
            minValue = parseFinite(mNumericMinEdit->text());
            if (!minValue.has_value())
            {
                mNumericMinEdit->setStyleSheet("border: 1px solid red");
                return;
            }
        }
        if (maxBounded)
        {
            maxValue = parseFinite(mNumericMaxEdit->text());
            if (!maxValue.has_value())
            {
                mNumericMaxEdit->setStyleSheet("border: 1px solid red");
                return;
            }
        }
        if (minValue.has_value() && maxValue.has_value() && *minValue > *maxValue)
        {
            // Inverted range: `min == max` is allowed (single-point
            // filter), strict `>` is the rejection.
            mNumericMinEdit->setStyleSheet("border: 1px solid red");
            mNumericMaxEdit->setStyleSheet("border: 1px solid red");
            return;
        }
        emit FilterNumericRangeSubmitted(mFilterID, index, minValue, maxValue);
    }
    else if (column.type == LogConfiguration::Type::Boolean)
    {
        const bool includeTrue = mBoolIncludeTrue->isChecked();
        const bool includeFalse = mBoolIncludeFalse->isChecked();
        if (!includeTrue && !includeFalse)
        {
            mBoolIncludeTrue->setStyleSheet("QCheckBox { color: red; }");
            mBoolIncludeFalse->setStyleSheet("QCheckBox { color: red; }");
            return;
        }
        emit FilterBooleanSubmitted(mFilterID, index, includeTrue, includeFalse);
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
    case LogConfiguration::Type::Time:
    {
        mStackedWidget->setCurrentIndex(PAGE_TIME);
        const auto minMax = mModel.GetMinMaxValues<qint64>(index);
        if (minMax.has_value())
        {
            SetBeginEnd(minMax->first, minMax->second);
        }
        break;
    }
    case LogConfiguration::Type::Enumeration:
        mStackedWidget->setCurrentIndex(PAGE_ENUM);
        PopulateEnumValues(index);
        break;
    case LogConfiguration::Type::Integer:
    case LogConfiguration::Type::Floating:
    case LogConfiguration::Type::Number:
        mStackedWidget->setCurrentIndex(PAGE_NUMERIC);
        break;
    case LogConfiguration::Type::Boolean:
        mStackedWidget->setCurrentIndex(PAGE_BOOLEAN);
        break;
    case LogConfiguration::Type::Unknown:
    case LogConfiguration::Type::Any:
    case LogConfiguration::Type::String:
    default:
        mStackedWidget->setCurrentIndex(PAGE_STRING);
        break;
    }
    // Page change can leave a stale warning border on a hidden page; a
    // single reset keeps the next OK click against a clean slate.
    ClearWarningStyles();
    // Refresh OK-enabled gating so leaving an empty enum page re-enables OK.
    UpdateEnumSelectionCount();
}

void FilterEditor::PopulateEnumValues(int columnIndex)
{
    mEnumValuesModel->clear();
    mEnumSearchEdit->clear();
    if (columnIndex < 0 || static_cast<size_t>(columnIndex) >= mModel.Configuration().columns.size())
    {
        UpdateEnumSelectionCount();
        return;
    }
    const auto &column = mModel.Configuration().columns[static_cast<size_t>(columnIndex)];
    if (column.type != LogConfiguration::Type::Enumeration)
    {
        UpdateEnumSelectionCount();
        return;
    }
    // Aliases share the dictionary, so any registered KeyId works.
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
    if (dict == nullptr || dict->Empty())
    {
        // `UpdateEnumSelectionCount` swaps in the placeholder.
        UpdateEnumSelectionCount();
        return;
    }
    // Pre-sort alphabetically; dictionary order is observation order.
    QStringList sortedValues;
    sortedValues.reserve(static_cast<qsizetype>(dict->Values().size()));
    for (const std::string &value : dict->Values())
    {
        sortedValues.append(QString::fromStdString(value));
    }
    std::ranges::sort(sortedValues, [](const QString &a, const QString &b) { return a.localeAwareCompare(b) < 0; });
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
    const int total = mEnumValuesModel->rowCount();
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

    // Enum page: show placeholder and disable OK when empty/no checks.
    // Other pages defer validation to `OnOkClicked`.
    const bool onEnumPage = mStackedWidget->currentIndex() == PAGE_ENUM;
    if (onEnumPage)
    {
        const bool empty = total == 0;
        mEnumEmptyPlaceholder->setVisible(empty);
        mEnumValuesView->setVisible(!empty);
        mEnumSearchEdit->setEnabled(!empty);
        mEnumSelectAllButton->setEnabled(!empty);
        mEnumClearAllButton->setEnabled(!empty);
        mOkButton->setEnabled(!empty && selected > 0);
    }
    else
    {
        mOkButton->setEnabled(true);
        mEnumEmptyPlaceholder->hide();
        mEnumValuesView->show();
        mEnumSearchEdit->setEnabled(true);
        mEnumSelectAllButton->setEnabled(true);
        mEnumClearAllButton->setEnabled(true);
    }
}

void FilterEditor::ClearWarningStyles()
{
    mEnumValuesView->setStyleSheet(QString());
    mStringLineEdit->setStyleSheet(QString());
    mNumericMinEdit->setStyleSheet(QString());
    mNumericMaxEdit->setStyleSheet(QString());
    mNumericMinUnbounded->setStyleSheet(QString());
    mNumericMaxUnbounded->setStyleSheet(QString());
    mBoolIncludeTrue->setStyleSheet(QString());
    mBoolIncludeFalse->setStyleSheet(QString());
}
