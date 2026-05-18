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
        {QStringLiteral("Auto-detect"), loglib::LogConfiguration::Type::Any, true},
        {QStringLiteral("Any (treat as string)"), loglib::LogConfiguration::Type::Any, false},
        {QStringLiteral("String"), loglib::LogConfiguration::Type::String, false},
        {QStringLiteral("Boolean"), loglib::LogConfiguration::Type::Boolean, false},
        {QStringLiteral("Integer"), loglib::LogConfiguration::Type::Integer, false},
        {QStringLiteral("Floating-point"), loglib::LogConfiguration::Type::Floating, false},
        {QStringLiteral("Number"), loglib::LogConfiguration::Type::Number, false},
        {QStringLiteral("Time"), loglib::LogConfiguration::Type::Time, false},
        {QStringLiteral("Enumeration"), loglib::LogConfiguration::Type::Enumeration, false},
        {QStringLiteral("Level"), loglib::LogConfiguration::Type::Level, false},
    };
    return TYPE_CHOICES;
}

int FindTypeChoiceIndex(loglib::LogConfiguration::Type type, bool autoDetect)
{
    const auto &choices = TypeChoices();
    for (size_t i = 0; i < choices.size(); ++i)
    {
        if (choices[i].type == type && choices[i].autoDetect == autoDetect)
        {
            return static_cast<int>(i);
        }
    }
    // Defensive: any unexpected combination collapses to the
    // top "Auto-detect" entry rather than leaving the combo on a
    // stale index that disagrees with the live column.
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
    connect(mTypeCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, &ColumnEditor::OnTypeChanged);

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
    if (mColumnIndex < 0 || mColumnIndex >= static_cast<int>(columns.size()))
    {
        reject();
        return;
    }
    const auto &column = columns[static_cast<size_t>(mColumnIndex)];

    mHeaderEdit->setText(QString::fromStdString(column.header));
    mKeysLabel->setText(FormatKeys(column.keys));
    mTypeCombo->setCurrentIndex(FindTypeChoiceIndex(column.type, column.autoDetect));
    mVisibleCheck->setChecked(column.visible);
    mHealthLabel->setText(FormatHealthLine(mModel->ColumnHealth(mColumnIndex)));
}

void ColumnEditor::OnTypeChanged(int /*comboIndex*/)
{
    // Reactive preview: re-render the health line as if the new type
    // were already in effect. We do *not* compute a fresh health pass
    // here (it requires walking the table); the snapshot we hold is
    // for the *old* type, so the preview just labels it as "current"
    // until Apply runs. Keeping the label live, even at the cost of
    // a one-step-stale percentage, matches the "edit in place"
    // expectation users have from QSpinBox / QSlider previews.
    mHealthLabel->setText(FormatHealthLine(mModel ? mModel->ColumnHealth(mColumnIndex) : std::nullopt));
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
    if (mColumnIndex < 0 || mColumnIndex >= static_cast<int>(columns.size()))
    {
        return;
    }
    const int comboIndex = mTypeCombo->currentIndex();
    const auto &choices = TypeChoices();
    if (comboIndex < 0 || comboIndex >= static_cast<int>(choices.size()))
    {
        return;
    }
    const auto &choice = choices[static_cast<size_t>(comboIndex)];

    auto &manager = mModel->ConfigurationManager();
    const size_t idx = static_cast<size_t>(mColumnIndex);

    // Capture the original type before we mutate, so we can decide
    // whether the back-fill walk is worth running. A no-op type
    // change (the user clicked OK without touching the combo) skips
    // the O(rows) reconcile.
    const auto previousType = columns[idx].type;
    const bool previousAutoDetect = columns[idx].autoDetect;

    // Header / visible are independent toggles; the type-pair has to
    // land together so an intermediate read by another component
    // never sees `Type::Any + autoDetect == false` paired with the
    // wrong sibling field.
    manager.SetColumnHeader(idx, mHeaderEdit->text().toStdString());
    manager.SetColumnVisible(idx, mVisibleCheck->isChecked());
    manager.SetColumnType(idx, choice.type);
    manager.SetColumnAutoDetect(idx, choice.autoDetect);

    // Reconcile already-loaded rows with the user's type pick:
    //   - `Time`        -- run the backfill so existing strings
    //                      parse into Timestamp slots.
    //   - `Enumeration` / `Level` -- build the canonical dictionary
    //                      and encode every existing slot.
    //   - other types   -- materialise any leftover `DictRef` so
    //                      sort/filter on the new type works.
    // Skipped when neither field actually moved (no-op accept).
    const bool typeChanged = previousType != choice.type || previousAutoDetect != choice.autoDetect;
    if (typeChanged)
    {
        mModel->NotifyColumnTypeEdited(mColumnIndex);
    }

    // The diagnostics cache is keyed by (type, data); the type just
    // moved, so re-snapshot before we hand control back. This is the
    // single seam that keeps the header tooltip / decoration / status
    // bar coherent after a column-editor commit -- no other slot
    // needs to know about column-editor edits.
    mModel->RefreshColumnHealth();

    // Push the header/decoration/visibility refresh; the model emits
    // `headerDataChanged` + a column-wide `dataChanged` from
    // `NotifyColumnEdited`, so the view repaints cells whose
    // formatting may have flipped with the new type.
    mModel->NotifyColumnEdited(mColumnIndex);
}
