#include "filter_editor.hpp"

#include "theme_control.hpp"

#include <loglib/enum_dictionary.hpp>
#include <loglib/log_level.hpp>
#include <loglib/log_processing.hpp>

#include <QApplication>
#include <QDoubleValidator>
#include <QFont>
#include <QLocale>
#include <QMessageBox>
#include <QPalette>
#include <QStandardItem>
#include <QTimeZone>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <string_view>
#include <unordered_set>

using namespace loglib;

namespace
{
constexpr int ENUM_PICKER_MAX_HEIGHT_PX = 320;

/// Page indices in `mStackedWidget`. Must match the insertion order
/// in `SetupLayout`; bump together with `UpdateSelectedColumn` and
/// `UpdateEnumSelectionCount` when adding a page.
constexpr int PAGE_STRING = 0;
constexpr int PAGE_TIME = 1;
constexpr int PAGE_ENUM = 2;
constexpr int PAGE_NUMERIC = 3;
constexpr int PAGE_BOOLEAN = 4;

/// True iff @p widget paints onto a dark surface. Sampled from
/// `QPalette::Base` (the role used by text-input backgrounds and
/// list rows), which is the surface our validation feedback
/// actually contrasts against -- not `QPalette::Window`. The
/// dark / light threshold itself lives in `ThemeControl::IsDarkColor`
/// so the auto theme switch and the validation feedback share the
/// same line.
[[nodiscard]] bool IsDarkBase(const QWidget *widget)
{
    const QPalette palette = (widget != nullptr) ? widget->palette() : qApp->palette();
    return ThemeControl::IsDarkColor(palette.color(QPalette::Base));
}

/// Foreground / border warning colour tuned to remain legible on
/// both Light and Dark `QPalette::Base` backgrounds. Solid red
/// (`#FF0000`) on a dark surface fails contrast badly; a soft
/// salmon (`#FF8A80`) on a dark surface and a Material-style
/// deep red (`#D32F2F`) on a light surface both pass.
[[nodiscard]] QString WarningColorHex(const QWidget *widget)
{
    return IsDarkBase(widget) ? QStringLiteral("#FF8A80") : QStringLiteral("#D32F2F");
}

/// QSS snippet that paints @p widget's foreground in the
/// palette-aware warning colour. For check boxes (which paint
/// their label via the foreground role) and labels.
[[nodiscard]] QString WarningTextStyle(const QWidget *widget)
{
    return QStringLiteral("QCheckBox { color: %1; } QLabel { color: %1; }").arg(WarningColorHex(widget));
}

/// QSS snippet that paints @p widget's border in the
/// palette-aware warning colour. Used for line / number edits
/// where the input bytes stay legible but the chrome flags the
/// problem.
[[nodiscard]] QString WarningBorderStyle(const QWidget *widget)
{
    return QStringLiteral("border: 1px solid %1").arg(WarningColorHex(widget));
}

/// `QListView` border-only warning style for the enum picker.
[[nodiscard]] QString WarningListBorderStyle(const QWidget *widget)
{
    return QStringLiteral("QListView { border: 1px solid %1; }").arg(WarningColorHex(widget));
}
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

    mBeginUnboundedCheckBox = new QCheckBox("No begin limit", this);
    mEndUnboundedCheckBox = new QCheckBox("No end limit", this);

    mStackedWidget = new QStackedWidget(this);

    // Picker model + proxy. `setUniformItemSizes(true)` keeps layout
    // tractable up to `MAX_ENUM_VALUES`. The proxy only filters by
    // search text; `PopulateEnumValues` inserts items in locale-aware
    // order, so we don't re-sort.
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

    // Shared validator for both numeric edits. C locale pins the
    // decimal separator to '.' regardless of system locale.
    auto *numericValidator = new QDoubleValidator(this);
    QLocale cLocale = QLocale::c();
    cLocale.setNumberOptions(QLocale::RejectGroupSeparator);
    numericValidator->setLocale(cLocale);
    numericValidator->setNotation(QDoubleValidator::ScientificNotation);

    mNumericMinEdit = new QLineEdit(this);
    mNumericMinEdit->setPlaceholderText("Min value (empty = unbounded)");
    mNumericMinEdit->setValidator(numericValidator);

    mNumericMaxEdit = new QLineEdit(this);
    mNumericMaxEdit->setPlaceholderText("Max value (empty = unbounded)");
    mNumericMaxEdit->setValidator(numericValidator);

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

    // Unbounded checkbox disables the matching date/time edits and clears
    // any "both unbounded" warning border. OnOkClicked reads the checkbox
    // to decide whether to emit nullopt.
    connect(mBeginUnboundedCheckBox, &QCheckBox::toggled, this, [this](bool checked) {
        mBeginDateEdit->setEnabled(!checked);
        mBeginTimeEdit->setEnabled(!checked);
        ClearWarningStyles();
    });
    connect(mEndUnboundedCheckBox, &QCheckBox::toggled, this, [this](bool checked) {
        mEndDateEdit->setEnabled(!checked);
        mEndTimeEdit->setEnabled(!checked);
        ClearWarningStyles();
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

    // Editing either bound clears any red-border warning carried
    // over from a previous OK click. An empty edit means "unbounded"
    // on that side; the both-empty rejection styles both edits.
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

void FilterEditor::Load(int row, std::optional<qint64> begin, std::optional<qint64> end)
{
    mRowComboBox->setCurrentIndex(row);
    SetBeginEnd(begin, end);
}

void FilterEditor::Load(int row, const QStringList &selectedValues)
{
    mRowComboBox->setCurrentIndex(row);
    const auto &columns = mModel.Configuration().columns;
    const bool isLevel = row >= 0 && static_cast<size_t>(row) < columns.size() &&
                         columns[static_cast<size_t>(row)].type == LogConfiguration::Type::Level;
    if (isLevel)
    {
        PopulateLevelValues(row);

        // The picker holds canonical names ("Info", "Warn", ...) while
        // `selectedValues` may carry raw dictionary entries from a
        // prior Enumeration session ("INFO", "WARNING", custom
        // `levelMapping` aliases, ...). Resolve each saved value
        // through the alias table and key the selection by `LogLevel`
        // so a case-mismatch doesn't silently drop the tick.
        const auto &column = columns[static_cast<size_t>(row)];
        std::unordered_set<LogLevel> selectedLevels;
        selectedLevels.reserve(static_cast<size_t>(selectedValues.size()));
        for (const QString &value : selectedValues)
        {
            const QByteArray utf8 = value.toUtf8();
            const std::string_view bytes(utf8.constData(), static_cast<size_t>(utf8.size()));
            if (auto level = ResolveLevel(bytes, column.levelMapping); level.has_value())
            {
                selectedLevels.insert(*level);
            }
        }
        for (int i = 0; i < mEnumValuesModel->rowCount(); ++i)
        {
            QStandardItem *item = mEnumValuesModel->item(i);
            if (item == nullptr)
            {
                continue;
            }
            const QByteArray itemUtf8 = item->text().toUtf8();
            const std::string_view itemBytes(itemUtf8.constData(), static_cast<size_t>(itemUtf8.size()));
            const auto itemLevel = ParseLevelName(itemBytes);
            const bool checked = itemLevel.has_value() && selectedLevels.contains(*itemLevel);
            item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
        }
    }
    else
    {
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
    }
    UpdateEnumSelectionCount();
}

void FilterEditor::Load(int row, std::optional<double> minValue, std::optional<double> maxValue)
{
    mRowComboBox->setCurrentIndex(row);
    // C locale keeps the decimal separator as '.' so saved bounds
    // round-trip byte-exactly (the validator uses the same locale).
    const QLocale cLocale = QLocale::c();
    // Coerce non-finite bounds (`±inf`/`NaN` from a hand-edited
    // config) to "unbounded" (empty edit) so the dialog opens
    // submittable -- `OnOkClicked` would otherwise reject them on
    // submit and trap the user.
    const auto setEdit = [&cLocale](QLineEdit *edit, std::optional<double> v) {
        if (v.has_value() && std::isfinite(*v))
        {
            edit->setText(cLocale.toString(*v, 'g', std::numeric_limits<double>::max_digits10));
        }
        else
        {
            edit->clear();
        }
    };
    setEdit(mNumericMinEdit, minValue);
    setEdit(mNumericMaxEdit, maxValue);
}

void FilterEditor::Load(int row, bool includeTrue, bool includeFalse)
{
    mRowComboBox->setCurrentIndex(row);
    mBoolIncludeTrue->setChecked(includeTrue);
    mBoolIncludeFalse->setChecked(includeFalse);
}

void FilterEditor::SetInitialColumn(int row)
{
    const auto &columns = mModel.Configuration().columns;
    if (row < 0 || static_cast<size_t>(row) >= columns.size())
    {
        return;
    }
    // Defensive: production only passes visible columns, but
    // binding a filter to a hidden column is confusing UX.
    if (!columns[static_cast<size_t>(row)].visible)
    {
        return;
    }
    // setCurrentIndex fires `currentIndexChanged` -> `UpdateSelectedColumn`,
    // which swaps the stacked page (string / time / enum / numeric / bool).
    // No emit when the index is unchanged, but the constructor already
    // calls `UpdateSelectedColumn(0)` for that case.
    mRowComboBox->setCurrentIndex(row);
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
    // Empty edit means "unbounded"; an unparsable / NaN value also
    // collapses to unbounded for the getter (`OnOkClicked` is the
    // place that surfaces parse errors as a red border).
    const QString text = mNumericMinEdit->text();
    if (text.isEmpty())
    {
        return std::nullopt;
    }
    bool ok = false;
    const double value = QLocale::c().toDouble(text, &ok);
    if (!ok || std::isnan(value))
    {
        return std::nullopt;
    }
    return value;
}

std::optional<double> FilterEditor::GetNumericRangeMax() const
{
    const QString text = mNumericMaxEdit->text();
    if (text.isEmpty())
    {
        return std::nullopt;
    }
    bool ok = false;
    const double value = QLocale::c().toDouble(text, &ok);
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
    beginLayout->addWidget(mBeginUnboundedCheckBox);

    auto *endLayout = new QHBoxLayout;
    endLayout->addWidget(new QLabel("End Date and Time:", this));
    endLayout->addWidget(mEndDateEdit);
    endLayout->addWidget(mEndTimeEdit);
    endLayout->addWidget(mEndUnboundedCheckBox);

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
    auto *maxLayout = new QHBoxLayout();
    maxLayout->addWidget(new QLabel("Max (<=):", this));
    maxLayout->addWidget(mNumericMaxEdit);
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

    // Insert in `PAGE_*` order. Inserting out of order would shift
    // later page indices.
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

void FilterEditor::SetBeginEnd(std::optional<qint64> begin, std::optional<qint64> end)
{
    // Seed each edit with a sensible value even when its side is
    // unbounded (the user may uncheck the checkbox and start picking):
    // unbounded side reuses the other side's value, both-unbounded
    // falls back to "now".
    auto pickSeed = [](std::optional<qint64> primary, std::optional<qint64> fallback) -> QDateTime {
        if (primary.has_value())
        {
            return ConvertToQDateTime(*primary);
        }
        if (fallback.has_value())
        {
            return ConvertToQDateTime(*fallback);
        }
        return QDateTime::currentDateTime();
    };
    const QDateTime beginDateTime = pickSeed(begin, end);
    const QDateTime endDateTime = pickSeed(end, begin);

    // Apply seed + constraint per widget. The two MUST stay paired
    // (don't batch all setDateTime then all setMin/Max): each
    // setDateTime fires the cross-coupling that propagates begin <-> end
    // constraints, and reordering lets the cascade clamp to unintended
    // values. The unbounded side must explicitly clear any stale
    // constraint left over from `UpdateSelectedColumn` so an Edit on
    // e.g. `(nullopt, X)` doesn't keep begin pinned to `[X, X]`.
    enum class Side
    {
        Lower,
        Upper
    };
    auto applySeedAndBound = [](QDateTimeEdit *edit, const QDateTime &seed, std::optional<qint64> bound, Side side) {
        edit->setDateTime(seed);
        if (bound.has_value())
        {
            if (side == Side::Lower)
            {
                edit->setMinimumDateTime(seed);
            }
            else
            {
                edit->setMaximumDateTime(seed);
            }
        }
        else
        {
            if (side == Side::Lower)
            {
                edit->clearMinimumDateTime();
            }
            else
            {
                edit->clearMaximumDateTime();
            }
        }
    };

    applySeedAndBound(mBeginDateEdit, beginDateTime, begin, Side::Lower);
    applySeedAndBound(mBeginTimeEdit, beginDateTime, begin, Side::Lower);
    applySeedAndBound(mEndDateEdit, endDateTime, end, Side::Upper);
    applySeedAndBound(mEndTimeEdit, endDateTime, end, Side::Upper);

    // The toggled handler disables the matching edits as a side effect.
    mBeginUnboundedCheckBox->setChecked(!begin.has_value());
    mEndUnboundedCheckBox->setChecked(!end.has_value());
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
        // At least one bound must stay engaged; both unbounded would
        // match every row, so paint the checkboxes red and stop.
        const bool beginUnbounded = mBeginUnboundedCheckBox->isChecked();
        const bool endUnbounded = mEndUnboundedCheckBox->isChecked();
        if (beginUnbounded && endUnbounded)
        {
            const QString warning = WarningTextStyle(mBeginUnboundedCheckBox);
            mBeginUnboundedCheckBox->setStyleSheet(warning);
            mEndUnboundedCheckBox->setStyleSheet(warning);
            return;
        }
        const std::optional<qint64> beginMicros =
            beginUnbounded ? std::nullopt
                           : std::optional<qint64>{ConvertToTimeStamp(mBeginDateEdit->date(), mBeginTimeEdit->time())};
        const std::optional<qint64> endMicros =
            endUnbounded ? std::nullopt
                         : std::optional<qint64>{ConvertToTimeStamp(mEndDateEdit->date(), mEndTimeEdit->time())};
        emit FilterTimeStampSubmitted(mFilterID, index, beginMicros, endMicros);
    }
    else if (column.type == LogConfiguration::Type::Enumeration || column.type == LogConfiguration::Type::Level)
    {
        const QStringList selected = GetSelectedEnumValues();
        if (selected.isEmpty())
        {
            // Empty selection would hide every row.
            mEnumValuesView->setStyleSheet(WarningListBorderStyle(mEnumValuesView));
            return;
        }
        // Level columns submit canonical level names; the predicate
        // build expands them to raw dictionary entries via the rank cache.
        emit FilterEnumSubmitted(mFilterID, index, selected);
    }
    else if (column.type == LogConfiguration::Type::Integer || column.type == LogConfiguration::Type::Floating ||
             column.type == LogConfiguration::Type::Number)
    {
        // An empty edit means "unbounded" on that side. Insist on
        // at least one populated bound -- both empty would match
        // every numeric row.
        const QString minText = mNumericMinEdit->text();
        const QString maxText = mNumericMaxEdit->text();
        if (minText.isEmpty() && maxText.isEmpty())
        {
            const QString warning = WarningBorderStyle(mNumericMinEdit);
            mNumericMinEdit->setStyleSheet(warning);
            mNumericMaxEdit->setStyleSheet(warning);
            return;
        }
        // Reject `NaN` / `±inf`: `QLocale::toDouble` accepts them,
        // and `setText` bypasses the validator. `Load()` already
        // coerces non-finite saved bounds to "unbounded"; this is
        // the second line of defence.
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
        if (!minText.isEmpty())
        {
            minValue = parseFinite(minText);
            if (!minValue.has_value())
            {
                mNumericMinEdit->setStyleSheet(WarningBorderStyle(mNumericMinEdit));
                return;
            }
        }
        if (!maxText.isEmpty())
        {
            maxValue = parseFinite(maxText);
            if (!maxValue.has_value())
            {
                mNumericMaxEdit->setStyleSheet(WarningBorderStyle(mNumericMaxEdit));
                return;
            }
        }
        if (minValue.has_value() && maxValue.has_value() && *minValue > *maxValue)
        {
            // `min == max` is a valid single-point filter; only
            // strict `>` is rejected.
            const QString warning = WarningBorderStyle(mNumericMinEdit);
            mNumericMinEdit->setStyleSheet(warning);
            mNumericMaxEdit->setStyleSheet(warning);
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
            const QString warning = WarningTextStyle(mBoolIncludeTrue);
            mBoolIncludeTrue->setStyleSheet(warning);
            mBoolIncludeFalse->setStyleSheet(warning);
            return;
        }
        emit FilterBooleanSubmitted(mFilterID, index, includeTrue, includeFalse);
    }
    else
    {
        if (mStringLineEdit->text().isEmpty())
        {
            mStringLineEdit->setStyleSheet(WarningBorderStyle(mStringLineEdit));
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
    case LogConfiguration::Type::Level:
        mStackedWidget->setCurrentIndex(PAGE_ENUM);
        PopulateLevelValues(index);
        break;
    case LogConfiguration::Type::Integer:
    case LogConfiguration::Type::Floating:
    case LogConfiguration::Type::Number:
        mStackedWidget->setCurrentIndex(PAGE_NUMERIC);
        break;
    case LogConfiguration::Type::Boolean:
        mStackedWidget->setCurrentIndex(PAGE_BOOLEAN);
        break;
    case LogConfiguration::Type::Any:
    case LogConfiguration::Type::String:
    default:
        mStackedWidget->setCurrentIndex(PAGE_STRING);
        break;
    }
    // Reset any stale warning borders from a hidden page so the
    // next OK click starts from a clean slate.
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

void FilterEditor::PopulateLevelValues(int columnIndex)
{
    mEnumValuesModel->clear();
    mEnumSearchEdit->clear();
    if (columnIndex < 0 || static_cast<size_t>(columnIndex) >= mModel.Configuration().columns.size())
    {
        UpdateEnumSelectionCount();
        return;
    }
    const auto &column = mModel.Configuration().columns[static_cast<size_t>(columnIndex)];
    if (column.type != LogConfiguration::Type::Level)
    {
        UpdateEnumSelectionCount();
        return;
    }
    // A level is "observed" iff its `LevelRankCache` slot is not
    // `Unknown`. `nullptr` means the column is `Type::Level` but no
    // batch has populated the cache yet -- surface every level as
    // observed so saved selections still travel through and the next
    // `Grew` rebuild can correct the picker.
    const std::vector<LogLevel> *ranks = mModel.Table().LevelRankCache(static_cast<size_t>(columnIndex));
    // Indexed by raw `LogLevel` ordinal (Unknown=0..Fatal=6), so the
    // array is one larger than `CANONICAL_LEVEL_COUNT` (which excludes
    // `Unknown`). Slot 0 is unused; Trace..Fatal occupy 1..6.
    std::array<bool, CANONICAL_LEVEL_COUNT + 1> observed{};
    if (ranks == nullptr)
    {
        observed.fill(true);
    }
    else
    {
        for (const LogLevel rank : *ranks)
        {
            if (rank == LogLevel::Unknown)
            {
                continue;
            }
            const auto ordinal = static_cast<size_t>(rank);
            if (ordinal < observed.size())
            {
                observed[ordinal] = true;
            }
        }
    }

    // Show the six canonical levels in severity order. Items stay
    // togglable so a saved filter on a level that just rotated out of
    // view can still be unticked. Unobserved levels get an italic
    // font + tooltip so the user knows picking them rejects every row.
    constexpr std::array<LogLevel, CANONICAL_LEVEL_COUNT> CANONICAL_LEVELS = {
        LogLevel::Trace, LogLevel::Debug, LogLevel::Info, LogLevel::Warn, LogLevel::Error, LogLevel::Fatal
    };
    for (const LogLevel level : CANONICAL_LEVELS)
    {
        const std::string_view name = CanonicalLevelName(level);
        const QString display = QString::fromUtf8(name.data(), static_cast<int>(name.size()));
        auto *item = new QStandardItem(display);
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Unchecked);
        if (!observed[static_cast<size_t>(level)])
        {
            item->setToolTip(tr("No %1 entries observed in this column yet.").arg(display));
            QFont font = item->font();
            font.setItalic(true);
            item->setFont(font);
            // Soften the foreground so unobserved rows are visually
            // distinct but still legible across light/dark themes.
            item->setForeground(QApplication::palette().color(QPalette::Disabled, QPalette::Text));
        }
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
    mBoolIncludeTrue->setStyleSheet(QString());
    mBoolIncludeFalse->setStyleSheet(QString());
    mBeginUnboundedCheckBox->setStyleSheet(QString());
    mEndUnboundedCheckBox->setStyleSheet(QString());
}
