#include "column_editor.hpp"

#include "log_model.hpp"

#include <loglib/log_configuration.hpp>
#include <loglib/log_table.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QStringList>
#include <QVBoxLayout>

#include <string>
#include <utility>

namespace
{
/// Single source of truth for the "Type" combo. Each row models one
/// user-selectable choice and resolves to a concrete `(Type, autoDetect)`
/// pair. The "Auto-detect" entry folds together
/// `Type::Any + autoDetect == true` so the user doesn't have to
/// reason about the two-field encoding; "Any (manual)" stays as a
/// distinct option for the rare case where the user wants the type
/// pinned to `Any` (treat-as-string) without re-running auto-detect.
struct TypeChoice
{
    QString label;
    loglib::LogConfiguration::Type type;
    bool autoDetect;
};

const std::vector<TypeChoice> &TypeChoices()
{
    static const std::vector<TypeChoice> TYPE_CHOICES = {
        {.label = QStringLiteral("Auto-detect"), .type = loglib::LogConfiguration::Type::Any, .autoDetect = true},
        {.label = QStringLiteral("Any (treat as string)"),
         .type = loglib::LogConfiguration::Type::Any,
         .autoDetect = false},
        {.label = QStringLiteral("String"), .type = loglib::LogConfiguration::Type::String, .autoDetect = false},
        {.label = QStringLiteral("Boolean"), .type = loglib::LogConfiguration::Type::Boolean, .autoDetect = false},
        {.label = QStringLiteral("Integer"), .type = loglib::LogConfiguration::Type::Integer, .autoDetect = false},
        {.label = QStringLiteral("Floating-point"),
         .type = loglib::LogConfiguration::Type::Floating,
         .autoDetect = false},
        {.label = QStringLiteral("Number"), .type = loglib::LogConfiguration::Type::Number, .autoDetect = false},
        {.label = QStringLiteral("Time"), .type = loglib::LogConfiguration::Type::Time, .autoDetect = false},
        {.label = QStringLiteral("Enumeration"),
         .type = loglib::LogConfiguration::Type::Enumeration,
         .autoDetect = false},
        {.label = QStringLiteral("Level"), .type = loglib::LogConfiguration::Type::Level, .autoDetect = false},
    };
    return TYPE_CHOICES;
}

int FindTypeChoiceIndex(loglib::LogConfiguration::Type type, bool autoDetect)
{
    const auto &choices = TypeChoices();
    // (1) Exact `(type, autoDetect)` match. Covers the two `Type::Any`
    // pairs and the user-pinned `(ConcreteType, false)` cases.
    for (size_t i = 0; i < choices.size(); ++i)
    {
        if (choices[i].type == type && choices[i].autoDetect == autoDetect)
        {
            return static_cast<int>(i);
        }
    }
    // (2) Auto-detector-promoted column: the streaming pipeline
    // flips `type` via `SetColumnType` but leaves `Column::autoDetect`
    // on, so a promoted column carries e.g. `(Enumeration, true)`,
    // `(String, true)`, `(Time, true)`. None of those pairs exist in
    // the table (each concrete entry is `autoDetect == false`).
    // Surface the concrete type so the combo doesn't mislead the user
    // into thinking the column is still in "Auto-detect" mode.
    // `WriteBack()`'s combo-change gate preserves the original
    // `autoDetect` flag if the user accepts without picking a
    // different entry; an explicit re-pick of the same concrete entry
    // is treated as the user-pinned intent that the entry models.
    for (size_t i = 0; i < choices.size(); ++i)
    {
        if (choices[i].type == type)
        {
            return static_cast<int>(i);
        }
    }
    // (3) Defensive: any otherwise-unrepresentable type falls back
    // to "Auto-detect" rather than leaving the combo on a stale
    // index that disagrees with the live column.
    return 0;
}

QString FormatKeys(const std::vector<std::string> &keys)
{
    if (keys.empty())
    {
        return QStringLiteral("<none>");
    }
    QStringList out;
    out.reserve(static_cast<int>(keys.size()));
    for (const auto &k : keys)
    {
        out.append(QString::fromStdString(k));
    }
    return out.join(QStringLiteral(", "));
}

QString FormatHealthLine(const std::optional<loglib::LogTable::ColumnTypeHealth> &health)
{
    if (!health.has_value())
    {
        return QStringLiteral("No health snapshot yet -- stream some data first.");
    }
    const qulonglong present = health->presentSlots;
    const qulonglong matching = health->matchingSlots;
    const qulonglong mismatched = present > matching ? present - matching : 0;
    if (present == 0)
    {
        return QStringLiteral("0 values observed.");
    }
    if (mismatched == 0)
    {
        // Translators-friendly: avoid the "%1 of %1" idiom that
        // reads like a typo when the format string is bumped to
        // ICU/plural rules.
        return QStringLiteral("All %1 values match the configured type.").arg(present);
    }
    return QStringLiteral("<span style=\"color:#b04040;\">"
                          "%1 of %2 values do not match the configured type.</span>")
        .arg(mismatched)
        .arg(present);
}
} // namespace

ColumnEditor::ColumnEditor(LogModel *model, int columnIndex, QWidget *parent)
    : QDialog(parent), mModel(model), mColumnIndex(columnIndex)
{
    setWindowTitle(tr("Edit Column"));
    setObjectName(QStringLiteral("ColumnEditor"));
    setModal(true);

    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout();
    layout->addLayout(form);

    mHeaderEdit = new QLineEdit(this);
    mHeaderEdit->setObjectName(QStringLiteral("headerEdit"));
    form->addRow(tr("Header:"), mHeaderEdit);

    mKeysLabel = new QLabel(this);
    mKeysLabel->setObjectName(QStringLiteral("keysLabel"));
    mKeysLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(tr("Keys:"), mKeysLabel);

    mTypeCombo = new QComboBox(this);
    mTypeCombo->setObjectName(QStringLiteral("typeCombo"));
    for (const auto &choice : TypeChoices())
    {
        mTypeCombo->addItem(choice.label);
    }
    form->addRow(tr("Type:"), mTypeCombo);

    mVisibleCheck = new QCheckBox(tr("Show column in the table"), this);
    mVisibleCheck->setObjectName(QStringLiteral("visibleCheck"));
    form->addRow(QString(), mVisibleCheck);

    auto *separator = new QFrame(this);
    separator->setFrameShape(QFrame::HLine);
    separator->setFrameShadow(QFrame::Sunken);
    layout->addWidget(separator);

    mHealthLabel = new QLabel(this);
    mHealthLabel->setObjectName(QStringLiteral("healthLabel"));
    mHealthLabel->setWordWrap(true);
    mHealthLabel->setTextFormat(Qt::RichText);
    layout->addWidget(mHealthLabel);

    mButtonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    mButtonBox->setObjectName(QStringLiteral("buttonBox"));
    layout->addWidget(mButtonBox);

    connect(mButtonBox, &QDialogButtonBox::accepted, this, &ColumnEditor::Apply);
    connect(mButtonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

    Populate();
}

void ColumnEditor::Populate()
{
    if (!mModel)
    {
        reject();
        return;
    }
    const auto &columns = mModel->Configuration().columns;
    if (mColumnIndex < 0 || std::cmp_greater_equal(mColumnIndex, columns.size()))
    {
        reject();
        return;
    }
    const auto &column = columns[static_cast<size_t>(mColumnIndex)];

    mHeaderEdit->setText(QString::fromStdString(column.header));
    mKeysLabel->setText(FormatKeys(column.keys));
    const int initialIndex = FindTypeChoiceIndex(column.type, column.autoDetect);
    mTypeCombo->setCurrentIndex(initialIndex);
    // Capture the seeded selection so `WriteBack()` can distinguish
    // "user left the combo alone" from "user re-picked the same
    // entry that happened to be displayed". The former preserves
    // the column's existing `(type, autoDetect)` pair (critical for
    // auto-promoted columns where the combo lands on a concrete
    // entry via the type-only fallback); the latter is a deliberate
    // pin and writes through normally.
    mInitialTypeChoiceIndex = initialIndex;
    mVisibleCheck->setChecked(column.visible);
    mHealthLabel->setText(FormatHealthLine(mModel->ColumnHealth(mColumnIndex)));
}

void ColumnEditor::Apply()
{
    WriteBack();
    accept();
}

void ColumnEditor::WriteBack()
{
    if (!mModel)
    {
        return;
    }
    const auto &columns = mModel->Configuration().columns;
    if (mColumnIndex < 0 || std::cmp_greater_equal(mColumnIndex, columns.size()))
    {
        return;
    }
    const int comboIndex = mTypeCombo->currentIndex();
    const auto &choices = TypeChoices();
    if (comboIndex < 0 || std::cmp_greater_equal(comboIndex, choices.size()))
    {
        return;
    }
    const auto &choice = choices[static_cast<size_t>(comboIndex)];

    auto &manager = mModel->ConfigurationManager();
    const auto idx = static_cast<size_t>(mColumnIndex);

    // Header / visible are independent toggles; write them first so
    // a same-batch type change still sees the new header in any
    // diagnostic surfaces. `ApplyColumnTypeEdit` (below) writes the
    // `(type, autoDetect)` pair atomically and reconciles the rows.
    manager.SetColumnHeader(idx, mHeaderEdit->text().toStdString());
    manager.SetColumnVisible(idx, mVisibleCheck->isChecked());

    // The type/autoDetect edit is the single seam responsible for:
    //   - the atomic typed write,
    //   - the encode/back-fill / dictionary teardown,
    //   - the `enumColumnsChanged` signal across the enum/level
    //     boundary,
    //   - the `columnHealthChanged` refresh.
    //
    // Only fire it when the user actually changed the combo. The
    // bare `(previousType == newType && previousAutoDetect ==
    // newAutoDetect)` short-circuit in `ApplyColumnTypeEdit` is
    // insufficient because the combo can resolve to a concrete entry
    // via `FindTypeChoiceIndex`'s type-only fallback for
    // auto-promoted columns (e.g. `(Enumeration, autoDetect=true)`
    // resolves to the "Enumeration" entry whose `autoDetect ==
    // false`). Without this gate, an accept-without-change would
    // write `(Enumeration, false)` -- silently disabling the
    // auto-detector's overflow demotion -- or, worse, route through
    // the "Auto-detect" entry and write `(Any, true)`, destroying
    // the resolved type and re-triggering detection.
    if (comboIndex != mInitialTypeChoiceIndex)
    {
        mModel->ApplyColumnTypeEdit(mColumnIndex, choice.type, choice.autoDetect);
    }

    // Push the header/decoration/visibility refresh; the model emits
    // `headerDataChanged` + a column-wide `dataChanged` from
    // `NotifyColumnEdited`, so the view repaints cells whose
    // formatting may have flipped with the new type.
    mModel->NotifyColumnEdited(mColumnIndex);
}
