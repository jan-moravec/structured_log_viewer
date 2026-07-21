#include "highlight_rules_editor.hpp"

#include "theme_control.hpp"

#include <loglib/theme.hpp>

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QSpacerItem>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusTipEvent>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidgetAction>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>

namespace
{

constexpr int STATUS_CLEAR_MS = 4000;
constexpr int SWATCH_ICON_FALLBACK_PX = 16;
constexpr int SWATCH_PAINT_INSET = 1;
constexpr int SWATCH_CORNER_RADIUS = 3;

/// Order matches `HighlightRule::Type`. The combo shows this list;
/// the `mMatchStack` uses the same index. Time / Enumeration land
/// on a read-only pane in v1 (the parse / render path still
/// handles them for hand-authored rules).
constexpr std::array<const char *, 5> TYPE_LABELS = {
    "Text (string)", "Time", "Enumeration", "Number", "Boolean"
};

/// `HighlightRule::Match` -> combo item label. Matches the wire
/// order pinned by the Glaze meta.
constexpr std::array<const char *, 4> STRING_MATCH_LABELS = {"Exactly", "Contains", "Regular expression", "Wildcard"};

[[nodiscard]] QIcon RenderSwatchIcon(
    const QBrush &background, const QBrush &foreground, int sizePx, bool paintBorder
)
{
    QPixmap pix(sizePx, sizePx);
    pix.fill(Qt::transparent);
    QPainter painter(&pix);
    painter.setRenderHint(QPainter::Antialiasing, true);
    if (background.style() != Qt::NoBrush)
    {
        painter.setBrush(background);
    }
    else
    {
        // "Inherit" / unset -- render a transparent tile with a
        // diagonal slash so the empty state reads visually.
        painter.setBrush(Qt::transparent);
    }
    if (paintBorder)
    {
        painter.setPen(
            QPen(foreground.style() != Qt::NoBrush ? foreground.color() : QColor(Qt::gray), 1)
        );
    }
    else
    {
        painter.setPen(Qt::NoPen);
    }
    painter.drawRoundedRect(
        QRectF(SWATCH_PAINT_INSET, SWATCH_PAINT_INSET, sizePx - 1, sizePx - 1),
        SWATCH_CORNER_RADIUS,
        SWATCH_CORNER_RADIUS
    );
    return QIcon{pix};
}

} // namespace

HighlightRulesEditor::HighlightRulesEditor(
    std::vector<loglib::LogConfiguration::HighlightRule> rules,
    std::vector<loglib::LogConfiguration::Column> columns,
    ThemeControl *theme,
    QWidget *parent
)
    : QWidget(parent, Qt::Window),
      mColumns(std::move(columns)),
      mTheme(theme),
      mLocalRules(std::move(rules)),
      mBaseline(mLocalRules)
{
    setWindowTitle(tr("Highlight rules"));
    setAttribute(Qt::WA_DeleteOnClose, false);
    resize(720, 480);

    // -- List side ---------------------------------------------------------
    mListWidget = new QListWidget(this);
    mListWidget->setSelectionMode(QAbstractItemView::SingleSelection);
    mListWidget->setUniformItemSizes(true);
    connect(mListWidget, &QListWidget::currentRowChanged, this, [this](int) { OnSelectionChanged(); });

    mNewButton = new QPushButton(tr("&New"), this);
    mDuplicateButton = new QPushButton(tr("D&uplicate"), this);
    mDeleteButton = new QPushButton(tr("&Delete"), this);
    mMoveUpButton = new QPushButton(tr("Move u&p"), this);
    mMoveDownButton = new QPushButton(tr("Move do&wn"), this);
    connect(mNewButton, &QPushButton::clicked, this, &HighlightRulesEditor::OnNewClicked);
    connect(mDuplicateButton, &QPushButton::clicked, this, &HighlightRulesEditor::OnDuplicateClicked);
    connect(mDeleteButton, &QPushButton::clicked, this, &HighlightRulesEditor::OnDeleteClicked);
    connect(mMoveUpButton, &QPushButton::clicked, this, &HighlightRulesEditor::OnMoveUpClicked);
    connect(mMoveDownButton, &QPushButton::clicked, this, &HighlightRulesEditor::OnMoveDownClicked);

    auto *listButtons = new QHBoxLayout();
    listButtons->addWidget(mNewButton);
    listButtons->addWidget(mDuplicateButton);
    listButtons->addWidget(mDeleteButton);
    listButtons->addStretch();
    listButtons->addWidget(mMoveUpButton);
    listButtons->addWidget(mMoveDownButton);

    auto *leftLayout = new QVBoxLayout();
    leftLayout->addLayout(listButtons);
    leftLayout->addWidget(mListWidget);
    auto *leftPane = new QWidget(this);
    leftPane->setLayout(leftLayout);

    // -- Form side ---------------------------------------------------------
    mNameEdit = new QLineEdit(this);
    mEnabledCheck = new QCheckBox(tr("Enabled"), this);
    mEnabledCheck->setChecked(true);
    mColumnCombo = new QComboBox(this);
    mTypeCombo = new QComboBox(this);
    for (const char *label : TYPE_LABELS)
    {
        mTypeCombo->addItem(QString::fromLatin1(label));
    }
    connect(mNameEdit, &QLineEdit::textEdited, this, [this](const QString &) { OnFieldEdited(); });
    connect(mEnabledCheck, &QCheckBox::toggled, this, [this](bool) { OnFieldEdited(); });
    connect(
        mColumnCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) { OnColumnChanged(); }
    );
    connect(mTypeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) { OnTypeChanged(); });

    // String pane
    mStringMatchCombo = new QComboBox(this);
    for (const char *label : STRING_MATCH_LABELS)
    {
        mStringMatchCombo->addItem(QString::fromLatin1(label));
    }
    mStringNeedleEdit = new QLineEdit(this);
    mStringNeedleEdit->setPlaceholderText(tr("Pattern or literal"));
    connect(
        mStringMatchCombo,
        QOverload<int>::of(&QComboBox::currentIndexChanged),
        this,
        [this](int) { OnFieldEdited(); }
    );
    connect(mStringNeedleEdit, &QLineEdit::textEdited, this, [this](const QString &) { OnFieldEdited(); });

    auto *stringPane = new QWidget(this);
    {
        auto *layout = new QHBoxLayout(stringPane);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(mStringMatchCombo);
        layout->addWidget(mStringNeedleEdit, 1);
    }

    // Number pane
    mNumberMinEnabled = new QCheckBox(tr("Min"), this);
    mNumberMinValue = new QDoubleSpinBox(this);
    mNumberMinValue->setRange(std::numeric_limits<double>::lowest(), std::numeric_limits<double>::max());
    mNumberMinValue->setDecimals(6);
    mNumberMaxEnabled = new QCheckBox(tr("Max"), this);
    mNumberMaxValue = new QDoubleSpinBox(this);
    mNumberMaxValue->setRange(std::numeric_limits<double>::lowest(), std::numeric_limits<double>::max());
    mNumberMaxValue->setDecimals(6);
    connect(mNumberMinEnabled, &QCheckBox::toggled, this, [this](bool on) {
        mNumberMinValue->setEnabled(on);
        OnFieldEdited();
    });
    connect(
        mNumberMinValue,
        QOverload<double>::of(&QDoubleSpinBox::valueChanged),
        this,
        [this](double) { OnFieldEdited(); }
    );
    connect(mNumberMaxEnabled, &QCheckBox::toggled, this, [this](bool on) {
        mNumberMaxValue->setEnabled(on);
        OnFieldEdited();
    });
    connect(
        mNumberMaxValue,
        QOverload<double>::of(&QDoubleSpinBox::valueChanged),
        this,
        [this](double) { OnFieldEdited(); }
    );

    auto *numberPane = new QWidget(this);
    {
        auto *layout = new QHBoxLayout(numberPane);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(mNumberMinEnabled);
        layout->addWidget(mNumberMinValue, 1);
        layout->addSpacing(12);
        layout->addWidget(mNumberMaxEnabled);
        layout->addWidget(mNumberMaxValue, 1);
    }

    // Boolean pane
    mBoolIncludeTrue = new QCheckBox(tr("true"), this);
    mBoolIncludeFalse = new QCheckBox(tr("false"), this);
    connect(mBoolIncludeTrue, &QCheckBox::toggled, this, [this](bool) { OnFieldEdited(); });
    connect(mBoolIncludeFalse, &QCheckBox::toggled, this, [this](bool) { OnFieldEdited(); });

    auto *boolPane = new QWidget(this);
    {
        auto *layout = new QHBoxLayout(boolPane);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(new QLabel(tr("Match:"), this));
        layout->addWidget(mBoolIncludeTrue);
        layout->addWidget(mBoolIncludeFalse);
        layout->addStretch();
    }

    // Read-only pane for Time / Enumeration.
    mReadOnlyLabel = new QLabel(
        tr("This rule type isn't editable from the GUI in this version.\n"
           "Time and Enumeration rules can be edited by hand in the configuration file."),
        this
    );
    mReadOnlyLabel->setWordWrap(true);
    auto *readOnlyPane = new QWidget(this);
    {
        auto *layout = new QVBoxLayout(readOnlyPane);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(mReadOnlyLabel);
    }

    mMatchStack = new QStackedWidget(this);
    // Order MUST match `HighlightRule::Type` enum order:
    //   0 = String, 1 = Time, 2 = Enumeration, 3 = Number, 4 = Boolean.
    mMatchStack->insertWidget(0, stringPane);
    mMatchStack->insertWidget(1, readOnlyPane);
    mMatchStack->insertWidget(2, readOnlyPane);
    mMatchStack->insertWidget(3, numberPane);
    mMatchStack->insertWidget(4, boolPane);

    // Style pickers
    mForegroundButton = BuildSwatchButton(/*isForeground=*/true);
    mBackgroundButton = BuildSwatchButton(/*isForeground=*/false);
    mBoldCheck = new QCheckBox(tr("Bold"), this);
    mItalicCheck = new QCheckBox(tr("Italic"), this);
    connect(mBoldCheck, &QCheckBox::toggled, this, [this](bool) { OnFieldEdited(); });
    connect(mItalicCheck, &QCheckBox::toggled, this, [this](bool) { OnFieldEdited(); });

    auto *stylePane = new QGroupBox(tr("Rendering"), this);
    {
        auto *layout = new QHBoxLayout(stylePane);
        layout->addWidget(new QLabel(tr("Background:"), this));
        layout->addWidget(mBackgroundButton);
        layout->addSpacing(12);
        layout->addWidget(new QLabel(tr("Foreground:"), this));
        layout->addWidget(mForegroundButton);
        layout->addSpacing(12);
        layout->addWidget(mBoldCheck);
        layout->addWidget(mItalicCheck);
        layout->addStretch();
    }

    // Form layout
    auto *formGrid = new QGridLayout();
    int row = 0;
    formGrid->addWidget(new QLabel(tr("Name:"), this), row, 0);
    formGrid->addWidget(mNameEdit, row++, 1);
    formGrid->addWidget(mEnabledCheck, row++, 1);
    formGrid->addWidget(new QLabel(tr("Column:"), this), row, 0);
    formGrid->addWidget(mColumnCombo, row++, 1);
    formGrid->addWidget(new QLabel(tr("Match type:"), this), row, 0);
    formGrid->addWidget(mTypeCombo, row++, 1);
    formGrid->addWidget(new QLabel(tr("Match:"), this), row, 0);
    formGrid->addWidget(mMatchStack, row++, 1);
    formGrid->addWidget(stylePane, row++, 0, 1, 2);
    formGrid->setColumnStretch(1, 1);

    auto *rightLayout = new QVBoxLayout();
    rightLayout->addLayout(formGrid);
    rightLayout->addStretch();
    auto *rightPane = new QWidget(this);
    rightPane->setLayout(rightLayout);

    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->addWidget(leftPane);
    splitter->addWidget(rightPane);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 2);

    // -- Bottom bar --------------------------------------------------------
    mStatusLabel = new QLabel(this);
    mStatusLabel->setWordWrap(true);
    mStatusClearTimer = new QTimer(this);
    mStatusClearTimer->setSingleShot(true);
    connect(mStatusClearTimer, &QTimer::timeout, this, [this]() { mStatusLabel->clear(); });

    mSaveButton = new QPushButton(tr("&Save"), this);
    mSaveButton->setDefault(true);
    mRevertButton = new QPushButton(tr("&Revert"), this);
    mCloseButton = new QPushButton(tr("&Close"), this);
    connect(mSaveButton, &QPushButton::clicked, this, &HighlightRulesEditor::OnSaveClicked);
    connect(mRevertButton, &QPushButton::clicked, this, &HighlightRulesEditor::OnRevertClicked);
    connect(mCloseButton, &QPushButton::clicked, this, &HighlightRulesEditor::close);

    auto *bottomBar = new QHBoxLayout();
    bottomBar->addWidget(mStatusLabel, 1);
    bottomBar->addWidget(mSaveButton);
    bottomBar->addWidget(mRevertButton);
    bottomBar->addWidget(mCloseButton);

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->addWidget(splitter, 1);
    mainLayout->addLayout(bottomBar);

    // Repaint on theme change.
    if (mTheme != nullptr)
    {
        connect(mTheme, &ThemeControl::themeChanged, this, [this]() {
            // Palette moved; rebuild swatch menus + list icons.
            delete mForegroundButton->menu();
            delete mBackgroundButton->menu();
            // Rebuild the tool button state.
            const QPoint fgPos = mForegroundButton->pos();
            const QPoint bgPos = mBackgroundButton->pos();
            Q_UNUSED(fgPos);
            Q_UNUSED(bgPos);
            // Simplest: recreate the swatch buttons in place would
            // require reparenting; instead just rebuild the list.
            RebuildList(mCurrentRow);
        });
    }

    RepopulateColumnCombo();
    RebuildList(mLocalRules.empty() ? -1 : 0);
    UpdateFormEnabled();
    UpdateListButtons();
}

QToolButton *HighlightRulesEditor::BuildSwatchButton(bool isForeground)
{
    auto *button = new QToolButton(this);
    button->setPopupMode(QToolButton::InstantPopup);
    button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    button->setIconSize(QSize(SwatchIconSizePx() + 4, SwatchIconSizePx() + 4));

    auto *menu = new QMenu(button);
    // First entry: "Inherit" (rule.foregroundIndex / backgroundIndex = 0).
    QAction *inherit = menu->addAction(tr("Inherit"));
    inherit->setData(0);
    connect(inherit, &QAction::triggered, this, [this, button, isForeground]() {
        if (isForeground)
        {
            mSelectedForegroundIndex = 0;
        }
        else
        {
            mSelectedBackgroundIndex = 0;
        }
        button->setIcon(RenderSwatchIcon(QBrush{}, QBrush(Qt::gray), SwatchIconSizePx(), true));
        button->setText(tr("Inherit"));
        OnFieldEdited();
    });

    if (mTheme != nullptr)
    {
        for (std::uint8_t slot = 1; slot <= loglib::HIGHLIGHT_PALETTE_SIZE; ++slot)
        {
            const QBrush bg = mTheme->HighlightBrushFor(slot, Qt::BackgroundRole);
            const QBrush fg = mTheme->HighlightBrushFor(slot, Qt::ForegroundRole);
            QAction *act = menu->addAction(RenderSwatchIcon(bg, fg, SwatchIconSizePx(), true), tr("Slot %1").arg(slot));
            act->setData(slot);
            connect(act, &QAction::triggered, this, [this, button, slot, bg, fg, isForeground]() {
                if (isForeground)
                {
                    mSelectedForegroundIndex = slot;
                }
                else
                {
                    mSelectedBackgroundIndex = slot;
                }
                button->setIcon(RenderSwatchIcon(bg, fg, SwatchIconSizePx(), true));
                OnFieldEdited();
            });
        }
    }
    button->setMenu(menu);
    // Default icon: "inherit" swatch.
    button->setIcon(RenderSwatchIcon(QBrush{}, QBrush(Qt::gray), SwatchIconSizePx(), true));
    return button;
}

int HighlightRulesEditor::SwatchIconSizePx() const
{
    if (const QStyle *s = style(); s != nullptr)
    {
        const int metric = s->pixelMetric(QStyle::PM_SmallIconSize, nullptr, this);
        if (metric > 0)
        {
            return metric;
        }
    }
    return SWATCH_ICON_FALLBACK_PX;
}

QString HighlightRulesEditor::FormatListLabel(const loglib::LogConfiguration::HighlightRule &rule) const
{
    QString label = QString::fromStdString(rule.name);
    if (label.isEmpty())
    {
        label = tr("(unnamed)");
    }
    if (!rule.enabled)
    {
        label += tr(" [disabled]");
    }
    if (ResolveColumnIndex(rule) < 0)
    {
        label += tr(" [inactive]");
    }
    return label;
}

QIcon HighlightRulesEditor::FormatListIcon(const loglib::LogConfiguration::HighlightRule &rule, int sizePx) const
{
    if (mTheme == nullptr)
    {
        return {};
    }
    const QBrush bg = mTheme->HighlightBrushFor(rule.backgroundIndex, Qt::BackgroundRole);
    const QBrush fg = mTheme->HighlightBrushFor(rule.foregroundIndex, Qt::ForegroundRole);
    return RenderSwatchIcon(bg, fg, sizePx, true);
}

int HighlightRulesEditor::ResolveColumnIndex(const loglib::LogConfiguration::HighlightRule &rule) const
{
    if (rule.columnKeys.empty())
    {
        return -1;
    }
    for (std::size_t i = 0; i < mColumns.size(); ++i)
    {
        const auto &keys = mColumns[i].keys;
        const bool allPresent = std::ranges::all_of(rule.columnKeys, [&keys](const std::string &k) {
            return std::ranges::find(keys, k) != keys.end();
        });
        if (allPresent)
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void HighlightRulesEditor::RepopulateColumnCombo()
{
    if (mColumnCombo == nullptr)
    {
        return;
    }
    const QSignalBlocker block(mColumnCombo);
    mColumnCombo->clear();
    mColumnCombo->addItem(tr("(none)"), QVariant::fromValue(QStringList{}));
    for (const auto &col : mColumns)
    {
        QStringList keys;
        keys.reserve(static_cast<qsizetype>(col.keys.size()));
        for (const auto &k : col.keys)
        {
            keys.push_back(QString::fromStdString(k));
        }
        mColumnCombo->addItem(QString::fromStdString(col.header), QVariant::fromValue(keys));
    }
}

void HighlightRulesEditor::SetColumns(std::vector<loglib::LogConfiguration::Column> columns)
{
    mColumns = std::move(columns);
    RepopulateColumnCombo();
    RebuildList(mCurrentRow);
}

void HighlightRulesEditor::SetRules(std::vector<loglib::LogConfiguration::HighlightRule> rules)
{
    if (!ConfirmDiscardEdits())
    {
        return;
    }
    mLocalRules = std::move(rules);
    mBaseline = mLocalRules;
    RebuildList(mLocalRules.empty() ? -1 : 0);
    UpdateListButtons();
    UpdateFormEnabled();
}

void HighlightRulesEditor::RebuildList(int selectRow)
{
    if (mListWidget == nullptr)
    {
        return;
    }
    const int keep = (selectRow >= 0) ? selectRow : mCurrentRow;
    const QSignalBlocker block(mListWidget);
    mListWidget->clear();
    for (std::size_t i = 0; i < mLocalRules.size(); ++i)
    {
        const auto &rule = mLocalRules[i];
        auto *item = new QListWidgetItem(FormatListIcon(rule, SwatchIconSizePx()), FormatListLabel(rule));
        item->setToolTip(FormatListLabel(rule));
        mListWidget->addItem(item);
    }
    const int rowCount = mListWidget->count();
    if (rowCount == 0)
    {
        mCurrentRow = -1;
        LoadIntoForm(-1);
        return;
    }
    const int selected = std::clamp(keep, 0, rowCount - 1);
    mListWidget->setCurrentRow(selected);
    // setCurrentRow triggers currentRowChanged -> OnSelectionChanged
    // which calls LoadIntoForm; but currentRowChanged only fires
    // when the row actually changes, so force it here.
    if (mCurrentRow == selected)
    {
        LoadIntoForm(selected);
    }
    else
    {
        mCurrentRow = selected;
        LoadIntoForm(selected);
    }
}

void HighlightRulesEditor::OnSelectionChanged()
{
    if (mListWidget == nullptr)
    {
        return;
    }
    const int row = mListWidget->currentRow();
    mCurrentRow = row;
    LoadIntoForm(row);
    UpdateListButtons();
}

void HighlightRulesEditor::LoadIntoForm(int row)
{
    mSuppressDirtySignals = true;
    const auto guard = qScopeGuard([this]() { mSuppressDirtySignals = false; });

    if (row < 0 || row >= static_cast<int>(mLocalRules.size()))
    {
        mNameEdit->clear();
        mEnabledCheck->setChecked(true);
        mColumnCombo->setCurrentIndex(0);
        mTypeCombo->setCurrentIndex(0);
        mMatchStack->setCurrentIndex(0);
        mStringMatchCombo->setCurrentIndex(1); // Contains -- friendliest default.
        mStringNeedleEdit->clear();
        mNumberMinEnabled->setChecked(false);
        mNumberMinValue->setValue(0);
        mNumberMinValue->setEnabled(false);
        mNumberMaxEnabled->setChecked(false);
        mNumberMaxValue->setValue(0);
        mNumberMaxValue->setEnabled(false);
        mBoolIncludeTrue->setChecked(false);
        mBoolIncludeFalse->setChecked(false);
        mSelectedForegroundIndex = 0;
        mSelectedBackgroundIndex = 0;
        mBoldCheck->setChecked(false);
        mItalicCheck->setChecked(false);
        UpdateFormEnabled();
        return;
    }

    const auto &rule = mLocalRules[row];
    mNameEdit->setText(QString::fromStdString(rule.name));
    mEnabledCheck->setChecked(rule.enabled);

    // Column combo: match by keys.
    int comboIndex = 0; // "(none)" slot.
    for (std::size_t i = 0; i < mColumns.size(); ++i)
    {
        const auto &keys = mColumns[i].keys;
        if (rule.columnKeys.size() == keys.size() &&
            std::equal(rule.columnKeys.begin(), rule.columnKeys.end(), keys.begin()))
        {
            comboIndex = static_cast<int>(i) + 1;
            break;
        }
    }
    mColumnCombo->setCurrentIndex(comboIndex);

    const int typeIdx = static_cast<int>(rule.type);
    mTypeCombo->setCurrentIndex(typeIdx);
    mMatchStack->setCurrentIndex(typeIdx);

    // Type-specific fields.
    using RT = loglib::LogConfiguration::HighlightRule::Type;
    switch (rule.type)
    {
    case RT::String:
    {
        const int matchIdx = rule.matchType.has_value() ? static_cast<int>(*rule.matchType) : 1;
        mStringMatchCombo->setCurrentIndex(matchIdx);
        mStringNeedleEdit->setText(rule.filterString.has_value() ? QString::fromStdString(*rule.filterString) : QString{});
        break;
    }
    case RT::Number:
    {
        mNumberMinEnabled->setChecked(rule.filterMinValue.has_value());
        mNumberMinValue->setEnabled(rule.filterMinValue.has_value());
        mNumberMinValue->setValue(rule.filterMinValue.value_or(0.0));
        mNumberMaxEnabled->setChecked(rule.filterMaxValue.has_value());
        mNumberMaxValue->setEnabled(rule.filterMaxValue.has_value());
        mNumberMaxValue->setValue(rule.filterMaxValue.value_or(0.0));
        break;
    }
    case RT::Boolean:
    {
        bool includeTrue = false;
        bool includeFalse = false;
        for (const auto &v : rule.filterValues)
        {
            std::string lower = v;
            std::ranges::transform(lower, lower.begin(), [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });
            if (lower == "true")
            {
                includeTrue = true;
            }
            else if (lower == "false")
            {
                includeFalse = true;
            }
        }
        mBoolIncludeTrue->setChecked(includeTrue);
        mBoolIncludeFalse->setChecked(includeFalse);
        break;
    }
    case RT::Time:
    case RT::Enumeration:
    default:
        break;
    }

    mSelectedForegroundIndex = rule.foregroundIndex;
    mSelectedBackgroundIndex = rule.backgroundIndex;
    if (mTheme != nullptr)
    {
        const QBrush fgBg = mTheme->HighlightBrushFor(rule.foregroundIndex, Qt::BackgroundRole);
        const QBrush fgFg = mTheme->HighlightBrushFor(rule.foregroundIndex, Qt::ForegroundRole);
        mForegroundButton->setIcon(
            rule.foregroundIndex == 0 ? RenderSwatchIcon(QBrush{}, QBrush(Qt::gray), SwatchIconSizePx(), true)
                                       : RenderSwatchIcon(fgBg, fgFg, SwatchIconSizePx(), true)
        );
        const QBrush bgBg = mTheme->HighlightBrushFor(rule.backgroundIndex, Qt::BackgroundRole);
        const QBrush bgFg = mTheme->HighlightBrushFor(rule.backgroundIndex, Qt::ForegroundRole);
        mBackgroundButton->setIcon(
            rule.backgroundIndex == 0 ? RenderSwatchIcon(QBrush{}, QBrush(Qt::gray), SwatchIconSizePx(), true)
                                       : RenderSwatchIcon(bgBg, bgFg, SwatchIconSizePx(), true)
        );
    }
    mBoldCheck->setChecked(rule.bold);
    mItalicCheck->setChecked(rule.italic);

    UpdateFormEnabled();
}

void HighlightRulesEditor::UpdateFormEnabled()
{
    const bool haveSelection = mCurrentRow >= 0 && mCurrentRow < static_cast<int>(mLocalRules.size());
    for (QWidget *w : {static_cast<QWidget *>(mNameEdit),
                       static_cast<QWidget *>(mEnabledCheck),
                       static_cast<QWidget *>(mColumnCombo),
                       static_cast<QWidget *>(mTypeCombo),
                       static_cast<QWidget *>(mForegroundButton),
                       static_cast<QWidget *>(mBackgroundButton),
                       static_cast<QWidget *>(mBoldCheck),
                       static_cast<QWidget *>(mItalicCheck),
                       static_cast<QWidget *>(mMatchStack)})
    {
        if (w != nullptr)
        {
            w->setEnabled(haveSelection);
        }
    }
    // Read-only pane (Time / Enumeration in v1) grays the whole
    // stack irrespective of `haveSelection`.
    if (haveSelection)
    {
        const auto ruleType = mLocalRules[static_cast<std::size_t>(mCurrentRow)].type;
        const bool isReadOnly = (ruleType == loglib::LogConfiguration::HighlightRule::Type::Time ||
                                 ruleType == loglib::LogConfiguration::HighlightRule::Type::Enumeration);
        if (isReadOnly)
        {
            mMatchStack->setEnabled(false);
        }
    }
}

void HighlightRulesEditor::UpdateListButtons()
{
    const int row = mCurrentRow;
    const int total = static_cast<int>(mLocalRules.size());
    const bool haveSelection = row >= 0 && row < total;
    mDuplicateButton->setEnabled(haveSelection);
    mDeleteButton->setEnabled(haveSelection);
    mMoveUpButton->setEnabled(haveSelection && row > 0);
    mMoveDownButton->setEnabled(haveSelection && row < total - 1);
    mSaveButton->setEnabled(IsDirty());
    mRevertButton->setEnabled(IsDirty());
}

bool HighlightRulesEditor::IsDirty() const
{
    return mLocalRules != mBaseline;
}

void HighlightRulesEditor::MarkDirty()
{
    UpdateListButtons();
}

void HighlightRulesEditor::OnFieldEdited()
{
    if (mSuppressDirtySignals)
    {
        return;
    }
    GatherForm();
    RebuildList(mCurrentRow);
    MarkDirty();
}

void HighlightRulesEditor::OnColumnChanged()
{
    if (mSuppressDirtySignals)
    {
        return;
    }
    // Column type may gate the match-type combo; leave the current
    // rule.type unchanged (user can still type-mismatch a rule but
    // it will silently render inert). v1 keeps this simple.
    GatherForm();
    RebuildList(mCurrentRow);
    MarkDirty();
}

void HighlightRulesEditor::OnTypeChanged()
{
    if (mSuppressDirtySignals)
    {
        return;
    }
    mMatchStack->setCurrentIndex(mTypeCombo->currentIndex());
    UpdateFormEnabled();
    GatherForm();
    RebuildList(mCurrentRow);
    MarkDirty();
}

void HighlightRulesEditor::GatherForm()
{
    if (mCurrentRow < 0 || mCurrentRow >= static_cast<int>(mLocalRules.size()))
    {
        return;
    }
    auto &rule = mLocalRules[static_cast<std::size_t>(mCurrentRow)];
    rule.name = mNameEdit->text().toStdString();
    rule.enabled = mEnabledCheck->isChecked();

    // Column
    const QVariant columnData = mColumnCombo->currentData();
    rule.columnKeys.clear();
    if (columnData.canConvert<QStringList>())
    {
        for (const QString &k : columnData.toStringList())
        {
            rule.columnKeys.push_back(k.toStdString());
        }
    }

    rule.type = static_cast<loglib::LogConfiguration::HighlightRule::Type>(mTypeCombo->currentIndex());

    // Reset value-holders each time; type switch means old fields
    // are stale.
    rule.matchType.reset();
    rule.filterString.reset();
    rule.filterBegin.reset();
    rule.filterEnd.reset();
    rule.filterMinValue.reset();
    rule.filterMaxValue.reset();
    rule.filterValues.clear();

    using RT = loglib::LogConfiguration::HighlightRule::Type;
    switch (rule.type)
    {
    case RT::String:
        rule.matchType = static_cast<loglib::LogConfiguration::HighlightRule::Match>(mStringMatchCombo->currentIndex());
        rule.filterString = mStringNeedleEdit->text().toStdString();
        break;
    case RT::Number:
        if (mNumberMinEnabled->isChecked())
        {
            rule.filterMinValue = mNumberMinValue->value();
        }
        if (mNumberMaxEnabled->isChecked())
        {
            rule.filterMaxValue = mNumberMaxValue->value();
        }
        break;
    case RT::Boolean:
        if (mBoolIncludeTrue->isChecked())
        {
            rule.filterValues.emplace_back("true");
        }
        if (mBoolIncludeFalse->isChecked())
        {
            rule.filterValues.emplace_back("false");
        }
        break;
    case RT::Time:
    case RT::Enumeration:
    default:
        // Read-only in v1: leave the value-holders empty (matches
        // the reset above) so a user accidentally re-typing a
        // Time/Enum rule doesn't scramble hand-authored config
        // values. Rules stay inert until fixed via the config file.
        break;
    }

    rule.foregroundIndex = mSelectedForegroundIndex;
    rule.backgroundIndex = mSelectedBackgroundIndex;
    rule.bold = mBoldCheck->isChecked();
    rule.italic = mItalicCheck->isChecked();
}

void HighlightRulesEditor::OnNewClicked()
{
    loglib::LogConfiguration::HighlightRule rule;
    rule.name = "New rule";
    rule.enabled = true;
    rule.type = loglib::LogConfiguration::HighlightRule::Type::String;
    rule.matchType = loglib::LogConfiguration::HighlightRule::Match::Contains;
    rule.filterString = std::string{};
    if (!mColumns.empty())
    {
        rule.columnKeys = mColumns[0].keys;
    }
    mLocalRules.push_back(std::move(rule));
    RebuildList(static_cast<int>(mLocalRules.size()) - 1);
    UpdateListButtons();
    MarkDirty();
}

void HighlightRulesEditor::OnDuplicateClicked()
{
    if (mCurrentRow < 0 || mCurrentRow >= static_cast<int>(mLocalRules.size()))
    {
        return;
    }
    loglib::LogConfiguration::HighlightRule copy = mLocalRules[static_cast<std::size_t>(mCurrentRow)];
    copy.name += " (copy)";
    mLocalRules.insert(mLocalRules.begin() + mCurrentRow + 1, std::move(copy));
    RebuildList(mCurrentRow + 1);
    UpdateListButtons();
    MarkDirty();
}

void HighlightRulesEditor::OnDeleteClicked()
{
    if (mCurrentRow < 0 || mCurrentRow >= static_cast<int>(mLocalRules.size()))
    {
        return;
    }
    mLocalRules.erase(mLocalRules.begin() + mCurrentRow);
    const int newRow = mLocalRules.empty() ? -1
                                            : std::min(mCurrentRow, static_cast<int>(mLocalRules.size()) - 1);
    RebuildList(newRow);
    UpdateListButtons();
    MarkDirty();
}

void HighlightRulesEditor::OnMoveUpClicked()
{
    if (mCurrentRow <= 0 || mCurrentRow >= static_cast<int>(mLocalRules.size()))
    {
        return;
    }
    std::swap(mLocalRules[mCurrentRow], mLocalRules[mCurrentRow - 1]);
    RebuildList(mCurrentRow - 1);
    UpdateListButtons();
    MarkDirty();
}

void HighlightRulesEditor::OnMoveDownClicked()
{
    if (mCurrentRow < 0 || mCurrentRow >= static_cast<int>(mLocalRules.size()) - 1)
    {
        return;
    }
    std::swap(mLocalRules[mCurrentRow], mLocalRules[mCurrentRow + 1]);
    RebuildList(mCurrentRow + 1);
    UpdateListButtons();
    MarkDirty();
}

void HighlightRulesEditor::OnSaveClicked()
{
    if (!IsDirty())
    {
        return;
    }
    mBaseline = mLocalRules;
    emit rulesSaved(mLocalRules);
    UpdateListButtons();
    ShowStatus(tr("Highlight rules saved."), false);
}

void HighlightRulesEditor::OnRevertClicked()
{
    if (!IsDirty())
    {
        return;
    }
    mLocalRules = mBaseline;
    RebuildList(mLocalRules.empty() ? -1 : std::min(mCurrentRow, static_cast<int>(mLocalRules.size()) - 1));
    UpdateListButtons();
    ShowStatus(tr("Reverted to saved state."), false);
}

void HighlightRulesEditor::ShowStatus(const QString &message, bool isError)
{
    mStatusLabel->setText(message);
    mStatusLabel->setStyleSheet(isError ? QStringLiteral("color: #B91C1C;") : QString{});
    mStatusClearTimer->start(STATUS_CLEAR_MS);
}

bool HighlightRulesEditor::ConfirmDiscardEdits()
{
    if (!IsDirty())
    {
        return true;
    }
    const QMessageBox::StandardButton result = QMessageBox::warning(
        this,
        tr("Discard unsaved changes?"),
        tr("The rule list has unsaved edits. Discard them?"),
        QMessageBox::Discard | QMessageBox::Cancel,
        QMessageBox::Cancel
    );
    return result == QMessageBox::Discard;
}

void HighlightRulesEditor::closeEvent(QCloseEvent *event)
{
    if (!ConfirmDiscardEdits())
    {
        event->ignore();
        return;
    }
    QWidget::closeEvent(event);
}
